// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#include "FtpsTrust.h"

#include <ctype.h>
#include <string.h>

#include "mbedtls/x509_crt.h"

namespace {

bool isHexChar(char ch) {
  return (ch >= '0' && ch <= '9') ||
         (ch >= 'A' && ch <= 'F') ||
         (ch >= 'a' && ch <= 'f');
}

bool isAllowedSeparator(char ch) {
  return ch == ':' || ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

} // namespace

bool ftpsTrustNormalizeFingerprint(const char *input, char *outBuf, size_t outBufLen) {
  if (input == nullptr || outBuf == nullptr || outBufLen < 65U) {
    return false;
  }

  size_t outIndex = 0;
  for (size_t index = 0; input[index] != '\0'; ++index) {
    char ch = input[index];
    if (isHexChar(ch)) {
      if (outIndex >= 64U) {
        return false;
      }
      outBuf[outIndex++] = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
      continue;
    }

    if (!isAllowedSeparator(ch)) {
      return false;
    }
  }

  if (outIndex != 64U) {
    return false;
  }

  outBuf[outIndex] = '\0';
  return true;
}

bool ftpsTrustFingerprintsMatch(const char *a, const char *b) {
  if (a == nullptr || b == nullptr) {
    return false;
  }

  unsigned char diff = 0;
  for (size_t index = 0; index < 64U; ++index) {
    if (a[index] == '\0' || b[index] == '\0') {
      return false;
    }
    diff |= static_cast<unsigned char>(a[index] ^ b[index]);
  }

  return diff == 0 && a[64] == '\0' && b[64] == '\0';
}

bool ftpsTrustValidatePem(const char *pem, size_t maxLen) {
  static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
  static const char kEnd[] = "-----END CERTIFICATE-----";

  if (pem == nullptr || maxLen == 0) {
    return false;
  }

  size_t actualLen = strnlen(pem, maxLen + 1U);
  if (actualLen == 0 || actualLen > maxLen) {
    return false;
  }

  const char *begin = strstr(pem, kBegin);
  const char *end = strstr(pem, kEnd);
  if (begin == nullptr || end == nullptr || end <= begin) {
    return false;
  }

  if (strstr(begin + strlen(kBegin), kBegin) != nullptr) {
    return false;
  }

  if (strstr(end + strlen(kEnd), kEnd) != nullptr) {
    return false;
  }

  mbedtls_x509_crt cert;
  mbedtls_x509_crt_init(&cert);
  int parseResult = mbedtls_x509_crt_parse(
      &cert,
      reinterpret_cast<const unsigned char *>(pem),
      actualLen + 1U);
  mbedtls_x509_crt_free(&cert);

  if (parseResult != 0) {
    return false;
  }

  return true;
}