// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#ifndef IFTPS_TRANSPORT_H
#define IFTPS_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

enum class FtpTlsSecurityMode : uint8_t {
  Plain = 0,
  ExplicitTls = 1,
  ImplicitTls = 2,
};

struct FtpEndpoint {
  const char *host;
  uint16_t port;
};

struct FtpTlsConfig {
  FtpTlsSecurityMode securityMode = FtpTlsSecurityMode::ExplicitTls;
  const char *serverName = nullptr;
  const char *pinnedFingerprint = nullptr;
  const char *rootCaPem = nullptr;
  bool validateServerCert = true;
};

typedef void (*FtpsTraceCallback)(const char *phase);

class IFtpsTransport {
public:
  virtual ~IFtpsTransport() = default;

  virtual bool connectControl(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;
  virtual bool upgradeControlToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;

  virtual int ctrlRead(uint8_t *buf, size_t len) = 0;
  virtual int ctrlWrite(const uint8_t *buf, size_t len) = 0;
  virtual bool ctrlConnected() = 0;

  virtual bool openProtectedDataChannel(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;
  virtual int dataRead(uint8_t *buf, size_t len) = 0;
  virtual int dataWrite(const uint8_t *buf, size_t len) = 0;
  virtual bool dataConnected() = 0;
  virtual void closeData() = 0;

  virtual void closeAll() = 0;

  virtual bool getPeerCertFingerprint(char *out, size_t outLen) { return false; }
  virtual int getLastTlsError() { return 0; }
};

#endif // IFTPS_TRANSPORT_H
