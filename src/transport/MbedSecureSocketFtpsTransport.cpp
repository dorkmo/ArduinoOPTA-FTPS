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
// History (each approach failed for the reason noted):
//
//   1. TLSSocketWrapper::close() then delete — Mbed's close path ignores
//      set_timeout()/set_blocking(false); on some FTPS servers it blocks
//      60+ seconds waiting for the peer's FIN, tripping the watchdog.
//   2. Detached CMSIS cleanup thread — thread creation serialized on a
//      mutex held by the previously stuck cleanup thread, re-blocking
//      the caller.
//   3. Pure abandon (leak everything) — returns promptly but leaks one
//      LWIP netconn slot per transfer, limiting the session to exactly
//      one file before MEMP_NUM_NETCONN exhaustion.
//
// Current approach (4) — SO_LINGER=0 abortive close:
//
//   a) Set NSAPI_LINGER = {onoff=1, linger=0} on the underlying TCP
//      socket. In BSD/LWIP semantics this converts close() into an
//      immediate TCP RST (reset) instead of a graceful FIN/ACK
//      handshake, so close() does not wait for the peer.
//   b) Flip the socket to non-blocking with zero timeout as a safety
//      net in case NSAPI_LINGER is silently unsupported on this core.
//   c) Send mbedtls_ssl_close_notify() directly on the ssl context so
//      the FTPS server emits its final '226 Transfer complete' reply
//      promptly (without us routing through the blocking wrapper).
//   d) delete both heap objects. With the socket configured for
//      abortive close, TLSSocketWrapper's destructor and TCPSocket's
//      destructor should return quickly, releasing both the ~4 KB of
//      heap and the LWIP netconn slot.
//
// If this still blocks on a given Mbed/firmware combination, we fall
// back to abandon (approach 3) behind the FTPS_ABANDON_ON_CLOSE macro
// so at least the watchdog is not tripped.
//
// 2026-04-17 update: the historical "delete tls hangs" finding was
// triggered during boot-time FTP restore in setup(), before loop()
// or the watchdog were running. That code path has been removed
// (applyCachedControlSession is no longer called from upgradeDataToTls).
// At runtime, with the watchdog active and only 1-2 file transfers in
// flight, full cleanup (delete tls + delete tcp) is required because
// any leak exhausts the LWIP socket pool after just 1-2 reconnect
// cycles (verified with -3005 NO_SOCKET on file 2 data-open).
// We now default to FTPS_ABANDON_ON_CLOSE=0 (full cleanup). If this
// hangs, the 30s watchdog will recover and we revert.
#ifndef FTPS_ABANDON_ON_CLOSE
#define FTPS_ABANDON_ON_CLOSE 0
#endif

