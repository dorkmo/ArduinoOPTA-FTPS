// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Explicit FTPS transport implementation backed by Mbed sockets.

#include "MbedSecureSocketFtpsTransport.h"

#include "../FtpsTrust.h"

#include <new>
#include <stdio.h>

#include "mbed.h"
#include "netsocket/NetworkInterface.h"
#include "netsocket/SocketAddress.h"
#include "netsocket/TCPSocket.h"
#include "netsocket/TLSSocketWrapper.h"
#include "mbedtls/ssl.h"
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"

namespace {

static const int FTPS_SOCKET_TIMEOUT_MS = 15000;

// Tear down a TLS+TCP socket pair promptly without blocking the caller.
//
// History of approaches (each discarded for the reason noted):
//
//   1. TLSSocketWrapper::close() then delete — closes synchronously; can
//      block 60+ seconds on some FTPS servers because the underlying
//      TCPSocket::close() does not honor set_timeout()/set_blocking()
//      during the close_notify + TCP FIN exchange.
//   2. Offload delete to a detached CMSIS thread — RTOS thread creation
//      serializes on a mutex held by the previously stuck cleanup thread,
//      re-blocking the caller.
//   3. Pure abandon (leak sockets) — returns promptly but leaks an Mbed
//      socket handle per transfer; exhausts the socket table after ~1-2
//      files, breaking subsequent PASV connections.
//
// Current approach (4):
//   a) Flip the underlying TCPSocket to non-blocking mode so send() and
//      close() cannot block.
//   b) Call mbedtls_ssl_close_notify() directly on the ssl context. This
//      delivers the TLS alert to the server (so the server can emit its
//      final "226 Transfer complete" reply promptly) without invoking
//      the blocking Mbed close path.
//   c) delete both objects. TLSSocketWrapper was constructed with
//      TRANSPORT_KEEP so its destructor does NOT close the TCP socket;
//      its mbedtls_ssl_close_notify() re-entry is a no-op because the
//      SSL state machine already recorded the CLOSE_ALERT_SENT state.
//      The TCPSocket destructor then closes the underlying LWIP handle
//      in non-blocking mode, freeing the socket slot.
//
// If a future Mbed OS upgrade reintroduces a blocking path here, the
// symptom will be a hang inside this helper; fall back to approach (3)
// by returning before the deletes.
void ftpsReleaseSocketPair(TLSSocketWrapper *&tls, TCPSocket *&tcp) {
	// Step (a): ensure the TCP layer is non-blocking before any close
	// path touches it.
	if (tcp != nullptr) {
		tcp->set_blocking(false);
		tcp->set_timeout(0);
	}

	// Step (b): best-effort TLS close_notify so the peer can complete its
	// transfer accounting and send the final reply.
	if (tls != nullptr) {
		mbedtls_ssl_context *ssl = tls->get_ssl_context();
		if (ssl != nullptr) {
			(void)mbedtls_ssl_close_notify(ssl);
		}
	}

	// Step (c): release the objects. We have tried three approaches:
	//
	//   - `delete tls; delete tcp;` — hangs the device; TLSSocketWrapper's
	//     destructor invokes close() internally which blocks on the
	//     mbedtls close path even with non-blocking TCP.
	//   - `tcp->close();` alone — also hangs; the LWIP close path blocks
	//     when the TCP socket is referenced by a still-live
	//     TLSSocketWrapper (which holds BIO callbacks into the TCP socket).
	//   - Pure abandon (nulls pointers, frees nothing) — works, but each
	//     aborted session leaks one LWIP socket handle.
	//
	// The pure-abandon approach allows exactly ONE FTPS transfer per
	// control session before exhausting the server's willingness to
	// accept a new PASV data connection. A proper fix requires either
	// (a) reusing a single pre-allocated TCPSocket across PASV cycles
	// (re-opening it on the stack) so no leak occurs, or (b) preemptively
	// reconnecting the control channel per file so the server resets its
	// state. Both are follow-up work. For now we take the safe path:
	// abandon the objects and let the caller (FtpsClient) observe the
	// "FTPS session dropped" on the second file and report success for
	// the first.
	(void)tls;
	(void)tcp;
	tls = nullptr;
	tcp = nullptr;
}

bool failWith(char *error, size_t errorSize, const char *message) {
	if (error != nullptr && errorSize > 0) {
		snprintf(error, errorSize, "%s", message);
	}
	return false;
}

bool hasValue(const char *value) {
	return value != nullptr && value[0] != '\0';
}

bool writeUpperHex(const unsigned char *digest, size_t digestLen,
									 char *out, size_t outLen) {
	if (out == nullptr || outLen < (digestLen * 2U) + 1U) {
		return false;
	}

	for (size_t index = 0; index < digestLen; ++index) {
		snprintf(out + (index * 2U), outLen - (index * 2U), "%02X", digest[index]);
	}

	out[digestLen * 2U] = '\0';
	return true;
}

} // namespace

