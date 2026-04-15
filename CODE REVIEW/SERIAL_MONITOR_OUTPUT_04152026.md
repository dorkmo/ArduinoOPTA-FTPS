# Serial Monitor Output Guide — Opta FTPS Testing

**Date:** April 15, 2026  
**Purpose:** Define the serial-monitor debug output used while validating FTPS behavior on Arduino Opta.

---

## Recommendation

Yes: add serial-monitor diagnostics while testing FTPS on Opta.

But keep it **line-oriented and deterministic**, not an interactive command dialog yet.

At this stage, the highest-value output is:

- clear step boundaries
- explicit pass/fail status
- stable message prefixes
- masked secrets
- output that can be copied directly into test notes

An interactive serial-console command shell can come later if repeated hardware investigation proves it is worth the complexity.

---

## Output Style

Use the following prefixes consistently:

- `[INFO]` — neutral context such as target host, current mode, or setup state
- `[STEP]` — the next operation the sketch is about to attempt
- `[PASS]` — an operation completed successfully
- `[WARN]` — a non-fatal risk or expected limitation
- `[FAIL]` — an operation failed and should include a human-readable reason

Rules:

- One event per line.
- Mask credentials and secrets.
- Include the `FtpsError` numeric value when failure comes from `FtpsClient`.
- Prefer exact protocol step names over vague wording.
- Keep success messages short so failures remain easy to scan.

---

## Debug Output In Current Examples

### `examples/BasicUpload/BasicUpload.ino`

Current upload-example output now includes:

```text
==============================================
  Basic FTPS Upload
==============================================
[INFO] Target FTPS server: <host>:<port>
[INFO] Mode: Explicit FTPS with protected passive transfer.
[STEP] Ethernet initialization
[INFO] IP address: <dhcp-ip>
[STEP] FtpsClient.begin()
[PASS] FtpsClient.begin()
```

It then emits transfer-status lines such as:

```text
[STEP] FtpsClient.connect()
[FAIL] FtpsClient.connect() (FtpsError=<n>): <message>
[PASS] FtpsClient.connect()
[STEP] FtpsClient.store()
[FAIL] FtpsClient.store() (FtpsError=<n>): <message>
[PASS] FtpsClient.store(): uploaded <remote-path>
[STEP] FtpsClient.quit()
[PASS] FtpsClient.quit()
```

### `examples/BasicDownload/BasicDownload.ino`

Current download-example output now includes:

```text
==============================================
  Basic FTPS Download
==============================================
[INFO] Target FTPS server: <host>:<port>
[INFO] Mode: Explicit FTPS with protected passive transfer.
[STEP] Ethernet initialization
[INFO] IP address: <dhcp-ip>
[STEP] FtpsClient.begin()
[PASS] FtpsClient.begin()
```

It then emits transfer-status lines such as:

```text
[STEP] FtpsClient.connect()
[FAIL] FtpsClient.connect() (FtpsError=<n>): <message>
[PASS] FtpsClient.connect()
[STEP] FtpsClient.retrieve()
[FAIL] FtpsClient.retrieve() (FtpsError=<n>): <message>
[PASS] FtpsClient.retrieve() downloaded <bytes> bytes:
[STEP] FtpsClient.quit()
[PASS] FtpsClient.quit()
```

### `examples/FileZillaLiveTest/FileZillaLiveTest.ino`

The FileZilla live-test sketch is the recommended first hardware validation sketch.

Its output follows the same prefix contract and adds:

```text
==============================================
  FileZilla Server Live FTPS Test
==============================================
[INFO] Target FTPS server: <host>:<port>
[INFO] Trust mode: Fingerprint|ImportedCert
[INFO] TLS server name: <name-if-set>
[INFO] Mode: Explicit FTPS with protected passive transfers.
[STEP] Ethernet initialization
[PASS] Ethernet.begin(): <dhcp-ip>
[STEP] FtpsClient.begin()
[PASS] FtpsClient.begin()
[STEP] FtpsClient.connect()
[PASS] FtpsClient.connect()
[STEP] FtpsClient.store()
[PASS] FtpsClient.store(): uploaded <remote-path>
[STEP] FtpsClient.retrieve()
[PASS] FtpsClient.retrieve(): downloaded <bytes> bytes
[STEP] FtpsClient.quit()
[PASS] FtpsClient.quit()
```

### `FtpsSpikeTest/FtpsSpikeTest.ino`

This sketch already uses the preferred structure and remains the reference for transport-level diagnostics:

- `[STEP n]` blocks for each protocol phase
- `[PASS]` and `[FAIL]` markers for each concrete operation
- FTP command and reply echoing
- TLS handshake timing and error-code reporting

---

## Current Library Error Text

The current `FtpsClient` implementation returns explicit error messages instead of generic failures.

Examples:

- `ImportedCert trust with an IP host requires tlsServerName for hostname verification.`
- `Server certificate fingerprint did not match the configured SHA-256 pin.`
- `Data-channel TLS handshake failed; check trust material or session reuse requirements.`
- `The destination buffer was too small for the downloaded file.`

These messages should stay stable enough that hardware test notes remain readable across runs.

---

## Suggested Future Output Refinements

As hardware validation proceeds, extend serial output in this order:

1. Network ready: IP address, link state, DHCP/static status.
2. Control connect: DNS resolve, TCP connect, banner read.
3. TLS upgrade: `AUTH TLS`, control handshake result, trust mode in use.
4. FTPS protection: `PBSZ 0`, `PROT P`.
5. Login flow: `USER`, masked `PASS`, login result.
6. Passive data channel: `PASV`, parsed endpoint, data TLS handshake result.
7. Transfer result: bytes sent/received, final `226` or failure reply.
8. Cleanup: data close, `QUIT`, socket teardown.

---

## Guidance For The First Project `.ino`

For the first project-level `.ino`, reuse the same output contract instead of inventing a second style.

Recommended baseline:

- keep `[INFO]`, `[STEP]`, `[PASS]`, `[WARN]`, `[FAIL]`
- print one startup banner with sketch name and build purpose
- print one-line summaries for network, control TLS, data TLS, and transfer outcome
- include `FtpsError` numeric codes on library failures
- never print raw passwords or full certificate PEM blocks

That gives you a useful Opta-side diagnostic transcript without turning the sketch into a serial-console application prematurely.