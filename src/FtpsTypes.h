// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#ifndef FTPS_TYPES_H
#define FTPS_TYPES_H

#include <stdint.h>

enum class FtpsTrustMode : uint8_t {
  Fingerprint = 0,
  ImportedCert = 1,
};

/// v1 is fixed to Explicit FTPS over protected passive transfers.
/// Additional mode switches should not be exposed publicly until implemented.
struct FtpsServerConfig {
  const char *host = nullptr;
  uint16_t port = 21;
  const char *user = nullptr;
  const char *password = nullptr;
  const char *tlsServerName = nullptr;
  FtpsTrustMode trustMode = FtpsTrustMode::Fingerprint;
  const char *fingerprint = nullptr;
  const char *rootCaPem = nullptr;
  bool validateServerCert = true;
};

#endif // FTPS_TYPES_H