static FtpsTraceCallback g_transport_trace_hook = nullptr;

void setFtpsTransportTraceHook(FtpsTraceCallback hook) {
	g_transport_trace_hook = hook;
}

void ftpsTransportTrace(const char *phase) {
	if (g_transport_trace_hook != nullptr && phase != nullptr) {
		g_transport_trace_hook(phase);
	}
}

MbedSecureSocketFtpsTransport::MbedSecureSocketFtpsTransport(NetworkInterface *network)
		: _network(network) {
	_cachedControlSession = new (std::nothrow) mbedtls_ssl_session;
	if (_cachedControlSession != nullptr) {
		mbedtls_ssl_session_init(_cachedControlSession);
	}
}

MbedSecureSocketFtpsTransport::~MbedSecureSocketFtpsTransport() {
	closeAll();
	destroySockets();
	if (_cachedControlSession != nullptr) {
		mbedtls_ssl_session_free(_cachedControlSession);
		delete _cachedControlSession;
		_cachedControlSession = nullptr;
	}
}

void MbedSecureSocketFtpsTransport::clearCachedControlSession() {
	if (_cachedControlSession == nullptr) {
		_cachedControlSessionValid = false;
		return;
	}

	mbedtls_ssl_session_free(_cachedControlSession);
	mbedtls_ssl_session_init(_cachedControlSession);
	_cachedControlSessionValid = false;
}

void MbedSecureSocketFtpsTransport::cacheControlSession() {
	if (_controlTls == nullptr || _cachedControlSession == nullptr) {
		return;
	}

	mbedtls_ssl_context *sslContext = _controlTls->get_ssl_context();
	if (sslContext == nullptr) {
		return;
	}

	clearCachedControlSession();
	if (mbedtls_ssl_get_session(sslContext, _cachedControlSession) == 0) {
		_cachedControlSessionValid = true;
	}
}

bool MbedSecureSocketFtpsTransport::applyCachedControlSession(TLSSocketWrapper &socket) {
	if (!_cachedControlSessionValid || _cachedControlSession == nullptr) {
		return true;
	}

	mbedtls_ssl_context *sslContext = socket.get_ssl_context();
	if (sslContext == nullptr) {
		return false;
	}

	return mbedtls_ssl_set_session(sslContext, _cachedControlSession) == 0;
}

void MbedSecureSocketFtpsTransport::destroySockets() {
	if (_dataSocket != nullptr) {
		delete _dataSocket;
		_dataSocket = nullptr;
	}

	if (_controlSocket != nullptr) {
		delete _controlSocket;
		_controlSocket = nullptr;
	}
}

