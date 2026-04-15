// FtpsSpikeTest.ino — Explicit FTPS feasibility spike for Arduino Opta
//
// Purpose: Prove that TLSSocketWrapper can perform AUTH TLS + PBSZ 0 +
//          PROT P + protected passive STOR against a PR4100 NAS.
//
// This is a one-shot diagnostic sketch. It runs once in setup() and
// prints pass/fail to Serial. It is NOT production code.

#include <Arduino.h>

#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #error "This spike is designed for Arduino Opta only"
#endif

// Mbed networking — these are the headers under test
#include "mbed.h"
#include "netsocket/TCPSocket.h"
#include "netsocket/TLSSocketWrapper.h"

// ============================================================================
// TEST CONFIGURATION — edit these for your environment
// ============================================================================
static const char *FTP_HOST       = "192.168.1.100";  // FTPS server IP
static const uint16_t FTP_PORT    = 21;
static const char *FTP_USER       = "testuser";
static const char *FTP_PASS       = "testpass";
static const char *FTP_TEST_DIR   = "/ftps_test/spike";  // must exist on server
static const char *FTP_TEST_FILE  = "spike_test.txt";

// Informational only unless you also customize the underlying Mbed TLS
// auth mode. The sketch below does not implement a real verify-none path.
// For the first meaningful spike run, prefer supplying ROOT_CA_PEM.
static const bool SPIKE_CERT_VALIDATION_DISABLED = true;

// Optional: PEM-encoded root CA certificate for the server.
// Prefer setting this for the first meaningful spike run.
static const char *ROOT_CA_PEM    = nullptr;

// Timeout for socket operations (ms)
static const uint32_t SOCK_TIMEOUT_MS = 15000;

// ============================================================================
// Helpers
// ============================================================================

static NetworkInterface *gNet = nullptr;

// Print a pass/fail line and return the bool for chaining
static bool report(const char *step, bool ok, const char *detail = nullptr) {
  Serial.print(ok ? "[PASS] " : "[FAIL] ");
  Serial.print(step);
  if (detail) {
    Serial.print(" — ");
    Serial.print(detail);
  }
  Serial.println();
  return ok;
}

// Read one FTP response line. Returns the 3-digit code, or -1 on timeout.
// Handles multi-line responses (code-SP... terminated by code SP...).
static int ftpReadResponse(Socket *sock, char *buf, size_t bufLen,
                           uint32_t timeoutMs = SOCK_TIMEOUT_MS) {
  size_t pos = 0;
  int multiCode = -1;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    uint8_t ch;
    nsapi_size_or_error_t n = sock->recv(&ch, 1);
    if (n == NSAPI_ERROR_WOULD_BLOCK) {
      delay(5);
      continue;
    }
    if (n <= 0) {
      break;  // socket closed or error
    }

    if (ch == '\r') continue;

    if (ch == '\n') {
      buf[pos] = '\0';

      if (pos >= 3 && isdigit(buf[0]) && isdigit(buf[1]) && isdigit(buf[2])) {
        int code = (buf[0] - '0') * 100 + (buf[1] - '0') * 10 + (buf[2] - '0');

        if (pos > 3 && buf[3] == '-') {
          // Multi-line continuation
          multiCode = code;
          // Print continuation lines for diagnostics
          Serial.print("  << ");
          Serial.println(buf);
          pos = 0;
          continue;
        }

        if (multiCode == -1 || code == multiCode) {
          Serial.print("  << ");
          Serial.println(buf);
          return code;
        }
      }

      // Non-coded line in multi-line block — print and continue
      Serial.print("  << ");
      Serial.println(buf);
      pos = 0;
      continue;
    }

    if (pos < bufLen - 1) {
      buf[pos++] = (char)ch;
    }
  }

  buf[pos] = '\0';
  return -1;  // timeout
}

// Send an FTP command and read the response code.
static int ftpSendCommand(Socket *sock, const char *cmd,
                          char *buf, size_t bufLen) {
  Serial.print("  >> ");
  // Mask PASS command in output
  if (strncmp(cmd, "PASS ", 5) == 0) {
    Serial.println("PASS ****");
  } else {
    Serial.println(cmd);
  }

  // Build command with CRLF
  char line[256];
  snprintf(line, sizeof(line), "%s\r\n", cmd);
  size_t len = strlen(line);

  nsapi_size_or_error_t sent = sock->send(line, len);
  if (sent != (nsapi_size_or_error_t)len) {
    Serial.println("  !! send failed");
    return -1;
  }

  return ftpReadResponse(sock, buf, bufLen);
}

