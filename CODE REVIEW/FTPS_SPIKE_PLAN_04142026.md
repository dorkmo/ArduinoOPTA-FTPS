# FTPS Phase 0 Spike — TLSSocketWrapper Feasibility Test

**Date:** April 14, 2026
**Status:** Planning only — spike not yet executed
**Prerequisite for:** All FTPS implementation work (see `FTPS_IMPLEMENTATION_04132026.md`)
**Hardware required:** Arduino Opta + Ethernet cable + WD My Cloud PR4100 NAS on the same LAN

---

## Purpose

This is the single blocking gate for the FTPS implementation plan. The spike answers one question:

> Can an Arduino Opta use Mbed's `TLSSocketWrapper` to perform an Explicit FTPS session — `AUTH TLS` control upgrade, `PBSZ 0`, `PROT P`, and a protected passive-mode `STOR` — against the PR4100 NAS?

If the spike succeeds, proceed to Phase 1 (config schema). If it fails, diagnose the failure mode and evaluate the documented fallback transports (Option B: `TLSSocket`, Option A: `SSLClient`).

---

## What the spike proves or kills

| Test | Pass means | Fail means |
|------|-----------|------------|
| `TCPSocket` connect to PR4100:21 | Mbed raw TCP works on Opta Ethernet | Fundamental networking problem — unrelated to TLS |
| Read 220 banner | FTP protocol layer works over raw Mbed sockets | Socket read model is incompatible (unlikely) |
| Send `AUTH TLS`, read 234 | PR4100 accepts Explicit TLS upgrade requests | PR4100 may not have Explicit TLS enabled — check NAS config |
| `TLSSocketWrapper` handshake on control socket | STARTTLS-style upgrade works on Opta | Option C is dead; fall back to Option B or A |
| `PBSZ 0` → 200, `PROT P` → 200 | Server accepts protected data channel setup | Server may not support PROT P — check NAS TLS config |
| `USER` / `PASS` login over TLS control | Credentials are encrypted in transit | Auth failure — check credentials, cipher suite |
| `PASV` parse, `TLSSocketWrapper` on data socket | Protected passive data channel works | Session reuse may be required (see below) |
| `STOR` small file, read 226 completion | Full Explicit FTPS round-trip works | Data-channel TLS or transfer-completion problem |

### Secondary question: TLS session reuse

Many FTPS servers require the data channel's TLS session to reuse the control channel's session ID or ticket. This is risk #2 in the implementation plan.

**During the spike, observe:**
- Does the data-channel `TLSSocketWrapper` handshake succeed without any explicit session reuse configuration?
- If it fails, does the PR4100 log a "session reuse required" or similar error?
- Does `TLSSocketWrapper` expose any session ticket / session ID API that could be passed from control to data?

If session reuse is required and `TLSSocketWrapper` does not support it, this is a hard blocker that requires either:
- A PR4100 configuration change to disable session reuse enforcement
- A different TLS transport that supports session resumption
- A custom session-ticket injection (high effort, last resort)

---

## Prerequisites

### PR4100 NAS setup

1. Enable FTP on the PR4100 (Dashboard → Settings → FTP or similar).
2. Enable **Explicit TLS** (sometimes labeled "FTP with TLS" or "FTPS" or "AUTH TLS").
3. Keep passive mode enabled (should be default).
4. Create or confirm a test FTP user account with write access to a test directory.
5. Note the NAS IP address, FTP port (typically 21), username, and password.
6. Optionally: retrieve the PR4100's TLS certificate fingerprint (SHA-256) for later trust validation testing. For the spike, certificate validation can be skipped.

### Development environment

1. Install `arduino-cli` with the Arduino Mbed OS Opta board package (`arduino:mbed_opta`).
2. Confirm the board package version is `4.5.0` or later (matches the installed core).
3. Verify the sketch compiles before loading onto hardware:
   ```
   arduino-cli compile --fqbn arduino:mbed_opta:opta -v FtpsSpikeTest/FtpsSpikeTest.ino
   ```
4. Connect the Opta to the same LAN as the PR4100 via Ethernet cable.
5. Open a Serial Monitor at 115200 baud.

---

## Spike sketch

This is a standalone `.ino` file. It does **not** depend on any TankAlarm code. It should be placed outside the main TankAlarm project (e.g., in a temporary `FtpsSpikeTest/` folder) to avoid polluting the production build.

### Configuration constants

Replace these with your PR4100 test environment values before compiling.

