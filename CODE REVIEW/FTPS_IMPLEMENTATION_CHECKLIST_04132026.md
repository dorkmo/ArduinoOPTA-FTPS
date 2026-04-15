# FTPS Implementation Checklist — Arduino Opta FTPS Library

**Date:** April 13, 2026  
**Scope:** Planning checklist with initial implementation landed; hardware validation and hardening still pending  
**Related Note:** `FTPS_IMPLEMENTATION_04132026.md`

---

## Purpose

This checklist converts the FTPS design note into an ordered implementation plan that can be executed later. It is intentionally action-oriented and split into small verification points so the work can be resumed without re-discovering the scope.

> **Phase-numbering note (04142026):** This checklist uses 12 granular phases
> (0–11) while the main implementation plan (`FTPS_IMPLEMENTATION_04132026.md`)
> uses 5 broader phases.  The mapping is:
>
> | Checklist phase | Implementation-plan section |
> |---|---|
> | 0 (Pre-decisions) | Phase 0 — spike / decisions |
> | 1–3 (Config, API, Trust) | Phase 1 — config & API |
> | 4–6 (Transport, Handshake, Data) | Phase 2 — transport & TLS |
> | 7–10 (Helpers, Upload/Download, Examples, Stability) | Phase 3 — integration test |
> | 11 (Docs/Cleanup) | Phase 4 — docs & cleanup |

**Current decision baseline:**

- Target **Explicit TLS FTPS** only for the current plan.
- Keep **plain FTP** available during transition unless intentionally removed later.
- Keep **Implicit TLS** out of scope for the current plan.
- Use the remaining items below as validation and hardening work for the first implementation pass.

---

## Phase 0 — Pre-Implementation Decisions

- [x] Current planned implementation target: `TLSSocketWrapper` / secure-socket path.
- [ ] Run a compile/device go-no-go spike for `TLSSocketWrapper` before broad schema/UI refactor.
- [ ] Confirm the exact TLS-capable Ethernet client/library to use on Arduino Opta.
- [x] Confirm the installed Opta core exposes `NetworkInterface`, `TCPSocket`, `TLSSocket`, and `TLSSocketWrapper` headers to sketch builds.
- [x] Confirm the bundled Ethernet library exposes `Ethernet.getNetwork()` for access to the underlying Mbed network interface.
- [x] Confirm the bundled `EthernetSSLClient` is already TLSSocket-based.
- [ ] Verify that the chosen TLS client supports both:
  - control-channel TLS
  - passive data-channel TLS
  - passive data-channel TLS session reuse/resumption behavior required by the target server
- [x] Current plan: keep Plain FTP available during transition, but implement FTPS support only for Explicit TLS.
- [x] Current v1 trust-model plan:
  - fingerprint pinning
  - imported PEM certificate trust
- [x] Lock the exact trust-mode enum values and canonical imported-cert path in the design docs.
- [x] Lock v1 fingerprint semantics:
  - SHA-256 leaf-certificate fingerprint
  - normalize to 64-char uppercase hex without separators
- [x] Decide that insecure TLS, if retained at all, is debug-only and hidden from the normal UI.
- [x] Decide that certificate validation defaults to `true`.
- [x] Lock clock/hostname behavior for fingerprint and imported-cert trust modes.
- [x] Capture the current PR4100 assumptions for v1.
- [ ] Verify the PR4100 assumptions on the actual NAS/device.

### Exit Criteria

- [ ] `TLSSocketWrapper` spike succeeds or a documented fallback path is chosen.
- [ ] A specific TLS client/library is named.
- [x] The certificate trust model is chosen.
- [x] The initial feature scope is narrowed to Explicit TLS.

### Current verified findings

- Verified against installed Arduino Opta core `mbed_opta 4.5.0`.
- API exposure is confirmed; the remaining uncertainty is compile/runtime behavior, not header availability.
- The bundled `EthernetSSLClient` confirms TLSSocket support, but it does **not** replace the need to evaluate `TLSSocketWrapper` for Explicit FTPS `AUTH TLS` upgrade behavior.
- Imported PEM certificate trust should remain available as a fallback if direct fingerprint verification proves awkward in the chosen TLS path.
- Fingerprint mode is now defined as SHA-256 leaf-cert pinning with normalized 64-char uppercase hex storage.
- Certificate validation now defaults to `true`; insecure TLS is debug-only if retained at all.
- Imported-cert mode now assumes clock-aware validation and hostname rules rather than a generic "trust cert somehow" behavior.
- `arduino-cli` is not installed in this environment, so a standalone compile probe was not executed here.

---

## Phase 1 — Library Config Types and Defaults

