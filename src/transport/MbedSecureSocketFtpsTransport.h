// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Mbed TLSSocketWrapper-based FTPS transport.

#ifndef MBED_SECURE_SOCKET_FTPS_TRANSPORT_H
#define MBED_SECURE_SOCKET_FTPS_TRANSPORT_H

#include "IFtpsTransport.h"

class NetworkInterface;
class TCPSocket;
class TLSSocketWrapper;
struct mbedtls_ssl_session;

class MbedSecureSocketFtpsTransport : public IFtpsTransport {
public:
  explicit MbedSecureSocketFtpsTransport(NetworkInterface *network);
  ~MbedSecureSocketFtpsTransport() override;

  bool connectControl(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) override;
  bool upgradeControlToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) override;

  int ctrlRead(uint8_t *buf, size_t len) override;
  int ctrlWrite(const uint8_t *buf, size_t len) override;
  bool ctrlConnected() override;

  bool openProtectedDataChannel(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) override;
    bool openDataChannel(const FtpEndpoint &ep, char *error, size_t errorSize) override;
    bool upgradeDataToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) override;
  int dataRead(uint8_t *buf, size_t len) override;
  int dataWrite(const uint8_t *buf, size_t len) override;
  bool dataConnected() override;
  void closeData() override;

  void closeAll() override;

  bool getPeerCertFingerprint(char *out, size_t outLen) override;
  int getLastTlsError() override;
  int getLastNsapiError() override;

private:
  bool connectSocket(TCPSocket *&socket,
                     const FtpEndpoint &ep,
                     char *error,
                     size_t errorSize);
  bool configureTlsSocket(TLSSocketWrapper &socket,
                          const FtpTlsConfig &tls,
                          char *error,
                          size_t errorSize);
  bool completePinnedFingerprintCheck(TLSSocketWrapper &socket,
                                      const char *expectedFingerprint,
                                      char *error,
                                      size_t errorSize);
  void clearCachedControlSession();
  void cacheControlSession();
  bool applyCachedControlSession(TLSSocketWrapper &socket);
  void destroySockets();
  bool fingerprintFromSocket(TLSSocketWrapper &socket, char *out, size_t outLen);
  void closeControl();

  NetworkInterface *_network = nullptr;
  TCPSocket *_controlSocket = nullptr;
  TLSSocketWrapper *_controlTls = nullptr;
  TCPSocket *_dataSocket = nullptr;
  TLSSocketWrapper *_dataTls = nullptr;
  bool _controlConnected = false;
  bool _dataConnected = false;
  mbedtls_ssl_session *_cachedControlSession = nullptr;
  bool _cachedControlSessionValid = false;
  int _lastTlsError = 0;
  // Most-recent NSAPI socket-layer error (e.g. -3005 NSAPI_ERROR_NO_SOCKET
  // from socket->open(), -3008 NSAPI_ERROR_CONNECTION_TIMEOUT from
  // socket->connect(), etc.). 0 when last socket op succeeded.
  int _lastNsapiError = 0;
};

#endif // MBED_SECURE_SOCKET_FTPS_TRANSPORT_H