void ftpsReleaseSocketPair(TLSSocketWrapper *&tls, TCPSocket *&tcp) {
	// (a) Request abortive close (TCP RST instead of FIN/ACK handshake).
	if (tcp != nullptr) {
		struct nsapi_linger_t { int l_onoff; int l_linger; } linger = { 1, 0 };
		nsapi_error_t lingerResult = tcp->setsockopt(NSAPI_SOCKET, NSAPI_LINGER, &linger, sizeof(linger));
		if (lingerResult != NSAPI_ERROR_OK) {
			char buf[64];
			snprintf(buf, sizeof(buf), "xport:linger-unsupported:%d", (int)lingerResult);
			ftpsTransportTrace(buf);
		} else {
			ftpsTransportTrace("xport:linger-set");
		}
		// (b) Safety net: non-blocking + zero timeout.
		tcp->set_blocking(false);
		tcp->set_timeout(0);
	}

	// (c) Best-effort TLS close_notify so the peer recognises clean
	// shutdown and emits any pending reply promptly.
	if (tls != nullptr) {
		mbedtls_ssl_context *ssl = tls->get_ssl_context();
		if (ssl != nullptr) {
			(void)mbedtls_ssl_close_notify(ssl);
		}
	}

#if FTPS_ABANDON_ON_CLOSE
	// Hybrid abandon path. Three lessons from prior incidents shape this:
	//
	//   1. `delete tls` on mbed_opta 4.5.0 reliably hangs the device,
	//      sometimes silently, sometimes via watchdog reset. The TLS
	//      wrapper destructor re-enters LWIP callbacks during shutdown
	//      and is the smoking gun even with TRANSPORT_KEEP. We must
	//      leak the TLSSocketWrapper.
	//
	//   2. Pure abandon (`tls = nullptr; tcp = nullptr;`) leaks the
	//      LWIP socket. After ~2 connect cycles the LWIP socket pool
	//      (~8 entries) is exhausted and every subsequent connect
	//      silently fails. Observed in per-file reconnect tests.
	//
	//   3. `tcp->close()` alone (without delete) is NOT sufficient —
	//      the C++ TCPSocket retains an LWIP file descriptor reservation
	//      that only the destructor releases. After ~2 cycles the FD
	//      table fills and connect() fails again. Observed live with
	//      proc=2 failed=1 third-file failure at tcp-connecting.
	//
	// Therefore: explicitly close() AND delete the TCPSocket (its
	// destructor is benign on this platform), but still leak the
	// TLSSocketWrapper to dodge the dangerous mbedtls cleanup. Each
	// abandoned cycle costs ~10 KB of permanent heap, but releases the
	// LWIP socket and FD so subsequent reconnects work.
	if (tcp != nullptr) {
		(void)tcp->close();
		delete tcp;
	}
	(void)tls;
	tls = nullptr;
	tcp = nullptr;
#else
	// (d) Full cleanup path. New ordering as of 2026-04-17:
	//   - Close the TCP socket FIRST (with linger=0 + non-blocking
	//     already set above). Any subsequent BIO callbacks from the
	//     TLS destructor will fail immediately rather than block.
	//   - Then delete tls. Its destructor may try to send close_notify
	//     or do other shutdown via the BIO; with TCP already closed
	//     these calls return errors fast.
	//   - Then delete tcp to release the LWIP socket back to the pool.
	// Fine-grained traces so we can see exactly which step (if any)
	// hangs the watchdog.
	if (tcp != nullptr) {
		ftpsTransportTrace("xport:cleanup:tcp-close");
		(void)tcp->close();
		ftpsTransportTrace("xport:cleanup:tcp-closed");
	}
	if (tls != nullptr) {
		ftpsTransportTrace("xport:cleanup:tls-delete");
		delete tls;
		tls = nullptr;
		ftpsTransportTrace("xport:cleanup:tls-deleted");
	}
	if (tcp != nullptr) {
		ftpsTransportTrace("xport:cleanup:tcp-delete");
		delete tcp;
		tcp = nullptr;
		ftpsTransportTrace("xport:cleanup:tcp-deleted");
	}
#endif
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

	nsapi_error_t openResult = socket->open(_network);
	if (openResult != NSAPI_ERROR_OK) {
		_lastNsapiError = (int)openResult;
		char buf[64];
		snprintf(buf, sizeof(buf), "xport:open-failed:%d", (int)openResult);
		ftpsTransportTrace(buf);
		delete socket;
		socket = nullptr;
		char emsg[80];
		snprintf(emsg, sizeof(emsg), "Failed to open TCPSocket (nsapi=%d).", (int)openResult);
		return failWith(error, errorSize, emsg);
	}

	socket->set_timeout(FTPS_SOCKET_TIMEOUT_MS);

	nsapi_error_t connResult = socket->connect(address);
	if (connResult != NSAPI_ERROR_OK) {
		_lastNsapiError = (int)connResult;
		char buf[64];
		snprintf(buf, sizeof(buf), "xport:connect-failed:%d", (int)connResult);
		ftpsTransportTrace(buf);
		socket->close();
		delete socket;
		socket = nullptr;
		char emsg[80];
		snprintf(emsg, sizeof(emsg), "TCP connect failed (nsapi=%d).", (int)connResult);
		return failWith(error, errorSize, emsg);
	}

	_lastNsapiError = 0;
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
		// Use the non-blocking release helper — direct delete would
		// re-enter Mbed's blocking close path on some cores.
		ftpsReleaseSocketPair(_controlTls, _controlSocket);
		_controlConnected = false;
		return failWith(error, errorSize, "TLS handshake on the control channel failed.");
	}

	cacheControlSession();

	if (!completePinnedFingerprintCheck(*_controlTls, tls.pinnedFingerprint,
																				error, errorSize)) {
		// Fingerprint mismatch: release the socket pair and report failure
		// to the caller. Previously this path fell through to `return true`,
		// which silently handed an already-torn-down transport back to the
		// FTPS client — the next ctrlWrite() then failed with a confusing
		// "Failed to write FTP command" error at PBSZ.
		ftpsReleaseSocketPair(_controlTls, _controlSocket);
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

	// NOTE: applyCachedControlSession() was previously called here per
	// GPT-5.4 finding #4 (TLS session resumption on data channel). It is
	// disabled because it reliably hangs setup() during boot-time FTP
	// restore on Opta + mbed_opta 4.5.0, before loop()/watchdog can start.
	// The per-store reconnect path (FtpsClient::reconnect()) is sufficient
	// to unstick the multi-file backup without touching this code path.

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

int MbedSecureSocketFtpsTransport::getLastNsapiError() {
	return _lastNsapiError;
}
