// ArduinoOPTA-FTPS - Basic Upload Example
// SPDX-License-Identifier: CC0-1.0
//
// Upload a small buffer to an FTPS server using Explicit TLS.
// Requires: Arduino Opta + Ethernet connection + FTPS server on the LAN.

#include <Arduino.h>
#include <PortentaEthernet.h>
#include <Ethernet.h>
#include <FtpsClient.h>

static const char *FTP_HOST = "192.168.1.100";
static const uint16_t FTP_PORT = 21;
static const char *FTP_USER = "ftpuser";
static const char *FTP_PASS = "ftppass";

// SHA-256 fingerprint of the server's leaf certificate (64 hex chars, no separators)
static const char *SERVER_FINGERPRINT = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

static void printClientFailure(const char *step, FtpsClient &ftps, const char *error) {
  Serial.print("[FAIL] ");
  Serial.print(step);
  Serial.print(" (FtpsError=");
  Serial.print(static_cast<int>(ftps.lastError()));
  Serial.print("): ");
  Serial.println(error);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  Basic FTPS Upload");
  Serial.println("==============================================");
  Serial.print("[INFO] Target FTPS server: ");
  Serial.print(FTP_HOST);
  Serial.print(":");
  Serial.println(FTP_PORT);
  Serial.println("[INFO] Mode: Explicit FTPS with protected passive transfer.");
  Serial.println("[STEP] Ethernet initialization");
  byte mac[6];
  Ethernet.MACAddress(mac);
  if (Ethernet.begin(mac) == 0) {
    Serial.println("[FAIL] Ethernet.begin(): DHCP failed.");
    return;
  }
  Serial.print("[INFO] IP address: ");
  Serial.println(Ethernet.localIP());

  FtpsClient ftps;
  char error[128] = {};

  Serial.println("[STEP] FtpsClient.begin()");
  if (!ftps.begin(Ethernet.getNetwork(), error, sizeof(error))) {
    printClientFailure("FtpsClient.begin()", ftps, error);
    return;
  }
  Serial.println("[PASS] FtpsClient.begin()");

  FtpsServerConfig config;
  config.host = FTP_HOST;
  config.port = FTP_PORT;
  config.user = FTP_USER;
  config.password = FTP_PASS;
  config.trustMode = FtpsTrustMode::Fingerprint;
  config.fingerprint = SERVER_FINGERPRINT;
  config.validateServerCert = true;

  Serial.println("[STEP] FtpsClient.connect()");
  if (!ftps.connect(config, error, sizeof(error))) {
    printClientFailure("FtpsClient.connect()", ftps, error);
    return;
  }
  Serial.println("[PASS] FtpsClient.connect()");

  const char *data = "Hello from Arduino Opta!";
  Serial.println("[STEP] FtpsClient.store()");
  if (!ftps.store("/upload/test.txt", (const uint8_t *)data, strlen(data), error, sizeof(error))) {
    printClientFailure("FtpsClient.store()", ftps, error);
  } else {
    Serial.println("[PASS] FtpsClient.store(): uploaded /upload/test.txt");
  }

  Serial.println("[STEP] FtpsClient.quit()");
  ftps.quit();
  if (ftps.lastError() == FtpsError::QuitFailed) {
    Serial.println("[WARN] QUIT did not receive a 221 reply before shutdown.");
  } else {
    Serial.println("[PASS] FtpsClient.quit()");
  }
}

void loop() {
  // Nothing to do after setup.
}
