// ArduinoOPTA-FTPS - Retry Upload Example
// SPDX-License-Identifier: CC0-1.0
//
// Demonstrates bounded per-file retries using shared classifier helpers:
// - ftpsIsSessionDead(err)
// - ftpsIsTransferRetriable(err, nsapiCode)
//
// This pattern is useful for multi-file backup workloads on Arduino Opta where
// transient data-channel failures may occur while the control channel remains
// alive.

#include <Arduino.h>
#include <PortentaEthernet.h>
#include <Ethernet.h>
#include <FtpsClient.h>
#include <FtpsErrors.h>

static const char *FTP_HOST = "192.168.1.100";
static const uint16_t FTP_PORT = 21;
static const char *FTP_USER = "ftpuser";
static const char *FTP_PASS = "ftppass";

// SHA-256 fingerprint of server certificate (64 hex chars, no separators).
static const char *SERVER_FINGERPRINT = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

// Set true for static IP if DHCP is unreliable on your network.
static const bool USE_STATIC_IP = false;
static const IPAddress STATIC_IP(192, 168, 1, 60);
static const IPAddress STATIC_DNS(192, 168, 1, 1);
static const IPAddress STATIC_GATEWAY(192, 168, 1, 1);
static const IPAddress STATIC_SUBNET(255, 255, 255, 0);

static const uint8_t MAX_ATTEMPTS = 3;
static const uint32_t RETRY_BACKOFF_1_MS = 20000UL;
static const uint32_t RETRY_BACKOFF_2_MS = 40000UL;

struct UploadItem {
  const char *remotePath;
  const uint8_t *data;
  size_t size;
};

static void printClientFailure(const char *step, FtpsClient &ftps, const char *error) {
  Serial.print("[FAIL] ");
  Serial.print(step);
  Serial.print(" (FtpsError=");
  Serial.print(static_cast<int>(ftps.lastError()));
  Serial.print(", NSAPI=");
  Serial.print(ftps.lastNsapiError());
  Serial.print("): ");
  Serial.println(error);
}

static bool storeWithRetry(FtpsClient &ftps,
                           const char *remotePath,
                           const uint8_t *payload,
                           size_t payloadSize,
                           char *error,
                           size_t errorSize) {
  uint8_t attempt = 0;
  while (attempt < MAX_ATTEMPTS) {
    attempt++;
    error[0] = '\0';

    if (ftps.store(remotePath, payload, payloadSize, error, errorSize)) {
      if (attempt > 1) {
        Serial.print("[INFO] Retry recovered upload: ");
        Serial.print(remotePath);
        Serial.print(" after attempts=");
        Serial.println(attempt);
      }
      return true;
    }

    const FtpsError err = ftps.lastError();
    const int nsapi = ftps.lastNsapiError();

    if (ftpsIsSessionDead(err)) {
      Serial.print("[FAIL] Session became unusable during upload: ");
      Serial.println(remotePath);
      return false;
    }

    if (!ftpsIsTransferRetriable(err, nsapi)) {
      Serial.print("[FAIL] Non-retriable upload failure: ");
      Serial.println(remotePath);
      return false;
    }

    if (attempt >= MAX_ATTEMPTS) {
      break;
    }

    const uint32_t backoffMs = (attempt == 1) ? RETRY_BACKOFF_1_MS : RETRY_BACKOFF_2_MS;
    Serial.print("[WARN] Retrying upload: ");
    Serial.print(remotePath);
    Serial.print(" next_attempt=");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.print(MAX_ATTEMPTS);
    Serial.print(" nsapi=");
    Serial.print(nsapi);
    Serial.print(" wait_ms=");
    Serial.println(backoffMs);

    const uint32_t start = millis();
    while (millis() - start < backoffMs) {
      delay(100);
    }
  }

  Serial.print("[FAIL] Retry budget exhausted for: ");
  Serial.println(remotePath);
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  FTPS Retry Upload Example");
  Serial.println("==============================================");

  byte mac[6];
  Ethernet.MACAddress(mac);
  if (USE_STATIC_IP) {
    Ethernet.begin(mac, STATIC_IP, STATIC_DNS, STATIC_GATEWAY, STATIC_SUBNET);
  } else {
    if (Ethernet.begin(mac) == 0) {
      Serial.println("[FAIL] Ethernet.begin(): DHCP failed.");
      return;
    }
  }

  Serial.print("[INFO] IP address: ");
  Serial.println(Ethernet.localIP());

  FtpsClient ftps;
  char error[128] = {};
  if (!ftps.begin(Ethernet.getNetwork(), error, sizeof(error))) {
    printClientFailure("FtpsClient.begin()", ftps, error);
    return;
  }

  FtpsServerConfig config;
  config.host = FTP_HOST;
  config.port = FTP_PORT;
  config.user = FTP_USER;
  config.password = FTP_PASS;
  config.trustMode = FtpsTrustMode::Fingerprint;
  config.fingerprint = SERVER_FINGERPRINT;
  config.validateServerCert = true;

  if (!ftps.connect(config, error, sizeof(error))) {
    printClientFailure("FtpsClient.connect()", ftps, error);
    return;
  }

  // Optional hardening mode for multi-file upload runs.
  // When enabled, the library reconnects before each store() after the first.
  ftps.setReconnectBetweenStores(false);

  static const char kFileA[] = "retry example payload A\n";
  static const char kFileB[] = "retry example payload B\n";

  const UploadItem uploads[] = {
    {"/upload/retry_a.txt", (const uint8_t *)kFileA, strlen(kFileA)},
    {"/upload/retry_b.txt", (const uint8_t *)kFileB, strlen(kFileB)},
  };

  uint8_t ok = 0;
  uint8_t failed = 0;
  for (size_t i = 0; i < (sizeof(uploads) / sizeof(uploads[0])); ++i) {
    if (storeWithRetry(ftps,
                       uploads[i].remotePath,
                       uploads[i].data,
                       uploads[i].size,
                       error,
                       sizeof(error))) {
      ok++;
      Serial.print("[PASS] Uploaded ");
      Serial.println(uploads[i].remotePath);
    } else {
      failed++;
      printClientFailure("storeWithRetry()", ftps, error);
      if (ftpsIsSessionDead(ftps.lastError())) {
        Serial.println("[INFO] Aborting remaining uploads: control session is dead.");
        break;
      }
    }
  }

  ftps.quit();

  Serial.print("[SUMMARY] uploaded=");
  Serial.print(ok);
  Serial.print(" failed=");
  Serial.println(failed);
}

void loop() {
  // One-shot demo.
}
