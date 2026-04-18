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

static FtpsProgressCallback g_ftpsProgressHook = nullptr;

void setFtpsClientProgressHook(FtpsProgressCallback hook) {
  g_ftpsProgressHook = hook;
}

static inline void ftpsProgressPulse() {
  if (g_ftpsProgressHook != nullptr) {
    g_ftpsProgressHook();
  }
}

namespace {

static const uint32_t FTPS_REPLY_TIMEOUT_MS = 15000;
static const uint32_t FTPS_DATA_IO_TIMEOUT_MS = 15000;
static const uint32_t FTPS_FINAL_REPLY_DRAIN_TIMEOUT_MS = 5000;
// Diagnostic: some targets reset while reading final STOR control reply after
// successful data-channel close. Reconnect-per-file callers can safely skip it.
static const bool FTPS_SKIP_STORE_FINAL_REPLY_DIAG = false;
static const size_t FTPS_REPLY_BUFFER_SIZE = 256;
static const size_t FTPS_COMMAND_BUFFER_SIZE = 192;
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
      ftpsProgressPulse();
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

// Forward declaration — ftpSendCommand is defined below but called by
// ftpSendCommandWithArg.
int ftpSendCommand(IFtpsTransport &transport,
                   const char *command,
                   char *buf,
                   size_t bufLen);

int ftpSendCommandWithArg(IFtpsTransport &transport,
                          const char *verb,
                          const char *argument,
                          char *reply,
                          size_t replySize,
                          char *error,
                          size_t errorSize) {
  char command[FTPS_COMMAND_BUFFER_SIZE] = {};
  if (!formatCommandWithArg(verb,
                            argument,
                            command,
                            sizeof(command),
                            error,
                            errorSize)) {
    if (reply != nullptr && replySize > 0) {
      snprintf(reply,
               replySize,
               "%s",
               hasValue(error) ? error : "FTP command is too long.");
    }
    return -1;
  }

  return ftpSendCommand(transport, command, reply, replySize);
}

bool containsIgnoreCase(const char *haystack, const char *needle) {
  if (!hasValue(haystack) || !hasValue(needle)) {
    return false;
  }

  size_t needleLen = strlen(needle);
  for (size_t start = 0; haystack[start] != '\0'; ++start) {
    size_t matched = 0;
    while (matched < needleLen && haystack[start + matched] != '\0') {
      char hay = static_cast<char>(tolower(
          static_cast<unsigned char>(haystack[start + matched])));
      char nee = static_cast<char>(tolower(
          static_cast<unsigned char>(needle[matched])));
      if (hay != nee) {
        break;
      }
      ++matched;
    }

    if (matched == needleLen) {
      return true;
    }
  }

  return false;
}

bool ftpReplyLooksLikeExistingPath(const char *reply) {
  return containsIgnoreCase(reply, "exist") ||
         containsIgnoreCase(reply, "already");
}

bool parseSizeReplyBytes(const char *reply, size_t &remoteBytes) {
  remoteBytes = 0;
  if (!hasValue(reply)) {
    return false;
  }

  const char *cursor = reply;
  if (isdigit(static_cast<unsigned char>(cursor[0])) &&
      isdigit(static_cast<unsigned char>(cursor[1])) &&
      isdigit(static_cast<unsigned char>(cursor[2]))) {
    cursor += 3;
  }

  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }

  if (!isdigit(static_cast<unsigned char>(*cursor))) {
    return false;
  }

