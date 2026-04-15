// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Explicit FTPS client implementation.

#include <Arduino.h>

#include "FtpsClient.h"
#include "FtpsTrust.h"
#include "transport/IFtpsTransport.h"
#include "transport/MbedSecureSocketFtpsTransport.h"

#include "netsocket/nsapi_types.h"
#include "netsocket/SocketAddress.h"

#include <ctype.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {

static const uint32_t FTPS_REPLY_TIMEOUT_MS = 15000;
static const uint32_t FTPS_DATA_IO_TIMEOUT_MS = 15000;
static const uint32_t FTPS_FINAL_REPLY_DRAIN_TIMEOUT_MS = 5000;
static const size_t FTPS_REPLY_BUFFER_SIZE = 256;
static const size_t FTPS_MAX_PEM_SIZE = 4096;

bool hasValue(const char *value) {
  return value != nullptr && value[0] != '\0';
}

bool copyStringToBuffer(const char *input,
                        char *output,
                        size_t outputSize) {
  if (output == nullptr || outputSize == 0) {
    return false;
  }

  if (!hasValue(input)) {
    output[0] = '\0';
    return false;
  }

  int copiedLen = snprintf(output, outputSize, "%s", input);
  if (copiedLen <= 0 || static_cast<size_t>(copiedLen) >= outputSize) {
    output[0] = '\0';
    return false;
  }

  return true;
}

void clearError(char *error, size_t errorSize) {
  if (error != nullptr && errorSize > 0) {
    error[0] = '\0';
  }
}

bool failWith(FtpsError &lastError, FtpsError code,
              char *error, size_t errorSize,
              const char *message) {
  lastError = code;
  if (error != nullptr && errorSize > 0 && message != error) {
    snprintf(error, errorSize, "%s", message);
  }
  return false;
}

bool isIpLiteral(const char *value) {
  if (!hasValue(value)) {
    return false;
  }

  SocketAddress address;
  return address.set_ip_address(value) == NSAPI_ERROR_OK;
}

bool writeAll(IFtpsTransport &transport,
              bool dataChannel,
              const uint8_t *data,
              size_t length) {
  unsigned long start = millis();
  size_t offset = 0;
  while (offset < length) {
    int written = dataChannel
        ? transport.dataWrite(data + offset, length - offset)
        : transport.ctrlWrite(data + offset, length - offset);

    if (written == NSAPI_ERROR_WOULD_BLOCK) {
      if ((millis() - start) >= FTPS_DATA_IO_TIMEOUT_MS) {
        return false;
      }
      delay(5);
      continue;
    }

    if (written <= 0) {
      return false;
    }

    start = millis();
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool formatCommandWithArg(const char *verb,
                          const char *argument,
                          char *command,
                          size_t commandSize,
                          char *error,
                          size_t errorSize) {
  int commandLen = snprintf(command, commandSize, "%s %s", verb, argument);
  if (commandLen <= 0 || static_cast<size_t>(commandLen) >= commandSize) {
    if (error != nullptr && errorSize > 0) {
      snprintf(error, errorSize, "%s command is too long.", verb);
    }
    return false;
  }
  return true;
}

int ftpReadResponse(IFtpsTransport &transport,
                    char *buf,
                    size_t bufLen,
                    uint32_t timeoutMs = FTPS_REPLY_TIMEOUT_MS) {
  size_t pos = 0;
  int multiCode = -1;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    uint8_t ch = 0;
    int readResult = transport.ctrlRead(&ch, 1);
    if (readResult == NSAPI_ERROR_WOULD_BLOCK) {
      delay(5);
      continue;
    }

    if (readResult <= 0) {
      break;
    }

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      buf[pos] = '\0';
      if (pos >= 3 && isdigit(buf[0]) && isdigit(buf[1]) && isdigit(buf[2])) {
        int code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');
        if (pos > 3 && buf[3] == '-') {
          multiCode = code;
          pos = 0;
          continue;
        }

        if (multiCode == -1 || code == multiCode) {
          return code;
        }
      }

      pos = 0;
      continue;
    }

    if (pos < bufLen - 1U) {
      buf[pos++] = static_cast<char>(ch);
    }
  }

  if (bufLen > 0) {
    buf[pos < bufLen ? pos : bufLen - 1U] = '\0';
  }
  return -1;
}