```cpp
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
static const char *FTP_HOST       = "192.168.1.100";  // PR4100 IP
static const uint16_t FTP_PORT    = 21;
static const char *FTP_USER       = "testuser";
static const char *FTP_PASS       = "testpass";
static const char *FTP_TEST_DIR   = "/tankalarm/spike";  // must exist on NAS
static const char *FTP_TEST_FILE  = "spike_test.txt";

// Informational only unless you also customize the underlying Mbed TLS
// auth mode. The sketch below does not implement a real verify-none path.
// For the first meaningful spike run, prefer supplying ROOT_CA_PEM.
static const bool VALIDATE_CERT   = false;

// Optional: PEM-encoded root CA certificate for the PR4100.
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

  int parts[6] = {0};
  int idx = 0;
  for (; *p && idx < 6; ++p) {
    if (isdigit(*p)) {
      parts[idx] = parts[idx] * 10 + (*p - '0');
    } else if (*p == ',' || *p == ')') {
      idx++;
    }
  }
  if (idx < 6) return false;

  char ip[20];
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
  uint16_t port = (parts[4] << 8) | parts[5];

  addr.set_ip_address(ip);
  addr.set_port(port);
  return true;
}
```

### Main spike flow

```cpp
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

  if (!VALIDATE_CERT) {
    Serial.println("  WARNING: Certificate validation DISABLED (spike only)");
    // NOTE: In this sketch, VALIDATE_CERT only affects this message.
    // It does NOT call mbedtls_ssl_conf_authmode(MBEDTLS_SSL_VERIFY_NONE).
    // If ROOT_CA_PEM is null, a self-signed PR4100 certificate can still
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
    Serial.println("  NOTE: Create the test directory on the NAS and retry.");
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
    Serial.println("*** Check PR4100 logs for 'session reuse required' ***");
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
  Serial.println("Option C (TLSSocketWrapper / MbedSecureSocketFtpTransport)");
  Serial.println("is viable for Explicit FTPS on Arduino Opta.");
  Serial.println();
  Serial.println("Next steps:");
  Serial.println("  1. Re-run with VALIDATE_CERT = true and ROOT_CA_PEM set");
  Serial.println("  2. Test RETR (download) in addition to STOR (upload)");
  Serial.println("  3. Observe RAM usage (freeMemory or heap stats if available)");
  Serial.println("  4. Proceed to Phase 1 of the FTPS implementation plan");
}

void loop() {
  // Nothing — spike is a one-shot test in setup()
  delay(60000);
}
```

---

## Step-by-step execution guide

### 1. Prepare the NAS

- Verify Explicit TLS is enabled on the PR4100 FTP service.
- Create the test directory (`/tankalarm/spike` or equivalent).
- Confirm the test user has write access to that directory.

### 2. Prepare the sketch

- Copy the spike sketch code above into `FtpsSpikeTest/FtpsSpikeTest.ino`.
- Edit the configuration constants at the top to match your environment.
- Prefer setting `ROOT_CA_PEM` to the PR4100 trust cert for the first meaningful run.
- Do **not** assume `VALIDATE_CERT = false` disables verification in the sample sketch as written; it does not.

### 3. Compile

```
arduino-cli compile --fqbn arduino:mbed_opta:opta -v FtpsSpikeTest/FtpsSpikeTest.ino
```

**If compilation fails:**

| Error | Likely cause | Action |
|-------|-------------|--------|
| `TLSSocketWrapper.h: No such file` | Header not in include path | Try `#include "netsocket/TLSSocketWrapper.h"` or check board package version |
| `TCPSocket.h: No such file` | Same | Try `#include "netsocket/TCPSocket.h"` |
| `Ethernet.getNetwork()` undefined | Older Ethernet library | Check that `PortentaEthernet` is installed and imported |
| Linker errors for TLS symbols | Mbed TLS not linked | May need to add `MBEDTLS_xxx` config flags — document the exact error |
| `TRANSPORT_KEEP` not found | Enum naming difference or older header | Check the actual enum in the installed `TLSSocketWrapper.h` header |

**Record the exact error message.** Even a compile failure is a useful spike result — it tells us whether the headers are genuinely usable.

### 4. Upload and run

```
arduino-cli upload --fqbn arduino:mbed_opta:opta -p COMx FtpsSpikeTest/FtpsSpikeTest.ino
arduino-cli monitor -p COMx -c baudrate=115200
```

### 5. Read results

The sketch prints `[PASS]` or `[FAIL]` for each step. The first `[FAIL]` is the one that matters — everything after it is not reached.

---

## Failure diagnosis guide

### Failure at Step 3 (AUTH TLS rejected)

**Not a transport problem.** The PR4100 didn't accept the TLS upgrade request.

- Check that Explicit TLS is actually enabled on the NAS.
- Some NAS devices label it "FTP with TLS/SSL" or "FTPS with explicit security."
- If only Implicit TLS is available (port 990), the spike needs redesign — but this would also mean Explicit FTPS is not an option for this NAS.

### Failure at Step 4 (TLS handshake failed)

**This is the critical transport test.** Record the Mbed error code.

