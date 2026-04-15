// ArduinoOPTA-FTPS - FileZilla Live Test
// SPDX-License-Identifier: CC0-1.0
//
// Live FTPS integration test for Arduino Opta against FileZilla Server.
// Update the configuration block below before flashing the sketch.

#include <Arduino.h>

#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #error "This example is designed for Arduino Opta only"
#endif

#include <FtpsClient.h>

// ============================================================================
// TEST CONFIGURATION — edit these for your FileZilla Server
// ============================================================================
static const char *FTP_HOST = "192.168.1.100";
static const uint16_t FTP_PORT = 21;
static const char *FTP_USER = "testuser";
static const char *FTP_PASS = "testpass";

// Set this if the server certificate name does not match FTP_HOST.
// This is especially important when FTP_HOST is an IP literal and the
// certificate was issued to a DNS hostname.
static const char *FTP_TLS_SERVER_NAME = nullptr;

// Choose one trust mode:
//   FtpsTrustMode::Fingerprint
//   FtpsTrustMode::ImportedCert
static const FtpsTrustMode FTP_TRUST_MODE = FtpsTrustMode::Fingerprint;

// SHA-256 fingerprint of the FileZilla Server certificate.
static const char *FTP_FINGERPRINT =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

// Paste the FileZilla Server certificate or issuing CA certificate here when
// using FtpsTrustMode::ImportedCert.
static const char *ROOT_CA_PEM = nullptr;

static const char *REMOTE_TEST_PATH = "/ftps_test/filezilla_opta_live_test.txt";
static const bool RUN_UPLOAD_TEST = true;
static const bool RUN_DOWNLOAD_TEST = true;

// ============================================================================
// Helpers
// ============================================================================

static void printClientFailure(const char *step, FtpsClient &ftps, const char *error) {
  Serial.print("[FAIL] ");
  Serial.print(step);
  Serial.print(" (FtpsError=");
  Serial.print(static_cast<int>(ftps.lastError()));
  Serial.print("): ");
  Serial.println(error);
}

static void reportQuitStatus(FtpsClient &ftps) {
  if (ftps.lastError() == FtpsError::QuitFailed) {
    Serial.println("[WARN] QUIT did not receive a 221 reply before shutdown.");
  } else {
    Serial.println("[PASS] FtpsClient.quit()");
  }
}

static const char *trustModeName(FtpsTrustMode mode) {
  return mode == FtpsTrustMode::Fingerprint ? "Fingerprint" : "ImportedCert";
}

// ============================================================================
// Main sketch
// ============================================================================

void setup() {
  Serial.begin(115200);
  unsigned long waitStart = millis();
  while (!Serial && millis() - waitStart < 5000) { }

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  FileZilla Server Live FTPS Test");
  Serial.println("==============================================");
  Serial.print("[INFO] Target FTPS server: ");
  Serial.print(FTP_HOST);
  Serial.print(":");
  Serial.println(FTP_PORT);
  Serial.print("[INFO] Trust mode: ");
  Serial.println(trustModeName(FTP_TRUST_MODE));
  if (FTP_TLS_SERVER_NAME != nullptr) {
    Serial.print("[INFO] TLS server name: ");
    Serial.println(FTP_TLS_SERVER_NAME);
  }
  Serial.println("[INFO] Mode: Explicit FTPS with protected passive transfers.");

  Serial.println("[STEP] Ethernet initialization");
  byte mac[6];
  Ethernet.MACAddress(mac);
  if (Ethernet.begin(mac) == 0) {
    Serial.println("[FAIL] Ethernet.begin(): DHCP failed.");
    return;
  }
  Serial.print("[PASS] Ethernet.begin(): ");
  Serial.println(Ethernet.localIP());

  FtpsClient ftps;
  char error[192] = {};

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
  config.tlsServerName = FTP_TLS_SERVER_NAME;
  config.trustMode = FTP_TRUST_MODE;
  config.fingerprint = FTP_FINGERPRINT;
  config.rootCaPem = ROOT_CA_PEM;
  config.validateServerCert = true;

  Serial.println("[STEP] FtpsClient.connect()");
  if (!ftps.connect(config, error, sizeof(error))) {
    printClientFailure("FtpsClient.connect()", ftps, error);
    return;
  }
  Serial.println("[PASS] FtpsClient.connect()");

  static const char kUploadPayload[] =
      "Arduino Opta FTPS FileZilla live test\r\n"
      "If you can read this file, upload and TLS negotiation succeeded.\r\n";

  if (RUN_UPLOAD_TEST) {
    Serial.println("[STEP] FtpsClient.store()");
    if (!ftps.store(REMOTE_TEST_PATH,
                    reinterpret_cast<const uint8_t *>(kUploadPayload),
                    strlen(kUploadPayload),
                    error,
                    sizeof(error))) {
      printClientFailure("FtpsClient.store()", ftps, error);
      ftps.quit();
      reportQuitStatus(ftps);
      return;
    }
    Serial.print("[PASS] FtpsClient.store(): uploaded ");
    Serial.println(REMOTE_TEST_PATH);
  }

  if (RUN_DOWNLOAD_TEST) {
    uint8_t buffer[512] = {};
    size_t bytesRead = 0;
    Serial.println("[STEP] FtpsClient.retrieve()");
    if (!ftps.retrieve(REMOTE_TEST_PATH, buffer, sizeof(buffer), bytesRead, error, sizeof(error))) {
      printClientFailure("FtpsClient.retrieve()", ftps, error);
      ftps.quit();
      reportQuitStatus(ftps);
      return;
    }

    Serial.print("[PASS] FtpsClient.retrieve(): downloaded ");
    Serial.print(bytesRead);
    Serial.println(" bytes");
    Serial.println("[INFO] Retrieved payload:");
    Serial.write(buffer, bytesRead);
    Serial.println();
  }

  Serial.println("[STEP] FtpsClient.quit()");
  ftps.quit();
  reportQuitStatus(ftps);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  FILEZILLA LIVE TEST COMPLETE");
  Serial.println("==============================================");
}

void loop() {
  delay(60000);
}