int ftpSendCommand(IFtpsTransport &transport,
                   const char *command,
                   char *buf,
                   size_t bufLen) {
  char line[256] = {};
  int lineLen = snprintf(line, sizeof(line), "%s\r\n", command);
  if (lineLen <= 0 || static_cast<size_t>(lineLen) >= sizeof(line)) {
    if (buf != nullptr && bufLen > 0) {
      snprintf(buf, bufLen, "FTP command buffer overflow.");
    }
    return -1;
  }

  if (!writeAll(transport, false, reinterpret_cast<const uint8_t *>(line),
                static_cast<size_t>(lineLen))) {
    if (buf != nullptr && bufLen > 0) {
      snprintf(buf, bufLen, "Failed to write FTP command.");
    }
    return -1;
  }

  return ftpReadResponse(transport, buf, bufLen);
}

void drainFinalTransferReply(IFtpsTransport &transport) {
  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  (void)ftpReadResponse(transport,
                        reply,
                        sizeof(reply),
                        FTPS_FINAL_REPLY_DRAIN_TIMEOUT_MS);
}

bool parsePasv(const char *response,
               FtpEndpoint &endpoint,
               char *hostBuf,
               size_t hostBufLen) {
  const char *cursor = strchr(response, '(');
  if (cursor == nullptr) {
    return false;
  }

  ++cursor;
  unsigned int parts[6] = {0};
  size_t index = 0;
  bool hasDigits = false;
  for (; *cursor != '\0' && index < 6; ++cursor) {
    if (*cursor >= '0' && *cursor <= '9') {
      unsigned int digit = static_cast<unsigned int>(*cursor - '0');
      // Reject values outside 0..255 while parsing to avoid overflow.
      if (parts[index] > 25U || (parts[index] == 25U && digit > 5U)) {
        return false;
      }
      parts[index] = (parts[index] * 10U) + digit;
      hasDigits = true;
      continue;
    }

    if (*cursor == ',' || *cursor == ')') {
      if (!hasDigits) {
        return false;
      }
      hasDigits = false;
      ++index;
      if (*cursor == ')') {
        break;
      }
      continue;
    }

    if (*cursor != ' ' && *cursor != '\t') {
      return false;
    }
  }

  if (index != 6) {
    return false;
  }

  int hostLen = snprintf(hostBuf,
                         hostBufLen,
                         "%d.%d.%d.%d",
                         parts[0],
                         parts[1],
                         parts[2],
                         parts[3]);
  if (hostLen <= 0 || static_cast<size_t>(hostLen) >= hostBufLen) {
    return false;
  }

  endpoint.port = static_cast<uint16_t>((parts[4] << 8U) | parts[5]);
  if (endpoint.port == 0) {
    return false;
  }

  endpoint.host = hostBuf;
  return true;
}

} // namespace

FtpsClient::FtpsClient() = default;

FtpsClient::~FtpsClient() {
  delete _transport;
}