- [x] Define `FtpsServerConfig` struct with FTPS fields. *(Done in `src/FtpsTypes.h`.)*
- [x] Define a strongly-typed FTPS trust-mode enum. *(`FtpsTrustMode` in `src/FtpsTypes.h`.)*
- [x] Keep the public config fixed to Explicit FTPS until additional modes are implemented. *(`FtpsServerConfig` no longer exposes `securityMode`; Explicit FTPS is implicit in the v1 API surface.)*
- [x] Add defaults for:
  - trust mode — `Fingerprint`
  - certificate validation — `true`
  - TLS server name — `nullptr`
  - pinned fingerprint — `nullptr`
- [x] Default `validateServerCert` to `true`.
- [x] Keep passive mode fixed in the public config until non-passive transfers exist. *(`FtpsServerConfig` no longer exposes `passiveMode`; passive transfers remain the only planned v1 behavior.)*
- [ ] If a debug-only `allowInsecureTls` flag is retained, default it to `false` and keep it out of the public API surface.
- [ ] Use canonical imported-cert conventions:
  - `/ftps/server_trust.pem`
  - `/ftps/server_trust.pem.tmp`
- [ ] Store imported trust certificate outside any main config JSON if persisted by the host application.
- [ ] Ensure invalid FTPS fields clamp to safe defaults.
- [ ] Normalize stored fingerprints to 64-char uppercase hex without separators.
- [ ] If `host` is a hostname and `tlsServerName` is empty, default `tlsServerName` to `host`.

### Suggested Fields

- [x] `trustMode` (`0=fingerprint`, `1=imported-cert`)
- [x] `validateServerCert`
- [x] `tlsServerName`
- [x] `rootCaPem` or cert path using canonical path metadata
- [x] `fingerprint` (normalized 64-char uppercase SHA-256 leaf-cert fingerprint)

### Files to Touch Later

- [x] `src/FtpsTypes.h` — config struct and enum definitions *(scaffolded)*
- [x] `src/FtpsClient.h` — public API header *(scaffolded with `begin()`, `lastError()`)*
- [x] `src/FtpsClient.cpp` — default initialization and validation logic *(stub bodies in place)*

### Exit Criteria

- [ ] Config types compile cleanly.
- [x] Default values produce safe behavior. *(Explicit FTPS by fixed v1 behavior, validate cert on, fingerprint mode.)*
- [ ] Host applications can persist and reload FTPS metadata correctly.

---

## Phase 2 — Public API Surface

- [x] Finalize the public `FtpsClient` class surface: `begin()`, `connect()`, `store()`, `retrieve()`, `quit()`, and `lastError()`. *(Decision: `begin()` added for transport init; `lastError()` added for programmatic errors; see Implementation doc §Pre-Implementation Design Decisions.)*
- [x] Define the `FtpsError` enum for structured error reporting. *(Done: includes `NetworkNotInitialized`, `PassiveModeRejected`, `ConnectionFailed` alongside original codes.)*
- [x] Define transport ownership model. *(`FtpsClient` owns `IFtpsTransport` internally; no DI for v1.)*
- [ ] Add validation for:
  - trust-mode enum values
  - port range
  - required trust settings when validation is enabled
- [ ] Prevent silent downgrade from FTPS to plain FTP when invalid config is provided.
- [ ] Add dedicated import/replace/clear handling for imported trust certificates.

### API Behavior Decisions

- [ ] If port is omitted, default to `21`.
- [ ] If `trustMode == fingerprint`, require a fingerprint and normalize it to 64-char uppercase hex.
- [ ] If `trustMode == imported-cert`, require a PEM trust certificate to be provided.
- [ ] Use exact trust-mode enum values:
  - `FtpsTrustMode::Fingerprint`
  - `FtpsTrustMode::ImportedCert`
- [ ] PEM trust should accept only one PEM certificate block in v1.
- [ ] PEM trust should reject payloads larger than `4096` bytes after normalization.
- [ ] If `host` is an IP and `trustMode == imported-cert`, require `tlsServerName` unless the certificate SAN explicitly includes that IP.
- [ ] If certificate validation is enabled but trust material is missing, fail with a specific error.
- [ ] Return stable FTPS-specific error categories instead of generic `FTP failed` where possible.

### Files to Touch Later

- [ ] `src/FtpsClient.h` — public class definition
- [ ] `src/FtpsClient.cpp` — implementation
- [ ] `src/FtpsErrors.h` — error enum

### Exit Criteria

- [ ] Host applications can configure and use the library through the public API.
- [ ] Invalid FTPS configurations fail cleanly with specific errors.

---

## Phase 3 — Trust Configuration

