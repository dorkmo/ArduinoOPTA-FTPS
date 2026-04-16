// ArduinoOPTA-FTPS - WD My Cloud OS 5 Live Test
// SPDX-License-Identifier: CC0-1.0
//
// Live FTPS integration test for Arduino Opta against a WD My Cloud NAS
// running My Cloud OS 5 with FTP + FTP SSL enabled.
//
// See the README.md in this example folder for NAS setup instructions.
// Update the configuration block below before flashing.

#include <Arduino.h>

#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #error "This example is designed for Arduino Opta only"
#endif

#include <FtpsClient.h>

// ============================================================================
// TEST CONFIGURATION — edit these for your setup
// ============================================================================

// IP address of your WD My Cloud NAS on the local network
static const char *FTP_HOST = "192.168.1.200";
static const uint16_t FTP_PORT = 21;

// FTP credentials — use a dedicated My Cloud user account
static const char *FTP_USER = "ftpuser";
static const char *FTP_PASS = "changeme";

// SHA-256 fingerprint of the NAS TLS certificate (64 hex chars, no separators).
// See README.md for how to obtain this from your NAS.
static const char *FTP_FINGERPRINT =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

// Remote paths for testing (created automatically via MKD)
static const char *REMOTE_PARENT_DIR = "/ftps_test";
static const char *REMOTE_TEST_DIR = "/ftps_test/opta_live";
static const char *REMOTE_TEST_PATH = "/ftps_test/opta_live/wd_mycloud_test.txt";

// Set to true to use a static IP instead of DHCP.
// DHCP may hang on some Opta boards; static IP is more reliable.
static const bool USE_STATIC_IP = false;
static const IPAddress STATIC_IP(192, 168, 1, 50);
static const IPAddress STATIC_DNS(192, 168, 1, 1);
static const IPAddress STATIC_GATEWAY(192, 168, 1, 1);
static const IPAddress STATIC_SUBNET(255, 255, 255, 0);

// ============================================================================
// Helpers
// ============================================================================

static void ftpsTraceCallback(const char *phase) {
  Serial.print("[FTPS] ");
  Serial.println(phase);
}

static void printClientFailure(const char *step, FtpsClient &ftps, const char *error) {
  Serial.print("[FAIL] ");
  Serial.print(step);
  Serial.print(" (FtpsError=");
  Serial.print(static_cast<int>(ftps.lastError()));
  Serial.print("): ");
  Serial.println(error);
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
  Serial.println("  WD My Cloud OS 5 Live FTPS Test");
  Serial.println("==============================================");
  Serial.print("[INFO] Target NAS: ");
  Serial.print(FTP_HOST);
  Serial.print(":");
  Serial.println(FTP_PORT);
  Serial.println("[INFO] Trust mode: Fingerprint");
  Serial.println("[INFO] Mode: Explicit FTPS with protected passive transfers.");

  // --- Ethernet ---
  Serial.println("[STEP] Ethernet initialization");
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
  Serial.print("[PASS] Ethernet.begin(): ");
  Serial.println(Ethernet.localIP());

  // --- FTPS client ---
  FtpsClient ftps;
  ftps.setTraceCallback(ftpsTraceCallback);
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
  config.trustMode = FtpsTrustMode::Fingerprint;
  config.fingerprint = FTP_FINGERPRINT;
  config.validateServerCert = true;

  // --- Connect ---
  Serial.println("[STEP] FtpsClient.connect()");
  if (!ftps.connect(config, error, sizeof(error))) {
    printClientFailure("FtpsClient.connect()", ftps, error);
    return;
  }
  Serial.println("[PASS] FtpsClient.connect()");

  // --- MKD parent ---
  Serial.println("[STEP] FtpsClient.mkd() parent");
  if (!ftps.mkd(REMOTE_PARENT_DIR, error, sizeof(error))) {
    printClientFailure("FtpsClient.mkd() parent", ftps, error);
    ftps.quit();
    return;
  }
  Serial.print("[PASS] FtpsClient.mkd(): ");
  Serial.println(REMOTE_PARENT_DIR);

  // --- MKD nested ---
  Serial.println("[STEP] FtpsClient.mkd() nested");
  if (!ftps.mkd(REMOTE_TEST_DIR, error, sizeof(error))) {
    printClientFailure("FtpsClient.mkd() nested", ftps, error);
    ftps.quit();
    return;
  }
  Serial.print("[PASS] FtpsClient.mkd(): ");
  Serial.println(REMOTE_TEST_DIR);

  // --- Upload ---
  static const char kUploadPayload[] =
      "Arduino Opta FTPS test - WD My Cloud OS 5\r\n"
      "If you can read this file, upload and TLS negotiation succeeded.\r\n";

  Serial.println("[STEP] FtpsClient.store()");
  if (!ftps.store(REMOTE_TEST_PATH,
                  reinterpret_cast<const uint8_t *>(kUploadPayload),
                  strlen(kUploadPayload), error, sizeof(error))) {
    printClientFailure("FtpsClient.store()", ftps, error);
    ftps.quit();
    return;
  }
  Serial.print("[PASS] FtpsClient.store(): uploaded ");
  Serial.println(REMOTE_TEST_PATH);

  // --- Size ---
  size_t remoteBytes = 0;
  Serial.println("[STEP] FtpsClient.size()");
  if (!ftps.size(REMOTE_TEST_PATH, remoteBytes, error, sizeof(error))) {
    printClientFailure("FtpsClient.size()", ftps, error);
    ftps.quit();
    return;
  }
  Serial.print("[PASS] FtpsClient.size(): ");
  Serial.print(remoteBytes);
  Serial.println(" bytes");
  if (remoteBytes != strlen(kUploadPayload)) {
    Serial.println("[WARN] Size does not match upload payload length.");
  }

  // --- Download ---
  uint8_t buffer[512] = {};
  size_t bytesRead = 0;
  Serial.println("[STEP] FtpsClient.retrieve()");
  if (!ftps.retrieve(REMOTE_TEST_PATH, buffer, sizeof(buffer), bytesRead, error, sizeof(error))) {
    printClientFailure("FtpsClient.retrieve()", ftps, error);
    ftps.quit();
    return;
  }
  Serial.print("[PASS] FtpsClient.retrieve(): downloaded ");
  Serial.print(bytesRead);
  Serial.println(" bytes");

  // Verify content
  if (bytesRead == strlen(kUploadPayload) &&
      memcmp(buffer, kUploadPayload, bytesRead) == 0) {
    Serial.println("[PASS] Content verification: download matches upload.");
  } else {
    Serial.println("[FAIL] Content verification: MISMATCH.");
  }

  // --- Quit ---
  Serial.println("[STEP] FtpsClient.quit()");
  ftps.quit();
  if (ftps.lastError() == FtpsError::QuitFailed) {
    Serial.println("[WARN] QUIT did not receive a 221 reply before shutdown.");
  } else {
    Serial.println("[PASS] FtpsClient.quit()");
  }

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  WD MY CLOUD LIVE TEST COMPLETE");
  Serial.println("==============================================");
}

void loop() {
  delay(60000);
}
