// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#ifndef FTPS_ERRORS_H
#define FTPS_ERRORS_H

enum class FtpsError {
  None = 0,
  NetworkNotInitialized,
  BannerReadFailed,
  PbszRejected,
  ProtPRejected,
  TypeRejected,
  PasvParseFailed,
  DataConnectionFailed,
  QuitFailed,
  AuthTlsRejected,
  ControlTlsHandshakeFailed,
  CertValidationFailed,
  DataTlsHandshakeFailed,
  SessionReuseRequired,
  LoginRejected,
  TransferFailed,
  FinalReplyFailed,
  PassiveModeRejected,
  ConnectionFailed,
};

#endif // FTPS_ERRORS_H