- [ ] Implement SHA-256 fingerprint pinning validation.
- [ ] Implement imported PEM certificate trust validation.
- [ ] Support certificate present / replace / clear state management.
- [ ] Document trust setup:
  - Explicit TLS is preferred
  - Passive mode is still required
  - Enabling cert validation without correct trust info will block connections
- [ ] Accept fingerprint input with optional separators, but store the normalized format consistently.
- [ ] If `host` is a hostname and `tlsServerName` is blank, default the TLS server name automatically.
- [ ] If `trustMode == imported-cert` and time is not synced, fail with a certificate-time-specific error.
- [ ] Treat `fetch presented certificate` as an optional assisted-enrollment enhancement, not a v1 requirement.

### Files to Touch Later

- [ ] `src/FtpsTrust.h` — trust mode definitions and helpers
- [ ] `src/FtpsClient.cpp` — trust validation during connect

### Exit Criteria

- [ ] Both trust modes work when correctly configured.
- [ ] Missing or invalid trust material fails with a specific error.

---

## Phase 4 — Low-Level Transport Abstraction

- [ ] Replace the raw plain-FTP-only `FtpSession` design with a transport-aware session.
- [ ] Introduce an abstraction for control and data sockets.
- [ ] Ensure the abstraction can represent:
  - plain socket
  - TLS-upgraded control socket
  - TLS-wrapped passive data socket
- [ ] Keep the rest of the FTP helper call signatures as stable as possible.

### Core Refactor Targets

- [ ] `FtpSession` / transport ownership model
- [ ] socket ownership/lifetime management
- [ ] error propagation for handshake/validation failures

### Files to Touch Later

- [ ] `src/transport/IFtpsTransport.h`
- [ ] `src/transport/MbedSecureSocketFtpsTransport.h`
- [ ] `src/transport/MbedSecureSocketFtpsTransport.cpp`

### Exit Criteria

- [ ] The FTP helper layer no longer assumes `EthernetClient` is always plaintext.

---

## Phase 5 — Explicit TLS Handshake Flow

- [ ] Implement `AUTH TLS` on the control channel.
- [ ] Upgrade the control channel to TLS after `AUTH TLS` succeeds.
- [ ] Add certificate validation logic.
- [ ] Implement `PBSZ 0`.
- [ ] Implement `PROT P`.
- [ ] Ensure login happens only after the TLS control channel is established.

### Must-Have Failure Cases

- [ ] `AUTH TLS` rejected
- [ ] TLS handshake failure
- [ ] missing trust material
- [ ] certificate mismatch
- [ ] certificate time invalid / unsynced clock
- [ ] certificate hostname mismatch
- [ ] `PBSZ 0` failure
- [ ] `PROT P` failure

### Functions to Update Later

- [ ] `ftpConnectAndLogin()`
- [ ] any helper used by `ftpReadResponse()` / `ftpSendCommand()` if the socket abstraction changes

### Exit Criteria

- [ ] Control channel connects and authenticates over Explicit TLS.
- [ ] Errors are specific enough to debug field failures.

---

## Phase 6 — Passive Data Channel Protection

- [ ] Keep passive mode as the only supported transfer mode.
- [ ] Use the returned `PASV` endpoint to open a **TLS-protected** data socket when FTPS is selected.
- [ ] Ensure `STOR` works with protected data channel.
- [ ] Ensure `RETR` works with protected data channel.
- [ ] Verify final transfer-completion reply still behaves correctly after TLS data close.

### Functions to Update Later

- [ ] `ftpEnterPassive()`
- [ ] `ftpStoreBuffer()`
- [ ] `ftpRetrieveBuffer()`

### Exit Criteria

- [ ] A protected data channel works for both upload and download.

---

## Phase 7 — Common FTP Helper Validation

- [ ] Implement `quit()` for secure control-channel shutdown.
- [ ] Review timeout values for TLS handshakes and slower server responses.
- [ ] Review buffer sizes for TLS overhead.
- [ ] Add clear serial diagnostics for FTPS-specific failures.
- [ ] Keep FTPS error categories stable across serial logs and API responses.

### Exit Criteria

- [ ] Helper layer is stable enough to be reused across upload and download paths.

---

## Phase 8 — Upload and Download Validation

- [ ] Validate `store()` over FTPS against the reference server.
- [ ] Validate `retrieve()` over FTPS against the reference server.
- [ ] Confirm error reporting remains readable.
- [ ] Confirm partial-failure reporting works.

### Files to Touch Later

- [ ] `src/FtpsClient.cpp` — upload/download implementations

### Exit Criteria

- [ ] Upload and download both succeed against the reference server in Explicit TLS mode.

---

## Phase 9 — Example Sketches

