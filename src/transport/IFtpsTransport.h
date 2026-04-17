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

// Optional global hook invoked at key points inside transport implementations
// (connectSocket, TLS handshake, etc.) to aid diagnostics of hangs that are
// below the FtpsClient tracePhase granularity. Default is nullptr (no-op).
void setFtpsTransportTraceHook(FtpsTraceCallback hook);
void ftpsTransportTrace(const char *phase);

class IFtpsTransport {
public:
  virtual ~IFtpsTransport() = default;

  virtual bool connectControl(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;
  virtual bool upgradeControlToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;

  virtual int ctrlRead(uint8_t *buf, size_t len) = 0;
  virtual int ctrlWrite(const uint8_t *buf, size_t len) = 0;
  virtual bool ctrlConnected() = 0;

  virtual bool openProtectedDataChannel(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;

    // Split-phase data-channel helpers.
    // RFC 4217 §9 requires that the transfer command (STOR/RETR) is sent over
    // the control channel *before* the data-channel TLS handshake begins.
    // openDataChannel() opens the TCP connection only; upgradeDataToTls()
    // performs the TLS handshake afterwards (once the server has accepted the
    // transfer command and is ready to negotiate TLS on its side).
    virtual bool openDataChannel(const FtpEndpoint &ep, char *error, size_t errorSize) = 0;
    virtual bool upgradeDataToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) = 0;

  virtual int dataRead(uint8_t *buf, size_t len) = 0;
  virtual int dataWrite(const uint8_t *buf, size_t len) = 0;
  virtual bool dataConnected() = 0;
  virtual void closeData() = 0;

  virtual void closeAll() = 0;

  virtual bool getPeerCertFingerprint(char *out, size_t outLen) { return false; }
  virtual int getLastTlsError() { return 0; }
};

#endif // IFTPS_TRANSPORT_H
