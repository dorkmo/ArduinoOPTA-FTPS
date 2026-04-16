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

  /// Upload data to remotePath via STOR.
  bool store(const char *remotePath, const uint8_t *data, size_t length,
             char *error, size_t errorSize);

  /// Download remotePath via RETR into buffer.
  bool retrieve(const char *remotePath, uint8_t *buffer, size_t bufferSize,
                size_t &bytesRead, char *error, size_t errorSize);

  /// Send QUIT and close all sockets.
  void quit();

  /// Return the error code from the last failed operation.
  FtpsError lastError() const;

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
  FtpsError _lastError = FtpsError::None;
  FtpsTraceCallback _traceCallback = nullptr;
  const char *_lastPhase = "idle";

  void tracePhase(const char *phase);
};

#endif // FTPS_CLIENT_H