- [ ] Validate the upload example compiles and runs.
- [ ] Validate the download example compiles and runs.
- [ ] Validate the fingerprint trust example.
- [ ] Validate the imported PEM trust example.

### Examples currently present

- [x] `examples/BasicUpload/`
- [x] `examples/BasicDownload/`
- [x] `FtpsSpikeTest/FtpsSpikeTest.ino`
- [ ] Add and validate a dedicated fingerprint trust example sketch.
- [ ] Add and validate a dedicated imported PEM trust example sketch.

### Exit Criteria

- [ ] All example sketches compile cleanly for `arduino:mbed_opta`.
- [ ] At least upload and download examples verified on hardware.

---

## Phase 10 — Reconnection and Stability

- [ ] Validate repeated connect/transfer/quit cycles.
- [ ] Validate behavior after temporary network interruption.
- [ ] Validate that `quit()` and failure paths clean up all resources.
- [ ] Confirm no memory leaks across multiple transfer cycles.

### Exit Criteria

- [ ] Library is stable for repeated use in long-running sketches.

---

## Phase 11 — Documentation and Cleanup

- [ ] Update README with tested status.
- [ ] Replace comments that say transport is unvalidated.
- [ ] Add operator guidance for reference server setup:
  - select Explicit TLS
  - keep passive mode enabled
  - capture fingerprint or import PEM trust certificate
- [ ] Update CHANGELOG.md with release notes.
- [ ] Finalize `library.properties` metadata for first release.

### Exit Criteria

- [ ] User-facing docs and code comments match actual behavior.

---

## Test Checklist

## Reference Server Interoperability

- [ ] Explicit TLS enabled on reference server
- [ ] Passive mode enabled
- [ ] Valid username/password configured
- [ ] Compatible TLS version / cipher suite negotiated with the chosen Opta transport
- [ ] No client certificate / mTLS requirement blocks the session
- [ ] Cert fingerprint captured correctly
- [ ] Imported PEM trust cert path tested, if that trust mode is enabled
- [ ] Upload succeeds
- [ ] Download succeeds

## Negative Cases

- [ ] Wrong password returns auth-specific failure
- [ ] Invalid fingerprint format is rejected before connect
- [ ] Wrong fingerprint returns certificate-specific failure
- [ ] Wrong or stale imported trust cert returns certificate-specific failure
- [ ] Oversized PEM import is rejected cleanly
- [ ] Malformed PEM import is rejected cleanly without overwriting the existing cert
- [ ] Imported-cert mode with unsynced clock fails with certificate-time-specific error
- [ ] Imported-cert mode with IP host and missing required `tlsServerName` fails clearly
- [ ] TLS required on server while client is configured for plain FTP fails closed
- [ ] FTPS selected with no trust data fails clearly when validation is enabled
- [ ] Missing host or port still fails with clear message

## Stability

- [ ] Repeated transfer cycles do not leak memory
- [ ] TLS handshake + transfer completes within timeout budget
- [ ] Large downloads still complete within timeout
- [ ] Failure paths always close sockets cleanly

---

## Deferred Items / v2 Candidates

These items are intentionally deferred so the first release stays narrow and verifiable.

- [ ] Add Implicit FTPS as a v2 protocol-expansion target after Explicit FTPS is stable
- [ ] Evaluate active mode as a v2 compatibility feature after passive-mode interoperability is proven
- [ ] Add capability discovery and directory helpers such as `FEAT`, `PWD`, `MLSD`, and `NLST`
- [ ] Add file-management helpers such as `DELE`, `RNFR`/`RNTO`, `MKD`, and `RMD`
- [ ] Add stream-based transfer APIs for larger payloads
- [ ] Broader CA-bundle trust options beyond fingerprint pinning and imported PEM trust
- [ ] Dedicated "Test FTPS Connection" example or helper
- [ ] Automatic certificate-rotation support
- [ ] Expand compatibility testing and publish documented server notes beyond the initial reference server

---

## Recommended Order of Work

1. Run the `TLSSocketWrapper` go/no-go spike and either confirm it or select the documented fallback.
2. Define library config types and defaults.
3. Implement the transport abstraction.
4. Implement Explicit TLS control-channel flow.
5. Implement TLS passive data-channel flow.
6. Implement upload and download primitives.
7. Validate trust modes (fingerprint and imported PEM).
8. Write and validate example sketches.
9. Test reconnection and stability.
10. Update docs and publish release notes.

---

## Resume Point

When functional FTPS implementation work resumes, start at **Phase 0** and avoid transport-dependent implementation changes until the TLS client/library choice is settled. That is the single biggest technical uncertainty in the FTPS library.