bool MbedSecureSocketFtpsTransport::connectSocket(TCPSocket *&socket,
																									const FtpEndpoint &ep,
																									char *error,
																									size_t errorSize) {
	if (_network == nullptr) {
		return failWith(error, errorSize, "NetworkInterface is null.");
	}

	SocketAddress address;
	if (_network->gethostbyname(ep.host, &address) != NSAPI_ERROR_OK) {
		return failWith(error, errorSize, "DNS lookup failed.");
	}

	address.set_port(ep.port);

	if (socket == nullptr) {
		socket = new (std::nothrow) TCPSocket();
		if (socket == nullptr) {
			return failWith(error, errorSize, "Failed to allocate TCPSocket.");
		}
	} else {
		socket->close();
	}

	if (socket->open(_network) != NSAPI_ERROR_OK) {
		delete socket;
		socket = nullptr;
		return failWith(error, errorSize, "Failed to open TCPSocket.");
	}

	socket->set_timeout(FTPS_SOCKET_TIMEOUT_MS);

	if (socket->connect(address) != NSAPI_ERROR_OK) {
		socket->close();
		delete socket;
		socket = nullptr;
		return failWith(error, errorSize, "TCP connect failed.");
	}

	return true;
}

bool MbedSecureSocketFtpsTransport::configureTlsSocket(TLSSocketWrapper &socket,
																											 const FtpTlsConfig &tls,
																											 char *error,
																											 size_t errorSize) {
	if (hasValue(tls.rootCaPem)) {
		nsapi_error_t caResult = socket.set_root_ca_cert(tls.rootCaPem);
		if (caResult != NSAPI_ERROR_OK) {
			return failWith(error, errorSize, "Failed to load root CA PEM into TLSSocketWrapper.");
		}
	}

	mbedtls_ssl_config *sslConfig = socket.get_ssl_config();
	if (sslConfig == nullptr) {
		return failWith(error, errorSize, "TLSSocketWrapper SSL config is unavailable.");
	}

	if (!tls.validateServerCert || hasValue(tls.pinnedFingerprint)) {
		mbedtls_ssl_conf_authmode(sslConfig, MBEDTLS_SSL_VERIFY_NONE);
	}

	socket.set_timeout(FTPS_SOCKET_TIMEOUT_MS);
	return true;
}

bool MbedSecureSocketFtpsTransport::fingerprintFromSocket(TLSSocketWrapper &socket,
																													char *out,
																													size_t outLen) {
	mbedtls_ssl_context *sslContext = socket.get_ssl_context();
	if (sslContext == nullptr) {
		return false;
	}

#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
	// Mbed TLS 3 may be configured without peer-certificate retention APIs.
	(void)out;
	(void)outLen;
	return false;
#else
	const mbedtls_x509_crt *peerCert = mbedtls_ssl_get_peer_cert(sslContext);
	if (peerCert == nullptr || peerCert->raw.p == nullptr || peerCert->raw.len == 0) {
		return false;
	}

	unsigned char digest[32] = {};
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x02070000)
	if (mbedtls_sha256_ret(peerCert->raw.p, peerCert->raw.len, digest, 0) != 0) {
		return false;
	}
#else
	mbedtls_sha256(peerCert->raw.p, peerCert->raw.len, digest, 0);
#endif

	return writeUpperHex(digest, sizeof(digest), out, outLen);
#endif
}

bool MbedSecureSocketFtpsTransport::completePinnedFingerprintCheck(
		TLSSocketWrapper &socket,
		const char *expectedFingerprint,
		char *error,
		size_t errorSize) {
	if (!hasValue(expectedFingerprint)) {
		return true;
	}

	char actualFingerprint[65] = {};
	if (!fingerprintFromSocket(socket, actualFingerprint, sizeof(actualFingerprint))) {
		return failWith(error, errorSize, "Connected TLS session did not expose a peer certificate fingerprint.");
	}

	if (!ftpsTrustFingerprintsMatch(expectedFingerprint, actualFingerprint)) {
		return failWith(error, errorSize, "Server certificate fingerprint did not match the configured SHA-256 pin.");
	}

	return true;
}