bool FtpsClient::begin(NetworkInterface *network, char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (network == nullptr) {
    return failWith(
        _lastError,
        FtpsError::NetworkNotInitialized,
        error,
        errorSize,
        "NetworkInterface is null. Call Ethernet.begin() and pass Ethernet.getNetwork() to FtpsClient.begin().");
  }

  delete _transport;
  _transport = new (std::nothrow) MbedSecureSocketFtpsTransport(network);
  if (_transport == nullptr) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Failed to allocate MbedSecureSocketFtpsTransport.");
  }

  memset(&_activeConfig, 0, sizeof(_activeConfig));
  memset(_activeHost, 0, sizeof(_activeHost));
  memset(_activeUser, 0, sizeof(_activeUser));
  memset(_activePassword, 0, sizeof(_activePassword));
  memset(_activeTlsServerName, 0, sizeof(_activeTlsServerName));
  memset(_activeRootCaPem, 0, sizeof(_activeRootCaPem));
  memset(_normalizedFingerprint, 0, sizeof(_normalizedFingerprint));
  _connected = false;
  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::connect(const FtpsServerConfig &config, char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr) {
    return failWith(
        _lastError,
        FtpsError::NetworkNotInitialized,
        error,
        errorSize,
        "Call FtpsClient.begin() before connect().");
  }

  if (!hasValue(config.host)) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "FtpsServerConfig.host is required.");
  }

  if (!hasValue(config.user)) {
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        "FtpsServerConfig.user is required.");
  }

  if (!hasValue(config.password)) {
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        "FtpsServerConfig.password is required.");
  }

  if (config.port == 0) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "FtpsServerConfig.port must be non-zero.");
  }

  // Current library policy is fail-closed certificate validation.
  if (!config.validateServerCert) {
    return failWith(
        _lastError,
        FtpsError::CertValidationFailed,
        error,
        errorSize,
        "validateServerCert=false is not supported in this build.");
  }

  if (config.trustMode != FtpsTrustMode::Fingerprint &&
      config.trustMode != FtpsTrustMode::ImportedCert) {
    return failWith(
        _lastError,
        FtpsError::CertValidationFailed,
        error,
        errorSize,
        "Unsupported FtpsTrustMode value.");
  }

  memset(_normalizedFingerprint, 0, sizeof(_normalizedFingerprint));
  if (config.trustMode == FtpsTrustMode::Fingerprint) {
    if (!hasValue(config.fingerprint) ||
        !ftpsTrustNormalizeFingerprint(config.fingerprint,
                                       _normalizedFingerprint,
                                       sizeof(_normalizedFingerprint))) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "Fingerprint trust is selected, but the SHA-256 fingerprint is missing or malformed.");
    }
  }

  if (config.trustMode == FtpsTrustMode::ImportedCert) {
    if (!hasValue(config.rootCaPem) ||
        !ftpsTrustValidatePem(config.rootCaPem, FTPS_MAX_PEM_SIZE)) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "ImportedCert trust is selected, but the root CA PEM is missing or malformed.");
    }

    if (isIpLiteral(config.host) && !hasValue(config.tlsServerName)) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "ImportedCert trust with an IP host requires tlsServerName for hostname verification.");
    }
  }

  _transport->closeAll();
  _connected = false;

  memset(&_activeConfig, 0, sizeof(_activeConfig));
  memset(_activeHost, 0, sizeof(_activeHost));
  memset(_activeUser, 0, sizeof(_activeUser));
  memset(_activePassword, 0, sizeof(_activePassword));
  memset(_activeTlsServerName, 0, sizeof(_activeTlsServerName));
  memset(_activeRootCaPem, 0, sizeof(_activeRootCaPem));

  if (!copyStringToBuffer(config.host, _activeHost, sizeof(_activeHost))) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "FtpsServerConfig.host is too long.");
  }

  if (!copyStringToBuffer(config.user, _activeUser, sizeof(_activeUser))) {
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        "FtpsServerConfig.user is too long.");
  }

  if (!copyStringToBuffer(config.password, _activePassword, sizeof(_activePassword))) {
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        "FtpsServerConfig.password is too long.");
  }

  _activeConfig.host = _activeHost;
  _activeConfig.port = config.port;
  _activeConfig.user = _activeUser;
  _activeConfig.password = _activePassword;
  _activeConfig.trustMode = config.trustMode;
  _activeConfig.validateServerCert = config.validateServerCert;

  if (hasValue(config.tlsServerName)) {
    if (!copyStringToBuffer(config.tlsServerName,
                            _activeTlsServerName,
                            sizeof(_activeTlsServerName))) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "FtpsServerConfig.tlsServerName is too long.");
    }
    _activeConfig.tlsServerName = _activeTlsServerName;
  } else if (!isIpLiteral(_activeConfig.host)) {
    if (!copyStringToBuffer(_activeConfig.host,
                            _activeTlsServerName,
                            sizeof(_activeTlsServerName))) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "Derived tlsServerName is too long.");
    }
    _activeConfig.tlsServerName = _activeTlsServerName;
  }

  if (_activeConfig.trustMode == FtpsTrustMode::Fingerprint) {
    _activeConfig.fingerprint = _normalizedFingerprint;
    _activeConfig.rootCaPem = nullptr;
  } else {
    if (!copyStringToBuffer(config.rootCaPem,
                            _activeRootCaPem,
                            sizeof(_activeRootCaPem))) {
      return failWith(
          _lastError,
          FtpsError::CertValidationFailed,
          error,
          errorSize,
          "FtpsServerConfig.rootCaPem is too large.");
    }
    _activeConfig.rootCaPem = _activeRootCaPem;
    _activeConfig.fingerprint = nullptr;
  }

  FtpEndpoint endpoint = {_activeConfig.host, _activeConfig.port};
  FtpTlsConfig tls = {};
  tls.securityMode = FtpTlsSecurityMode::ExplicitTls;
  tls.serverName = _activeConfig.tlsServerName;
  tls.pinnedFingerprint =
      (_activeConfig.trustMode == FtpsTrustMode::Fingerprint) ? _normalizedFingerprint : nullptr;
  tls.rootCaPem = (_activeConfig.trustMode == FtpsTrustMode::ImportedCert) ? _activeConfig.rootCaPem : nullptr;
  tls.validateServerCert = _activeConfig.validateServerCert;

  if (!_transport->connectControl(endpoint, tls, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "TCP control connection failed.");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code != 220) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::BannerReadFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Failed to read FTP banner.");
  }

  code = ftpSendCommand(*_transport, "AUTH TLS", reply, sizeof(reply));
  if (code != 234) {
    code = ftpSendCommand(*_transport, "AUTH SSL", reply, sizeof(reply));
    if (code != 234) {
      _transport->closeAll();
      return failWith(
          _lastError,
          FtpsError::AuthTlsRejected,
          error,
          errorSize,
          hasValue(reply) ? reply : "AUTH TLS was rejected by the server.");
    }
  }

  if (!_transport->upgradeControlToTls(tls, error, errorSize)) {
    FtpsError failure = FtpsError::ControlTlsHandshakeFailed;
    if (_transport->getLastTlsError() == NSAPI_ERROR_AUTH_FAILURE) {
      failure = FtpsError::CertValidationFailed;
    }

    _transport->closeAll();
    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Control-channel TLS handshake failed.");
  }

  code = ftpSendCommand(*_transport, "PBSZ 0", reply, sizeof(reply));
  if (code != 200) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::PbszRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "PBSZ 0 was rejected.");
  }

  code = ftpSendCommand(*_transport, "PROT P", reply, sizeof(reply));
  if (code != 200) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::ProtPRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "PROT P was rejected.");
  }

  char command[192] = {};
  if (!formatCommandWithArg("USER",
                            _activeConfig.user,
                            command,
                            sizeof(command),
                            error,
                            errorSize)) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        hasValue(error) ? error : "USER command is too long.");
  }

  code = ftpSendCommand(*_transport, command, reply, sizeof(reply));
  if (code == 331) {
    if (!formatCommandWithArg("PASS",
                              _activeConfig.password,
                              command,
                              sizeof(command),
                              error,
                              errorSize)) {
      _transport->closeAll();
      return failWith(
          _lastError,
          FtpsError::LoginRejected,
          error,
          errorSize,
          hasValue(error) ? error : "PASS command is too long.");
    }

    code = ftpSendCommand(*_transport, command, reply, sizeof(reply));
    if (code != 230 && code != 232) {
      _transport->closeAll();
      return failWith(
          _lastError,
          FtpsError::LoginRejected,
          error,
          errorSize,
          hasValue(reply) ? reply : "PASS failed.");
    }
  } else if (code != 230 && code != 232) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::LoginRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "USER failed.");
  }

  code = ftpSendCommand(*_transport, "TYPE I", reply, sizeof(reply));
  if (code != 200) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::TypeRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "TYPE I was rejected.");
  }

  _connected = true;
  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::store(const char *remotePath, const uint8_t *data, size_t length,
                       char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before store().");
  }

  if (!hasValue(remotePath)) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "remotePath is required for store().");
  }

  if (length > 0 && data == nullptr) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "data is null but store() length is non-zero.");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommand(*_transport, "PASV", reply, sizeof(reply));
  if (code != 227) {
    return failWith(
        _lastError,
        FtpsError::PassiveModeRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "PASV was rejected.");
  }

  char dataHost[32] = {};
  FtpEndpoint dataEndpoint = {};
  if (!parsePasv(reply, dataEndpoint, dataHost, sizeof(dataHost))) {
    return failWith(
        _lastError,
        FtpsError::PasvParseFailed,
        error,
        errorSize,
        "Failed to parse PASV response.");
  }

  // Do not trust server-provided PASV host redirection by default.
  dataEndpoint.host = _activeConfig.host;

  FtpTlsConfig tls = {};
  tls.securityMode = FtpTlsSecurityMode::ExplicitTls;
  tls.serverName = _activeConfig.tlsServerName;
  tls.pinnedFingerprint =
      (_activeConfig.trustMode == FtpsTrustMode::Fingerprint) ? _normalizedFingerprint : nullptr;
  tls.rootCaPem = (_activeConfig.trustMode == FtpsTrustMode::ImportedCert) ? _activeConfig.rootCaPem : nullptr;
  tls.validateServerCert = _activeConfig.validateServerCert;

  if (!_transport->openProtectedDataChannel(dataEndpoint, tls, error, errorSize)) {
    int tlsError = _transport->getLastTlsError();
    FtpsError failure = FtpsError::DataConnectionFailed;
    if (tlsError != 0) {
      failure = (tlsError == NSAPI_ERROR_AUTH_FAILURE)
          ? FtpsError::SessionReuseRequired
          : FtpsError::DataTlsHandshakeFailed;
    }

    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Failed to open the protected passive data channel.");
  }

  char command[192] = {};
  if (!formatCommandWithArg("STOR",
                            remotePath,
                            command,
                            sizeof(command),
                            error,
                            errorSize)) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(error) ? error : "STOR command is too long.");
  }

  code = ftpSendCommand(*_transport, command, reply, sizeof(reply));
  if (code != 125 && code != 150) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "STOR was rejected.");
  }

  if (length > 0 && !writeAll(*_transport, true, data, length)) {
    _transport->closeData();
    drainFinalTransferReply(*_transport);
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "Failed while writing the protected FTPS data channel.");
  }

  _transport->closeData();
  code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code != 226 && code != 250) {
    return failWith(
        _lastError,
        FtpsError::FinalReplyFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Final STOR reply was not successful.");
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::retrieve(const char *remotePath, uint8_t *buffer, size_t bufferSize,
                          size_t &bytesRead, char *error, size_t errorSize) {
  bytesRead = 0;
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before retrieve().");
  }

  if (!hasValue(remotePath)) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "remotePath is required for retrieve().");
  }

  if (buffer == nullptr || bufferSize == 0) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "A non-empty destination buffer is required for retrieve().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommand(*_transport, "PASV", reply, sizeof(reply));
  if (code != 227) {
    return failWith(
        _lastError,
        FtpsError::PassiveModeRejected,
        error,
        errorSize,
        hasValue(reply) ? reply : "PASV was rejected.");
  }

  char dataHost[32] = {};
  FtpEndpoint dataEndpoint = {};
  if (!parsePasv(reply, dataEndpoint, dataHost, sizeof(dataHost))) {
    return failWith(
        _lastError,
        FtpsError::PasvParseFailed,
        error,
        errorSize,
        "Failed to parse PASV response.");
  }

  // Do not trust server-provided PASV host redirection by default.
  dataEndpoint.host = _activeConfig.host;

  FtpTlsConfig tls = {};
  tls.securityMode = FtpTlsSecurityMode::ExplicitTls;
  tls.serverName = _activeConfig.tlsServerName;
  tls.pinnedFingerprint =
      (_activeConfig.trustMode == FtpsTrustMode::Fingerprint) ? _normalizedFingerprint : nullptr;
  tls.rootCaPem = (_activeConfig.trustMode == FtpsTrustMode::ImportedCert) ? _activeConfig.rootCaPem : nullptr;
  tls.validateServerCert = _activeConfig.validateServerCert;

  if (!_transport->openProtectedDataChannel(dataEndpoint, tls, error, errorSize)) {
    int tlsError = _transport->getLastTlsError();
    FtpsError failure = FtpsError::DataConnectionFailed;
    if (tlsError != 0) {
      failure = (tlsError == NSAPI_ERROR_AUTH_FAILURE)
          ? FtpsError::SessionReuseRequired
          : FtpsError::DataTlsHandshakeFailed;
    }

    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Failed to open the protected passive data channel.");
  }

  char command[192] = {};
  if (!formatCommandWithArg("RETR",
                            remotePath,
                            command,
                            sizeof(command),
                            error,
                            errorSize)) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(error) ? error : "RETR command is too long.");
  }

  code = ftpSendCommand(*_transport, command, reply, sizeof(reply));
  if (code != 125 && code != 150) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "RETR was rejected.");
  }

  unsigned long dataReadStart = millis();
  while (true) {
    if (bytesRead >= bufferSize) {
      _transport->closeData();
      drainFinalTransferReply(*_transport);
      return failWith(
          _lastError,
          FtpsError::TransferFailed,
          error,
          errorSize,
          "The destination buffer was too small for the downloaded file.");
    }

    int readResult = _transport->dataRead(buffer + bytesRead, bufferSize - bytesRead);
    if (readResult == NSAPI_ERROR_WOULD_BLOCK) {
      if ((millis() - dataReadStart) >= FTPS_DATA_IO_TIMEOUT_MS) {
        _transport->closeData();
        drainFinalTransferReply(*_transport);
        bytesRead = 0;
        return failWith(
            _lastError,
            FtpsError::TransferFailed,
            error,
            errorSize,
            "Timed out while reading the protected FTPS data channel.");
      }
      delay(5);
      continue;
    }

    if (readResult > 0) {
      bytesRead += static_cast<size_t>(readResult);
      dataReadStart = millis();
      continue;
    }

    if (readResult == 0) {
      break;
    }

    _transport->closeData();
    drainFinalTransferReply(*_transport);
    bytesRead = 0;
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "Failed while reading the protected FTPS data channel.");
  }

  _transport->closeData();
  code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code != 226 && code != 250) {
    bytesRead = 0;
    return failWith(
        _lastError,
        FtpsError::FinalReplyFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Final RETR reply was not successful.");
  }

  _lastError = FtpsError::None;
  return true;
}

void FtpsClient::quit() {
  if (_transport == nullptr) {
    _connected = false;
    _lastError = FtpsError::None;
    return;
  }

  if (_connected) {
    char reply[FTPS_REPLY_BUFFER_SIZE] = {};
    int code = ftpSendCommand(*_transport, "QUIT", reply, sizeof(reply));
    if (code != 221) {
      _lastError = FtpsError::QuitFailed;
    } else {
      _lastError = FtpsError::None;
    }
  }

  _transport->closeAll();
  _connected = false;
}

FtpsError FtpsClient::lastError() const {
  return _lastError;
}