| Error code | Meaning | Action |
|------------|---------|--------|
| `NSAPI_ERROR_AUTH_FAILURE` (-3012) | Certificate validation failed | Provide `ROOT_CA_PEM`, or build a follow-up spike variant that explicitly customizes the TLS auth mode via Mbed TLS config APIs |
| `NSAPI_ERROR_TIMEOUT` | Handshake took too long | Increase `SOCK_TIMEOUT_MS` to 30000 |
| `NSAPI_ERROR_NO_MEMORY` | Not enough RAM for TLS buffers | Check heap — Opta has 8MB external SDRAM but Mbed TLS buffer allocation may be constrained |
| `NSAPI_ERROR_PARAMETER` | Bad parameter to the wrapper | Check that `TRANSPORT_KEEP` is the right enum value for a socket that was already connected |
| Other negative value | mbedtls-specific error | Cross-reference with `mbedtls/error.h` defines |

**If the handshake fails and cannot be fixed:**
- Option C is dead.
- Try the same spike but using `EthernetSSLClient` (Option B) or `SSLClient` (Option A) for the TLS upgrade step. This would require rewriting Steps 4–8 with the alternate API.

### Failure at Step 8 (data channel TLS handshake failed)

**Session reuse is the most likely cause.** The control channel TLS worked but the data channel's independent handshake was rejected.

1. Check PR4100 logs for an error mentioning "session reuse" or "session ID."
2. Check if `TLSSocketWrapper` has any API for session ticket extraction and injection. Look for methods like `get_session()`, `set_session()`, or constructor options that accept session data.
3. If no session reuse API exists or it doesn't help:
   - Check if the PR4100 has a setting to disable session reuse enforcement.
   - If not, this is a hard interop blocker for Option C.

### Failure at STOR / transfer completion

- If `STOR` is rejected (code >= 400): write permission issue on the NAS.
- If data write returns short: socket was closed prematurely — may be a TLS alert.
- If transfer completion (226) never arrives: the data-channel close didn't signal EOF correctly to the server. This can happen if `TLSSocketWrapper::close()` doesn't send a proper TLS `close_notify`.

---

## What to record

After each spike run, record the following in a brief results note:

1. **Board package version:** `arduino:mbed_opta` version number.
2. **Compile result:** Success, or copy of the first error message.
3. **Each step result:** The `[PASS]`/`[FAIL]` output from Serial.
4. **Handshake timings:** Control-channel and data-channel handshake durations in ms.
5. **The first failure's error code and message.**
6. **PR4100 configuration:** Explicit TLS enabled? Port? Any TLS-specific settings?
7. **PR4100 logs:** Any relevant error messages from the NAS side.
8. **RAM observations:** If Opta provides heap stats, note free memory before and after TLS handshake.

---

## Success criteria

The spike passes if **all** of the following are true:

- [ ] `TCPSocket` connects to PR4100:21 and reads a 220 banner.
- [ ] `AUTH TLS` returns 234.
- [ ] `TLSSocketWrapper` handshake succeeds on the control channel.
- [ ] `PBSZ 0` returns 200 and `PROT P` returns 200 over the encrypted control channel.
- [ ] Login succeeds (`USER` / `PASS`) over the encrypted control channel.
- [ ] `PASV` returns a parseable 227 response.
- [ ] `TLSSocketWrapper` handshake succeeds on the data channel.
- [ ] `STOR` uploads test data and the server returns 226 on completion.

### Bonus (second run):

- [ ] Re-run with `VALIDATE_CERT = true` and `ROOT_CA_PEM` set to the PR4100 cert.
- [ ] Add a `RETR` test after `STOR` to confirm download also works.
- [ ] Note total heap usage delta.

---

## After the spike

### If the spike passes

1. Update `FTPS_IMPLEMENTATION_04132026.md` — mark the Phase 0 spike as complete.
2. Update `FTPS_IMPLEMENTATION_CHECKLIST_04132026.md` — check off the TLSSocketWrapper go/no-go item.
3. Record handshake timing for the timeout budget (finding #19).
4. Record whether data-channel TLS required session reuse (risk #2 in the implementation plan).
5. Proceed to closing the open documentation items (findings #16–18) and then to Phase 1 (config schema).

### If the spike fails at the control-channel handshake

1. Document the exact error code.
2. Do **not** assume Option B (`TLSSocket`) or Option A (`SSLClient`) are protocol-equivalent fallbacks for Explicit FTPS control upgrade; both are TLS-from-connect style paths unless separately proven to adopt an existing socket.
3. Treat Option B / Option A as separate follow-up experiments only. The next realistic direct fallback for Explicit FTPS control upgrade is a lower-level custom Mbed TLS wrapper around the existing control socket, or an explicit product-scope change such as allowing Implicit FTPS.

### If the spike fails at the data-channel handshake only

1. Document the error and check for session reuse.
2. If session reuse is the problem and the PR4100 can be configured to not require it, change the NAS setting and re-run.
3. If session reuse is required and `TLSSocketWrapper` cannot provide it, evaluate whether the Mbed TLS API exposes `mbedtls_ssl_session` for manual session injection. This is high effort but technically possible.

### If the spike fails at compile time

1. Record the exact error.
2. Check whether the include paths need adjustment (e.g., `mbed.h` vs `Mbed.h`, `netsocket/` prefix).
3. If the headers genuinely don't exist in the installed board package, confirm the `mbed_opta` version and check whether a newer version exposes them.