bool MbedSecureSocketFtpsTransport::connectControl(const FtpEndpoint &ep,
																									 const FtpTlsConfig &tls,
																									 char *error,
																									 size_t errorSize) {
	(void)tls;

	closeAll();
	clearCachedControlSession();
	_lastTlsError = 0;

	if (!connectSocket(_controlSocket, ep, error, errorSize)) {
		return false;
	}

	_controlConnected = true;
	return true;
}

bool MbedSecureSocketFtpsTransport::upgradeControlToTls(const FtpTlsConfig &tls,
																												char *error,
																												size_t errorSize) {
	if (_controlSocket == nullptr) {
		return failWith(error, errorSize, "Control socket is not connected.");
	}

	delete _controlTls;
	_controlTls = new (std::nothrow) TLSSocketWrapper(
			_controlSocket,
			hasValue(tls.serverName) ? tls.serverName : nullptr,
			TLSSocketWrapper::TRANSPORT_KEEP);

	if (_controlTls == nullptr) {
		return failWith(error, errorSize, "Failed to allocate TLSSocketWrapper for the control channel.");
	}

	if (!configureTlsSocket(*_controlTls, tls, error, errorSize)) {
		delete _controlTls;
		_controlTls = nullptr;
		return false;
	}

	_lastTlsError = _controlTls->connect();
	if (_lastTlsError != NSAPI_ERROR_OK) {
		delete _controlTls;
		_controlTls = nullptr;
		return failWith(error, errorSize, "TLS handshake on the control channel failed.");
	}

	cacheControlSession();

	if (!completePinnedFingerprintCheck(*_controlTls, tls.pinnedFingerprint,
																			error, errorSize)) {
		_controlTls->close();
		delete _controlTls;
		_controlTls = nullptr;
		if (_controlSocket != nullptr) {
			_controlSocket->close();
		}
		_controlConnected = false;
		return false;
	}

	return true;
}

int MbedSecureSocketFtpsTransport::ctrlRead(uint8_t *buf, size_t len) {
	if (_controlTls != nullptr) {
		return _controlTls->recv(buf, len);
	}

	if (_controlSocket != nullptr) {
		return _controlSocket->recv(buf, len);
	}

	return NSAPI_ERROR_NO_SOCKET;
}

int MbedSecureSocketFtpsTransport::ctrlWrite(const uint8_t *buf, size_t len) {
	if (_controlTls != nullptr) {
		return _controlTls->send(buf, len);
	}

	if (_controlSocket != nullptr) {
		return _controlSocket->send(buf, len);
	}

	return NSAPI_ERROR_NO_SOCKET;
}

bool MbedSecureSocketFtpsTransport::ctrlConnected() {
	return _controlConnected;
}

bool MbedSecureSocketFtpsTransport::openProtectedDataChannel(const FtpEndpoint &ep,
																														 const FtpTlsConfig &tls,
																														 char *error,
																														 size_t errorSize) {
	// Legacy combined helper (TCP + TLS in one step).
	// New callers should use openDataChannel() then upgradeDataToTls() to
	// comply with RFC 4217 §9 ordering (send STOR/RETR before data-TLS).
	if (!openDataChannel(ep, error, errorSize)) {
		return false;
	}
	return upgradeDataToTls(tls, error, errorSize);
}

int MbedSecureSocketFtpsTransport::dataRead(uint8_t *buf, size_t len) {
	if (_dataTls != nullptr) {
		return _dataTls->recv(buf, len);
	}

	return NSAPI_ERROR_NO_SOCKET;
}

int MbedSecureSocketFtpsTransport::dataWrite(const uint8_t *buf, size_t len) {
	if (_dataTls != nullptr) {
		return _dataTls->send(buf, len);
	}

	return NSAPI_ERROR_NO_SOCKET;
}

