// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Explicit FTPS client for Arduino Opta.

#ifndef FTPS_CLIENT_H
#define FTPS_CLIENT_H

#include "transport/IFtpsTransport.h"
#include "FtpsTypes.h"
#include "FtpsErrors.h"

#include <stddef.h>

class NetworkInterface;

// Optional hook invoked during internal FTPS I/O wait loops.
// Sketches can set this to service watchdogs while transfers are active.
typedef void (*FtpsProgressCallback)();
void setFtpsClientProgressHook(FtpsProgressCallback hook);

/// Arduino-style FTPS client.
/// Owns its transport internally; callers only interact with this class.
class FtpsClient {
public:
  FtpsClient();
  ~FtpsClient();

  // Non-copyable — owns transport resources.
  FtpsClient(const FtpsClient &) = delete;
  FtpsClient &operator=(const FtpsClient &) = delete;

  /// Initialize the transport layer with a Mbed NetworkInterface.
  /// Call once before connect().  Returns false on failure.
  bool begin(NetworkInterface *network, char *error, size_t errorSize);

  /// Connect to the FTPS server described by config.
  bool connect(const FtpsServerConfig &config, char *error, size_t errorSize);

  /// Register an optional trace callback for diagnostic phase updates.
  void setTraceCallback(FtpsTraceCallback callback);

  /// Return the last internal phase reached by the client.
  const char *lastPhase() const;

  /// Create a remote directory via MKD.
  bool mkd(const char *remoteDir, char *error, size_t errorSize);

  /// Query the remote file size via SIZE.
  bool size(const char *remotePath, size_t &remoteBytes,
            char *error, size_t errorSize);

  /// List a remote directory (MLSD preferred, LIST fallback) into listingText.
  bool list(const char *remotePath,
            char *listingText,
            size_t listingTextSize,
            size_t &bytesRead,
            char *error,
            size_t errorSize);

  /// Delete a remote file via DELE.
  bool dele(const char *remotePath, char *error, size_t errorSize);

  /// Remove a remote directory via RMD.
  bool rmd(const char *remoteDir, char *error, size_t errorSize);

  /// Rename or move a remote path via RNFR/RNTO.
  bool rename(const char *fromPath,
              const char *toPath,
              char *error,
              size_t errorSize);

  /// Upload data to remotePath via STOR.
  bool store(const char *remotePath, const uint8_t *data, size_t length,
             char *error, size_t errorSize);

  /// Download remotePath via RETR into buffer.
  bool retrieve(const char *remotePath, uint8_t *buffer, size_t bufferSize,
                size_t &bytesRead, char *error, size_t errorSize);

  /// Diagnostic: Test if control connection is still alive (sends NOOP).
  /// Returns true if control channel responds, false if dead.
  bool isControlAlive(char *error, size_t errorSize);

  /// Send QUIT and close all sockets.
  void quit();

  /// Force-close all sockets (no QUIT) and re-run the connect/login/AUTH TLS
  /// flow using the cached configuration. Use this between large transfers
  /// to work around Mbed-OS socket teardown issues that can leave the control
  /// channel in a zombie state. Returns false if reconnect fails.
  /// Currently supports Fingerprint trust mode only.
  bool reconnect(char *error, size_t errorSize);

  /// Opt-in: when set true, store() will automatically call reconnect()
  /// before each upload after the first. Default is false (preserves
  /// existing single-session behavior). Reset by connect().
  void setReconnectBetweenStores(bool enabled);

  /// Return the error code from the last failed operation.
  FtpsError lastError() const;

  /// Return the most recent NSAPI socket-layer error code (e.g. -3005
  /// NSAPI_ERROR_NO_SOCKET for LWIP pool exhaustion, -3008
  /// NSAPI_ERROR_CONNECTION_TIMEOUT). Returns 0 if no socket-layer
  /// failure has been recorded since the last successful socket op.
  /// Useful for application-level retry policy: a DataConnectionFailed
  /// caused by -3005 is typically retriable after a TIME_WAIT drain,
  /// while -3008/-3001 generally are not.
  int lastNsapiError() const;

private:
  static constexpr size_t kMaxHostLen = 128;
  static constexpr size_t kMaxUserLen = 96;
  static constexpr size_t kMaxPasswordLen = 128;
  static constexpr size_t kMaxTlsServerNameLen = 128;
  static constexpr size_t kMaxRootCaPemLen = 4096 + 1;

  IFtpsTransport *_transport = nullptr;
  FtpsServerConfig _activeConfig = {};
  char _activeHost[kMaxHostLen] = {};
  char _activeUser[kMaxUserLen] = {};
  char _activePassword[kMaxPasswordLen] = {};
  char _activeTlsServerName[kMaxTlsServerNameLen] = {};
  char _activeRootCaPem[kMaxRootCaPemLen] = {};
  char _normalizedFingerprint[65] = {};
  bool _connected = false;
  bool _reconnectBetweenStores = false;
  uint32_t _storesSinceConnect = 0;
  FtpsError _lastError = FtpsError::None;
  FtpsTraceCallback _traceCallback = nullptr;
  const char *_lastPhase = "idle";

  void tracePhase(const char *phase);
  bool failAndDisconnect(FtpsError code,
                         char *error,
                         size_t errorSize,
                         const char *message);
};

#endif // FTPS_CLIENT_H
