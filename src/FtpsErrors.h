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
  DirectoryCreateFailed,
  SizeQueryFailed,
  ListFailed,
  DeleteFailed,
  RenameFailed,
  DirectoryRemoveFailed,
};

// Common NSAPI error codes surfaced by Mbed/LWIP on Opta.
// Kept here so application retry policy can avoid magic numbers.
static constexpr int kFtpsNsapiNoSocket = -3005;

/// Returns true when the control/session state is no longer usable and
/// callers should abandon the current FTPS batch/session.
inline bool ftpsIsSessionDead(FtpsError err) {
  switch (err) {
    case FtpsError::ConnectionFailed:
    case FtpsError::NetworkNotInitialized:
    case FtpsError::BannerReadFailed:
    case FtpsError::AuthTlsRejected:
    case FtpsError::ControlTlsHandshakeFailed:
    case FtpsError::CertValidationFailed:
    case FtpsError::LoginRejected:
    case FtpsError::PbszRejected:
    case FtpsError::ProtPRejected:
    case FtpsError::TypeRejected:
      return true;
    default:
      return false;
  }
}

/// Returns true when a transfer failure is likely transient and safe to retry
/// without declaring the whole FTPS session dead.
///
/// Pass lastNsapiError() as nsapiCode. If nsapiCode is 0 (unavailable), this
/// function is permissive for DataConnectionFailed.
inline bool ftpsIsTransferRetriable(FtpsError err, int nsapiCode) {
  if (err == FtpsError::DataConnectionFailed) {
    return nsapiCode == 0 || nsapiCode == kFtpsNsapiNoSocket;
  }
  if (err == FtpsError::DataTlsHandshakeFailed) {
    return true;
  }
  return false;
}

#endif // FTPS_ERRORS_H