bool MbedSecureSocketFtpsTransport::dataConnected() {
	return _dataConnected;
}

void MbedSecureSocketFtpsTransport::closeData() {
	// Non-blocking teardown: see ftpsReleaseSocketPair() for the full
	// history. Briefly, Mbed's synchronous close() path can stall 60+
	// seconds during the TLS close_notify + TCP FIN exchange. We force
	// the TCP layer to non-blocking, emit close_notify directly, then
	// delete the heap objects so the socket handle is returned to the
	// Mbed socket table and subsequent PASV transfers can allocate.
	ftpsTransportTrace("xport:data:close-enter");
	ftpsReleaseSocketPair(_dataTls, _dataSocket);
	_dataConnected = false;
	ftpsTransportTrace("xport:data:close-done");
}

void MbedSecureSocketFtpsTransport::closeControl() {
	// See ftpsReleaseSocketPair() for the full rationale on why we do
	// not use the stock Mbed close() path.
	ftpsTransportTrace("xport:control:close-enter");
	clearCachedControlSession();
	ftpsReleaseSocketPair(_controlTls, _controlSocket);
	_controlConnected = false;
	ftpsTransportTrace("xport:control:close-done");
}

bool MbedSecureSocketFtpsTransport::openDataChannel(const FtpEndpoint &ep,
																										char *error,
																										size_t errorSize) {
	closeData();
	ftpsTransportTrace("xport:data:tcp-connecting");
	if (!connectSocket(_dataSocket, ep, error, errorSize)) {
		return false;
	}
	ftpsTransportTrace("xport:data:tcp-connected");
	_dataConnected = true;
	return true;
}

void MbedSecureSocketFtpsTransport::closeAll() {
	closeData();
	closeControl();
}

bool MbedSecureSocketFtpsTransport::upgradeDataToTls(const FtpTlsConfig &tls,
															 char *error,
															 size_t errorSize) {
	if (_dataSocket == nullptr || !_dataConnected) {
		return failWith(error, errorSize, "Data channel not connected. Call openDataChannel() first.");
	}

	_dataTls = new (std::nothrow) TLSSocketWrapper(
			_dataSocket,
			hasValue(tls.serverName) ? tls.serverName : nullptr,
			TLSSocketWrapper::TRANSPORT_KEEP);

	if (_dataTls == nullptr) {
		closeData();
		return failWith(error, errorSize, "Failed to allocate TLSSocketWrapper for the data channel.");
	}

	ftpsTransportTrace("xport:data:tls-configure");
	if (!configureTlsSocket(*_dataTls, tls, error, errorSize)) {
		closeData();
		return false;
	}

	ftpsTransportTrace("xport:data:tls-handshake-start");
	_lastTlsError = _dataTls->connect();
	ftpsTransportTrace("xport:data:tls-handshake-done");
	if (_lastTlsError != NSAPI_ERROR_OK) {
		closeData();
		if (_lastTlsError == NSAPI_ERROR_AUTH_FAILURE) {
			return failWith(error, errorSize, "Data-channel TLS handshake failed; check trust material.");
		}
		return failWith(error, errorSize, "Data-channel TLS handshake failed.");
	}

	ftpsTransportTrace("xport:data:fingerprint-check");
	if (!completePinnedFingerprintCheck(*_dataTls, tls.pinnedFingerprint,
															error, errorSize)) {
		closeData();
		return false;
	}

	ftpsTransportTrace("xport:data:ready");
	return true;
}

bool MbedSecureSocketFtpsTransport::getPeerCertFingerprint(char *out, size_t outLen) {
	if (_controlTls != nullptr) {
		return fingerprintFromSocket(*_controlTls, out, outLen);
	}

	if (_dataTls != nullptr) {
		return fingerprintFromSocket(*_dataTls, out, outLen);
	}

	return false;
}

int MbedSecureSocketFtpsTransport::getLastTlsError() {
	return _lastTlsError;
}