  size_t parsed = 0;
  const size_t kMaxSize = static_cast<size_t>(-1);
  while (isdigit(static_cast<unsigned char>(*cursor))) {
    unsigned int digit = static_cast<unsigned int>(*cursor - '0');
    if (parsed > (kMaxSize - digit) / 10U) {
      return false;
    }
    parsed = (parsed * 10U) + digit;
    ++cursor;
  }

  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }

  if (*cursor != '\0') {
    return false;
  }

  remoteBytes = parsed;
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
      ftpsProgressPulse();
      delay(5);
      continue;
    }

    // Some TLS stacks can report 0 temporarily before response bytes are ready.
    // Treat it as "no data yet" and continue waiting until timeout.
    if (readResult == 0) {
      ftpsProgressPulse();
      delay(5);
      continue;
    }

    if (readResult < 0) {
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

void FtpsClient::setTraceCallback(FtpsTraceCallback callback) {
  _traceCallback = callback;
}

const char *FtpsClient::lastPhase() const {
  return _lastPhase;
}

void FtpsClient::tracePhase(const char *phase) {
  _lastPhase = hasValue(phase) ? phase : "idle";
  if (_traceCallback != nullptr) {
    _traceCallback(_lastPhase);
  }
}

bool FtpsClient::failAndDisconnect(FtpsError code,
                                   char *error,
                                   size_t errorSize,
                                   const char *message) {
  if (_transport != nullptr) {
    _transport->closeAll();
  }
  _connected = false;
  return failWith(_lastError, code, error, errorSize, message);
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

bool FtpsClient::mkd(const char *remoteDir, char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before mkd().");
  }

  if (!hasValue(remoteDir)) {
    return failWith(
        _lastError,
        FtpsError::DirectoryCreateFailed,
        error,
        errorSize,
        "remoteDir is required for mkd().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommandWithArg(*_transport,
                                   "MKD",
                                   remoteDir,
                                   reply,
                                   sizeof(reply),
                                   error,
                                   errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending MKD.");
  }

  if (code == 257 || code == 250 || code == 521 ||
      (code == 550 && ftpReplyLooksLikeExistingPath(reply))) {
    _lastError = FtpsError::None;
    return true;
  }

  return failWith(
      _lastError,
      FtpsError::DirectoryCreateFailed,
      error,
      errorSize,
      hasValue(reply) ? reply : "MKD was rejected.");
}

bool FtpsClient::size(const char *remotePath,
                      size_t &remoteBytes,
                      char *error,
                      size_t errorSize) {
  remoteBytes = 0;
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before size().");
  }

  if (!hasValue(remotePath)) {
    return failWith(
        _lastError,
        FtpsError::SizeQueryFailed,
        error,
        errorSize,
        "remotePath is required for size().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommandWithArg(*_transport,
                                   "SIZE",
                                   remotePath,
                                   reply,
                                   sizeof(reply),
                                   error,
                                   errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending SIZE.");
  }

  if (code != 213) {
    return failWith(
        _lastError,
        FtpsError::SizeQueryFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "SIZE was rejected.");
  }

  if (!parseSizeReplyBytes(reply, remoteBytes)) {
    return failWith(
        _lastError,
        FtpsError::SizeQueryFailed,
        error,
        errorSize,
        "Server returned a malformed SIZE reply.");
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::list(const char *remotePath,
                      char *listingText,
                      size_t listingTextSize,
                      size_t &bytesRead,
                      char *error,
                      size_t errorSize) {
  bytesRead = 0;
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before list().");
  }

  if (listingText == nullptr || listingTextSize == 0) {
    return failWith(
        _lastError,
        FtpsError::ListFailed,
        error,
        errorSize,
        "A non-empty destination buffer is required for list().");
  }

  listingText[0] = '\0';
  const char *targetPath = hasValue(remotePath) ? remotePath : ".";

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommand(*_transport, "PASV", reply, sizeof(reply));
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending PASV.");
  }

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

  dataEndpoint.host = _activeConfig.host;

  FtpTlsConfig tls = {};
  tls.securityMode = FtpTlsSecurityMode::ExplicitTls;
  tls.serverName = _activeConfig.tlsServerName;
  tls.pinnedFingerprint =
      (_activeConfig.trustMode == FtpsTrustMode::Fingerprint) ? _normalizedFingerprint : nullptr;
  tls.rootCaPem = (_activeConfig.trustMode == FtpsTrustMode::ImportedCert) ? _activeConfig.rootCaPem : nullptr;
  tls.validateServerCert = _activeConfig.validateServerCert;

  if (!_transport->openDataChannel(dataEndpoint, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::DataConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Failed to open passive data channel for list().");
  }

  bool usedMlsd = true;
  code = ftpSendCommandWithArg(*_transport,
                               "MLSD",
                               targetPath,
                               reply,
                               sizeof(reply),
                               error,
                               errorSize);
  if (code < 0) {
    _transport->closeData();
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending MLSD.");
  }

  if (code != 125 && code != 150) {
    usedMlsd = false;
    code = ftpSendCommandWithArg(*_transport,
                                 "LIST",
                                 targetPath,
                                 reply,
                                 sizeof(reply),
                                 error,
                                 errorSize);
    if (code < 0) {
      _transport->closeData();
      return failAndDisconnect(
          FtpsError::ConnectionFailed,
          error,
          errorSize,
          hasValue(error) ? error : "Control connection lost while sending LIST.");
    }
    if (code != 125 && code != 150) {
      _transport->closeData();
      return failWith(
          _lastError,
          FtpsError::ListFailed,
          error,
          errorSize,
          hasValue(reply) ? reply : "MLSD/LIST was rejected.");
    }
  }

  if (!_transport->upgradeDataToTls(tls, error, errorSize)) {
    int tlsError = _transport->getLastTlsError();
    FtpsError failure = (tlsError == NSAPI_ERROR_AUTH_FAILURE)
        ? FtpsError::SessionReuseRequired
        : FtpsError::DataTlsHandshakeFailed;
    drainFinalTransferReply(*_transport);
    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Data-channel TLS handshake failed after list command.");
  }

  unsigned long dataReadStart = millis();
  while (true) {
    if (bytesRead + 1U >= listingTextSize) {
      _transport->closeData();
      drainFinalTransferReply(*_transport);
      listingText[0] = '\0';
      bytesRead = 0;
      return failWith(
          _lastError,
          FtpsError::ListFailed,
          error,
          errorSize,
          "List output exceeded destination buffer.");
    }

    int readResult = _transport->dataRead(
        reinterpret_cast<uint8_t *>(listingText + bytesRead),
        listingTextSize - bytesRead - 1U);
    if (readResult == NSAPI_ERROR_WOULD_BLOCK) {
      ftpsProgressPulse();
      if ((millis() - dataReadStart) >= FTPS_DATA_IO_TIMEOUT_MS) {
        _transport->closeData();
        drainFinalTransferReply(*_transport);
        listingText[0] = '\0';
        bytesRead = 0;
        return failWith(
            _lastError,
            FtpsError::ListFailed,
            error,
            errorSize,
            "Timed out while reading list data channel.");
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
    listingText[0] = '\0';
    bytesRead = 0;
    return failWith(
        _lastError,
        FtpsError::ListFailed,
        error,
        errorSize,
        "Failed while reading list data channel.");
  }

  listingText[bytesRead] = '\0';
  _transport->closeData();

  code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code < 0) {
    listingText[0] = '\0';
    bytesRead = 0;
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost before final LIST/MLSD reply.");
  }

  if (code != 226 && code != 250) {
    listingText[0] = '\0';
    bytesRead = 0;
    return failWith(
        _lastError,
        FtpsError::FinalReplyFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Final list reply was not successful.");
  }

  if (usedMlsd && bytesRead == 0) {
    // Some servers return 150/226 for empty MLSD; keep success semantics.
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::dele(const char *remotePath, char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before dele().");
  }

  if (!hasValue(remotePath)) {
    return failWith(
        _lastError,
        FtpsError::DeleteFailed,
        error,
        errorSize,
        "remotePath is required for dele().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommandWithArg(*_transport,
                                   "DELE",
                                   remotePath,
                                   reply,
                                   sizeof(reply),
                                   error,
                                   errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending DELE.");
  }

  if (code != 250) {
    return failWith(
        _lastError,
        FtpsError::DeleteFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "DELE was rejected.");
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::rmd(const char *remoteDir, char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before rmd().");
  }

  if (!hasValue(remoteDir)) {
    return failWith(
        _lastError,
        FtpsError::DirectoryRemoveFailed,
        error,
        errorSize,
        "remoteDir is required for rmd().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommandWithArg(*_transport,
                                   "RMD",
                                   remoteDir,
                                   reply,
                                   sizeof(reply),
                                   error,
                                   errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending RMD.");
  }

  if (code != 250) {
    return failWith(
        _lastError,
        FtpsError::DirectoryRemoveFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "RMD was rejected.");
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::rename(const char *fromPath,
                        const char *toPath,
                        char *error,
                        size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before rename().");
  }

  if (!hasValue(fromPath) || !hasValue(toPath)) {
    return failWith(
        _lastError,
        FtpsError::RenameFailed,
        error,
        errorSize,
        "fromPath and toPath are required for rename().");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommandWithArg(*_transport,
                                   "RNFR",
                                   fromPath,
                                   reply,
                                   sizeof(reply),
                                   error,
                                   errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending RNFR.");
  }

  if (code != 350) {
    return failWith(
        _lastError,
        FtpsError::RenameFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "RNFR was rejected.");
  }

  code = ftpSendCommandWithArg(*_transport,
                               "RNTO",
                               toPath,
                               reply,
                               sizeof(reply),
                               error,
                               errorSize);
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending RNTO.");
  }

  if (code != 250) {
    return failWith(
        _lastError,
        FtpsError::RenameFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "RNTO was rejected.");
  }

  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::connect(const FtpsServerConfig &config, char *error, size_t errorSize) {
  clearError(error, errorSize);

  // Reset per-session counters. The reconnect-between-stores opt-in is
  // intentionally left unchanged so the application does not have to
  // re-enable it after every reconnect cycle.
  _storesSinceConnect = 0;

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

  tracePhase("connect:tcp-open");
  if (!_transport->connectControl(endpoint, tls, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "TCP control connection failed.");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  tracePhase("connect:banner");
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

  tracePhase("connect:auth-tls");
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

  tracePhase("connect:control-tls");
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

  tracePhase("connect:pbsz");
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

  tracePhase("connect:prot-p");
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

  tracePhase("connect:user");
  code = ftpSendCommandWithArg(*_transport,
                               "USER",
                               _activeConfig.user,
                               reply,
                               sizeof(reply),
                               error,
                               errorSize);
  if (code == 331) {
    tracePhase("connect:pass");
    code = ftpSendCommandWithArg(*_transport,
                                 "PASS",
                                 _activeConfig.password,
                                 reply,
                                 sizeof(reply),
                                 error,
                                 errorSize);
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

  tracePhase("connect:type-i");
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

  tracePhase("connect:done");
  _connected = true;
  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::store(const char *remotePath, const uint8_t *data, size_t length,
                       char *error, size_t errorSize) {
  tracePhase("store:entry");
  clearError(error, errorSize);

  tracePhase("store:check-transport");
  if (_transport == nullptr || !_connected) {
    tracePhase("store:not-connected");
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "Call connect() before store().");
  }

  // Opt-in workaround for Mbed-OS socket teardown issues that can leave
  // the control channel in a zombie state after a successful upload.
  // When enabled, perform a full QUIT-less close + reconnect before any
  // store() after the first one.
  if (_reconnectBetweenStores && _storesSinceConnect > 0) {
    tracePhase("store:auto-reconnect");
    if (!reconnect(error, errorSize)) {
      tracePhase("store:auto-reconnect-failed");
      return false;
    }
    tracePhase("store:auto-reconnect-ok");
  }

  tracePhase("store:check-remotepath");
  if (!hasValue(remotePath)) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "remotePath is required for store().");
  }

  tracePhase("store:check-data");
  if (length > 0 && data == nullptr) {
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        "data is null but store() length is non-zero.");
  }

  tracePhase("store:alloc-reply");
  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  tracePhase("store:pasv");
  int code = ftpSendCommand(*_transport, "PASV", reply, sizeof(reply));
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending PASV.");
  }

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

  tracePhase("store:data-open");
  // RFC 4217 §9: open the TCP data connection first, send the transfer command
  // over the control channel, then perform the TLS handshake on the data side.
  // Reversing this order (TLS before STOR) causes servers such as pyftpdlib
  // to hang because they only wrap the data socket with TLS after they have
  // accepted the STOR command.
  if (!_transport->openDataChannel(dataEndpoint, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::DataConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Failed to open the passive data TCP connection.");
  }

  tracePhase("store:stor");
  code = ftpSendCommandWithArg(*_transport,
                               "STOR",
                               remotePath,
                               reply,
                               sizeof(reply),
                               error,
                               errorSize);
  if (code < 0) {
    _transport->closeData();
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending STOR.");
  }

  if (code != 125 && code != 150) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "STOR was rejected.");
  }

  // Now upgrade the data connection to TLS (server is ready after sending 125/150).
  tracePhase("store:data-tls");
  if (!_transport->upgradeDataToTls(tls, error, errorSize)) {
    int tlsError = _transport->getLastTlsError();
    FtpsError failure = (tlsError == NSAPI_ERROR_AUTH_FAILURE)
        ? FtpsError::SessionReuseRequired
        : FtpsError::DataTlsHandshakeFailed;
    drainFinalTransferReply(*_transport);
    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Data-channel TLS handshake failed after STOR.");
  }

  tracePhase("store:write");
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

  tracePhase("store:data-close");
  _transport->closeData();
  
  if (FTPS_SKIP_STORE_FINAL_REPLY_DIAG) {
    tracePhase("store:final-reply-skipped");
    _lastError = FtpsError::None;
    return true;
  }

  tracePhase("store:final-reply-read");
  code = ftpReadResponse(*_transport, reply, sizeof(reply), FTPS_FINAL_REPLY_DRAIN_TIMEOUT_MS);
  tracePhase("store:final-reply-got");
  
  if (code < 0) {
    tracePhase("store:final-reply-failed");
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost before final STOR reply.");
  }

  if (code != 226 && code != 250) {
    tracePhase("store:final-reply-bad-code");
    return failWith(
        _lastError,
        FtpsError::FinalReplyFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Final STOR reply was not successful.");
  }

  tracePhase("store:done");
  _lastError = FtpsError::None;
  ++_storesSinceConnect;
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

  tracePhase("retrieve:pasv");
  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommand(*_transport, "PASV", reply, sizeof(reply));
  if (code < 0) {
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending PASV.");
  }

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

  tracePhase("retrieve:data-open");
  // RFC 4217 §9: TCP connect first, then send RETR, then upgrade to TLS.
  if (!_transport->openDataChannel(dataEndpoint, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::DataConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Failed to open the passive data TCP connection.");
  }

  tracePhase("retrieve:retr");
  code = ftpSendCommandWithArg(*_transport,
                               "RETR",
                               remotePath,
                               reply,
                               sizeof(reply),
                               error,
                               errorSize);
  if (code < 0) {
    _transport->closeData();
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost while sending RETR.");
  }

  if (code != 125 && code != 150) {
    _transport->closeData();
    return failWith(
        _lastError,
        FtpsError::TransferFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "RETR was rejected.");
  }

  tracePhase("retrieve:data-tls");
  if (!_transport->upgradeDataToTls(tls, error, errorSize)) {
    int tlsError = _transport->getLastTlsError();
    FtpsError failure = (tlsError == NSAPI_ERROR_AUTH_FAILURE)
        ? FtpsError::SessionReuseRequired
        : FtpsError::DataTlsHandshakeFailed;
    drainFinalTransferReply(*_transport);
    return failWith(
        _lastError,
        failure,
        error,
        errorSize,
        hasValue(error) ? error : "Data-channel TLS handshake failed after RETR.");
  }

  tracePhase("retrieve:read");
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
      ftpsProgressPulse();
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

  tracePhase("retrieve:data-close");
  _transport->closeData();
  tracePhase("retrieve:final-reply");
  code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code < 0) {
    bytesRead = 0;
    return failAndDisconnect(
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control connection lost before final RETR reply.");
  }

  if (code != 226 && code != 250) {
    bytesRead = 0;
    return failWith(
        _lastError,
        FtpsError::FinalReplyFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Final RETR reply was not successful.");
  }

  tracePhase("retrieve:done");
  _lastError = FtpsError::None;
  return true;
}

bool FtpsClient::isControlAlive(char *error, size_t errorSize) {
  clearError(error, errorSize);

  if (_transport == nullptr || !_connected) {
    failWith(_lastError, FtpsError::ConnectionFailed, error, errorSize,
             "Not connected.");
    return false;
  }

  tracePhase("control-check:noop");
  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  int code = ftpSendCommand(*_transport, "NOOP", reply, sizeof(reply));
  
  if (code < 0) {
    // Connection failed during NOOP
    tracePhase("control-check:failed");
    failAndDisconnect(FtpsError::ConnectionFailed, error, errorSize,
                      hasValue(error) ? error : "Control connection failed during NOOP.");
    return false;
  }

  if (code != 200) {
    // Server rejected NOOP (unusual)
    tracePhase("control-check:rejected");
    _lastError = FtpsError::TransferFailed;
    if (error != nullptr && errorSize > 0) {
      snprintf(error, errorSize, "NOOP rejected with code %d: %s", code,
               hasValue(reply) ? reply : "unknown");
    }
    return false;
  }

  tracePhase("control-check:ok");
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
    tracePhase("quit:send");
    char reply[FTPS_REPLY_BUFFER_SIZE] = {};
    int code = ftpSendCommand(*_transport, "QUIT", reply, sizeof(reply));
    if (code != 221) {
      _lastError = FtpsError::QuitFailed;
    } else {
      _lastError = FtpsError::None;
    }
  }

  tracePhase("quit:close-all");
  _transport->closeAll();
  tracePhase("quit:done");
  _connected = false;
}

void FtpsClient::setReconnectBetweenStores(bool enabled) {
  _reconnectBetweenStores = enabled;
}

bool FtpsClient::reconnect(char *error, size_t errorSize) {
  tracePhase("reconnect:entry");
  clearError(error, errorSize);

  if (_transport == nullptr) {
    return failWith(
        _lastError,
        FtpsError::NetworkNotInitialized,
        error,
        errorSize,
        "Call begin() before reconnect().");
  }

  // Snapshot the active configuration on the stack BEFORE closeAll/connect
  // touch _activeConfig and the _active* buffers (connect() memsets them).
  char snapHost[kMaxHostLen];
  char snapUser[kMaxUserLen];
  char snapPass[kMaxPasswordLen];
  char snapTls[kMaxTlsServerNameLen];
  char snapFp[sizeof(_normalizedFingerprint)];
  copyStringToBuffer(_activeHost, snapHost, sizeof(snapHost));
  copyStringToBuffer(_activeUser, snapUser, sizeof(snapUser));
  copyStringToBuffer(_activePassword, snapPass, sizeof(snapPass));
  copyStringToBuffer(_activeTlsServerName, snapTls, sizeof(snapTls));
  copyStringToBuffer(_normalizedFingerprint, snapFp, sizeof(snapFp));

  uint16_t snapPort = _activeConfig.port;
  FtpsTrustMode snapTrust = _activeConfig.trustMode;
  bool snapValidate = _activeConfig.validateServerCert;

  if (snapTrust == FtpsTrustMode::ImportedCert) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "reconnect() does not yet support ImportedCert trust mode.");
  }

  if (snapHost[0] == '\0') {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "reconnect() called with no cached host. Call connect() first.");
  }

  // Force-close all sockets without QUIT — the control channel may already
  // be in a zombie state, so trying to send QUIT could block.
  tracePhase("reconnect:close-all");
  _transport->closeAll();
  _connected = false;

  FtpsServerConfig fresh = {};
  fresh.host = snapHost;
  fresh.port = snapPort;
  fresh.user = snapUser;
  fresh.password = snapPass;
  fresh.tlsServerName = (snapTls[0] != '\0') ? snapTls : nullptr;
  fresh.trustMode = snapTrust;
  fresh.fingerprint = (snapFp[0] != '\0') ? snapFp : nullptr;
  fresh.rootCaPem = nullptr;
  fresh.validateServerCert = snapValidate;

  tracePhase("reconnect:connect");
  bool ok = connect(fresh, error, errorSize);
  tracePhase(ok ? "reconnect:done" : "reconnect:fail");
  return ok;
}

FtpsError FtpsClient::lastError() const {
  return _lastError;
}

int FtpsClient::lastNsapiError() const {
  return _transport != nullptr ? _transport->getLastNsapiError() : 0;
}