// Parse PASV response: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
static bool parsePasv(const char *response, SocketAddress &addr) {
  const char *p = strchr(response, '(');
  if (!p) return false;
  p++;

  unsigned int parts[6] = {0};
  int idx = 0;
  bool hasDigits = false;
  for (; *p && idx < 6; ++p) {
    if (isdigit(*p)) {
      unsigned int digit = static_cast<unsigned int>(*p - '0');
      if (parts[idx] > 25U || (parts[idx] == 25U && digit > 5U)) {
        return false;
      }
      parts[idx] = (parts[idx] * 10U) + digit;
      hasDigits = true;
      continue;
    }

    if (*p == ',' || *p == ')') {
      if (!hasDigits) {
        return false;
      }
      hasDigits = false;
      idx++;
      if (*p == ')') {
        break;
      }
    }
  }
  if (idx < 6) return false;

  char ip[20];
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
  uint16_t port = static_cast<uint16_t>((parts[4] << 8U) | parts[5]);
  if (port == 0) {
    return false;
  }

  addr.set_ip_address(ip);
  addr.set_port(port);
  return true;
}

// ============================================================================
// Spike test — runs once in setup()
// ============================================================================

void setup() {
  Serial.begin(115200);
  unsigned long waitStart = millis();
  while (!Serial && millis() - waitStart < 5000) { /* wait for serial */ }

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  FTPS Spike Test — TLSSocketWrapper on Opta");
  Serial.println("==============================================");
  Serial.println();

  // ------------------------------------------------------------------
  // Step 1: Ethernet init
  // ------------------------------------------------------------------
  Serial.println("[STEP 1] Ethernet initialization");
  byte mac[6];
  Ethernet.MACAddress(mac);
  if (Ethernet.begin(mac) == 0) {
    report("Ethernet.begin", false, "DHCP failed");
    return;
  }
  report("Ethernet.begin", true);
  Serial.print("  IP: ");
  Serial.println(Ethernet.localIP());

  gNet = Ethernet.getNetwork();
  if (!report("Ethernet.getNetwork()", gNet != nullptr,
              gNet ? "got NetworkInterface*" : "returned null")) {
    return;
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 2: Raw TCP connect to FTP server
  // ------------------------------------------------------------------
  Serial.println("[STEP 2] TCP connect to FTP server");
  TCPSocket ctrlSock;

  if (ctrlSock.open(gNet) != NSAPI_ERROR_OK) {
    report("ctrlSock.open", false);
    return;
  }
  ctrlSock.set_timeout(SOCK_TIMEOUT_MS);

  SocketAddress serverAddr;
  if (gNet->gethostbyname(FTP_HOST, &serverAddr) != NSAPI_ERROR_OK) {
    report("DNS resolve", false, FTP_HOST);
    return;
  }
  serverAddr.set_port(FTP_PORT);

  if (ctrlSock.connect(serverAddr) != NSAPI_ERROR_OK) {
    report("ctrlSock.connect", false);
    return;
  }
  report("TCP connect", true);

  // Read 220 banner
  char buf[256];
  int code = ftpReadResponse(&ctrlSock, buf, sizeof(buf));
  if (!report("220 banner", code == 220, buf)) {
    ctrlSock.close();
    return;
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 3: AUTH TLS
  // ------------------------------------------------------------------
  Serial.println("[STEP 3] AUTH TLS upgrade");
  code = ftpSendCommand(&ctrlSock, "AUTH TLS", buf, sizeof(buf));
  if (!report("AUTH TLS", code == 234, buf)) {
    // Try AUTH SSL as fallback (some servers only support that)
    code = ftpSendCommand(&ctrlSock, "AUTH SSL", buf, sizeof(buf));
    if (!report("AUTH SSL (fallback)", code == 234, buf)) {
      ftpSendCommand(&ctrlSock, "QUIT", buf, sizeof(buf));
      ctrlSock.close();
      return;
    }
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 4: TLS handshake on control channel
  // ------------------------------------------------------------------
  Serial.println("[STEP 4] TLSSocketWrapper handshake on control channel");
  Serial.println("  Creating TLSSocketWrapper with TRANSPORT_KEEP...");

  // Use TRANSPORT_KEEP so we manage the TCPSocket lifetime ourselves.
  // The hostname parameter is used for SNI (Server Name Indication).
  TLSSocketWrapper ctrlTls(&ctrlSock, FTP_HOST,
                           TLSSocketWrapper::TRANSPORT_KEEP);

  // Certificate trust setup
  if (ROOT_CA_PEM != nullptr) {
    nsapi_error_t caErr = ctrlTls.set_root_ca_cert(ROOT_CA_PEM);
    report("set_root_ca_cert", caErr == NSAPI_ERROR_OK,
           caErr == NSAPI_ERROR_OK ? "loaded" : "failed");
  }

  if (SPIKE_CERT_VALIDATION_DISABLED) {
    Serial.println("  WARNING: Certificate validation DISABLED (spike only)");
    // NOTE: In this sketch, SPIKE_CERT_VALIDATION_DISABLED only affects this message.
    // It does NOT call mbedtls_ssl_conf_authmode(MBEDTLS_SSL_VERIFY_NONE).
    // If ROOT_CA_PEM is null, a self-signed certificate can still
    // cause the handshake to fail and that is not enough to disprove Option C.
  }

  Serial.println("  Attempting TLS handshake (this may take several seconds)...");
  unsigned long hsStart = millis();
  nsapi_error_t hsErr = ctrlTls.connect();
  unsigned long hsElapsed = millis() - hsStart;

  char hsDetail[80];
  snprintf(hsDetail, sizeof(hsDetail), "err=%d, took %lums", hsErr, hsElapsed);
  if (!report("TLS handshake", hsErr == NSAPI_ERROR_OK, hsDetail)) {
    Serial.println();
    Serial.println("*** SPIKE RESULT: OPTION C (TLSSocketWrapper) DOES NOT WORK ***");
    Serial.println("*** Diagnose the error code and consider fallback transports ***");
    Serial.println();
    Serial.print("  Mbed TLS error code: ");
    Serial.println(hsErr);
    Serial.println("  Common causes:");
    Serial.println("    NSAPI_ERROR_AUTH_FAILURE (-3012): cert validation failed — try setting ROOT_CA_PEM");
    Serial.println("    NSAPI_ERROR_TIMEOUT: handshake took too long — increase SOCK_TIMEOUT_MS");
    Serial.println("    NSAPI_ERROR_NO_MEMORY: not enough RAM for TLS buffers");
    ctrlSock.close();
    return;
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 5: PBSZ 0 + PROT P (over encrypted control channel)
  // ------------------------------------------------------------------
  Serial.println("[STEP 5] PBSZ and PROT (over TLS control channel)");

  // From this point, all control I/O goes through ctrlTls, not ctrlSock.
  code = ftpSendCommand(&ctrlTls, "PBSZ 0", buf, sizeof(buf));
  if (!report("PBSZ 0", code == 200, buf)) {
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }

  code = ftpSendCommand(&ctrlTls, "PROT P", buf, sizeof(buf));
  if (!report("PROT P", code == 200, buf)) {
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 6: Login
  // ------------------------------------------------------------------
  Serial.println("[STEP 6] Login (over TLS control channel)");

  char cmd[128];
  snprintf(cmd, sizeof(cmd), "USER %s", FTP_USER);
  code = ftpSendCommand(&ctrlTls, cmd, buf, sizeof(buf));

  if (code == 331) {
    // Server wants a password
    snprintf(cmd, sizeof(cmd), "PASS %s", FTP_PASS);
    code = ftpSendCommand(&ctrlTls, cmd, buf, sizeof(buf));
    if (!report("PASS", code == 230, buf)) {
      ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
      ctrlTls.close();
      ctrlSock.close();
      return;
    }
  } else if (code == 230 || code == 232) {
    report("USER (no PASS needed)", true, buf);
  } else {
    report("USER", false, buf);
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }
  Serial.println();

  // ------------------------------------------------------------------
  // Step 7: CWD + TYPE I
  // ------------------------------------------------------------------
  Serial.println("[STEP 7] CWD and TYPE I");

  snprintf(cmd, sizeof(cmd), "CWD %s", FTP_TEST_DIR);
  code = ftpSendCommand(&ctrlTls, cmd, buf, sizeof(buf));
  if (!report("CWD", code == 250, buf)) {
    Serial.println("  NOTE: Create the test directory on the server and retry.");
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }

  code = ftpSendCommand(&ctrlTls, "TYPE I", buf, sizeof(buf));
  report("TYPE I", code == 200, buf);
  Serial.println();

  // ------------------------------------------------------------------
  // Step 8: PASV + protected data channel + STOR
  // ------------------------------------------------------------------
  Serial.println("[STEP 8] PASV + TLS data channel + STOR");

  code = ftpSendCommand(&ctrlTls, "PASV", buf, sizeof(buf));
  if (!report("PASV", code == 227, buf)) {
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }

  SocketAddress dataAddr;
  if (!report("Parse PASV", parsePasv(buf, dataAddr))) {
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }

  Serial.print("  Data endpoint: ");
  Serial.print(dataAddr.get_ip_address());
  Serial.print(":");
  Serial.println(dataAddr.get_port());

  // Open a raw TCP socket to the PASV endpoint, then wrap it with TLS
  TCPSocket dataSock;
  if (dataSock.open(gNet) != NSAPI_ERROR_OK) {
    report("dataSock.open", false);
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }
  dataSock.set_timeout(SOCK_TIMEOUT_MS);

  if (dataSock.connect(dataAddr) != NSAPI_ERROR_OK) {
    report("dataSock.connect", false);
    dataSock.close();
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }
  report("Data TCP connect", true);

  // Wrap data socket with TLS for PROT P
  Serial.println("  Wrapping data socket with TLSSocketWrapper...");
  TLSSocketWrapper dataTls(&dataSock, FTP_HOST,
                           TLSSocketWrapper::TRANSPORT_KEEP);

  if (ROOT_CA_PEM != nullptr) {
    dataTls.set_root_ca_cert(ROOT_CA_PEM);
  }

  Serial.println("  Data TLS handshake (session reuse test point)...");
  unsigned long dataHsStart = millis();
  nsapi_error_t dataHsErr = dataTls.connect();
  unsigned long dataHsElapsed = millis() - dataHsStart;

  snprintf(hsDetail, sizeof(hsDetail), "err=%d, took %lums", dataHsErr, dataHsElapsed);
  if (!report("Data TLS handshake", dataHsErr == NSAPI_ERROR_OK, hsDetail)) {
    Serial.println();
    Serial.println("*** DATA CHANNEL TLS FAILED ***");
    Serial.println("*** This is often caused by TLS session reuse enforcement ***");
    Serial.println("*** Check server logs for 'session reuse required' ***");
    Serial.println("*** TLSSocketWrapper may not propagate the control session ***");
    Serial.print("  Mbed TLS error code: ");
    Serial.println(dataHsErr);
    dataSock.close();
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }
  Serial.println();

  // Send STOR command over control channel
  Serial.println("[STEP 8b] STOR over protected data channel");
  snprintf(cmd, sizeof(cmd), "STOR %s", FTP_TEST_FILE);
  code = ftpSendCommand(&ctrlTls, cmd, buf, sizeof(buf));
  // 125 = transfer starting, 150 = opening data connection
  if (!report("STOR", code == 125 || code == 150, buf)) {
    dataTls.close();
    dataSock.close();
    ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
    ctrlTls.close();
    ctrlSock.close();
    return;
  }

  // Write test payload over encrypted data channel
  const char *testPayload = "FTPS spike test from Arduino Opta\r\n"
                            "If you can read this, STOR over PROT P works.\r\n";
  size_t payloadLen = strlen(testPayload);
  nsapi_size_or_error_t written = dataTls.send((const uint8_t *)testPayload,
                                               payloadLen);
  snprintf(hsDetail, sizeof(hsDetail), "sent %d of %u bytes",
           (int)written, (unsigned)payloadLen);
  report("Data write", written == (nsapi_size_or_error_t)payloadLen, hsDetail);

  // Close data channel (signals end of transfer)
  dataTls.close();
  dataSock.close();

  // Read transfer completion response on control channel
  code = ftpReadResponse(&ctrlTls, buf, sizeof(buf));
  report("Transfer complete", code == 226, buf);
  Serial.println();

  // ------------------------------------------------------------------
  // Step 9: QUIT and cleanup
  // ------------------------------------------------------------------
  Serial.println("[STEP 9] QUIT");
  ftpSendCommand(&ctrlTls, "QUIT", buf, sizeof(buf));
  ctrlTls.close();
  ctrlSock.close();

  // ------------------------------------------------------------------
  // Summary
  // ------------------------------------------------------------------
  Serial.println();
  Serial.println("==============================================");
  Serial.println("  SPIKE COMPLETE — ALL STEPS PASSED");
  Serial.println("==============================================");
  Serial.println();
  Serial.println("Option C (TLSSocketWrapper / MbedSecureSocketFtpsTransport)");
  Serial.println("is viable for Explicit FTPS on Arduino Opta.");
  Serial.println();
  Serial.println("Next steps:");
  Serial.println("  1. Re-run with SPIKE_CERT_VALIDATION_DISABLED = false and ROOT_CA_PEM set");
  Serial.println("  2. Test RETR (download) in addition to STOR (upload)");
  Serial.println("  3. Observe RAM usage (freeMemory or heap stats if available)");
  Serial.println("  4. Proceed to Phase 1 of the FTPS implementation plan");
}

void loop() {
  // Nothing — spike is a one-shot test in setup()
  delay(60000);
}
