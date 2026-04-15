# FTPS Implementation Checklist — TankAlarm Server

**Date:** April 13, 2026  
**Scope:** Planning only; no code changes implemented  
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
> | 1–3 (Config, API, UI) | Phase 1 — config & UI |
> | 4–6 (Transport, Handshake, Data) | Phase 2 — transport & TLS |
> | 7–10 (Helpers, Backup, Client, Archive) | Phase 3 — integration test |
> | 11 (Docs/Cleanup) | Phase 4 — docs & cleanup |

**Current decision baseline:**

- Target **Explicit TLS FTPS** only for the current plan.
- Keep **plain FTP** available during transition unless intentionally removed later.
- Keep **Implicit TLS** out of scope for the current plan.
- Do **not** implement any of the items below in this pass.

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
  - imported PR4100 certificate trust
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
- Imported PR4100 certificate trust should remain available as a fallback if direct fingerprint verification proves awkward in the chosen TLS path.
- Fingerprint mode is now defined as SHA-256 leaf-cert pinning with normalized 64-char uppercase hex storage.
- Certificate validation now defaults to `true`; insecure TLS is debug-only if retained at all.
- Imported-cert mode now assumes clock-aware validation and hostname rules rather than a generic "trust cert somehow" behavior.
- `arduino-cli` is not installed in this environment, so a standalone compile probe was not executed here.

---

## Phase 1 — Config Schema and Defaults

- [ ] Add new FTP security fields to `ServerConfig`.
- [ ] Add a strongly-typed FTPS security-mode enum.
- [ ] Add defaults for:
  - security mode
  - trust mode
  - certificate validation
  - TLS server name
  - pinned fingerprint
- [ ] Default `ftpValidateServerCert` to `true`.
- [ ] If `ftpAllowInsecureTls` is retained in code, default it to `false` and keep it out of the normal UI/API surface.
- [ ] Use canonical imported-cert file paths:
  - `/ftps/server_trust.pem`
  - `/ftps/server_trust.pem.tmp`
- [ ] Store imported PR4100 trust certificate outside the main config JSON.
- [ ] Preserve existing FTP username/password handling and at-rest obfuscation.
- [ ] Ensure legacy configs without FTPS fields still load cleanly.
- [ ] Ensure invalid FTPS fields clamp to safe defaults.
- [ ] Normalize stored fingerprints to 64-char uppercase hex without separators.
- [ ] If `ftpHost` is a hostname and `ftpTlsServerName` is empty, default `ftpTlsServerName` to `ftpHost` on load/save normalization.

### Suggested Fields

- [ ] `ftpSecurityMode`
- [ ] `ftpTlsTrustMode` (`0=fingerprint`, `1=imported-cert`)
- [ ] `ftpValidateServerCert`
- [ ] `ftpTlsServerName`
- [ ] `ftpTlsCertPath` using canonical path metadata (`""` or `/ftps/server_trust.pem`)
- [ ] `ftpTlsFingerprint` (normalized 64-char uppercase SHA-256 leaf-cert fingerprint)
- [ ] `ftpAllowInsecureTls` only if retained as hidden debug-only state

### Files to Touch Later

- [ ] `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
  - config struct definition
  - default initialization path
  - `loadConfig()`
  - `saveConfig()`

### Exit Criteria

- [ ] Old configs load without breaking.
- [ ] New configs persist FTPS metadata.
- [ ] Existing FTP secrets still round-trip correctly.

---

## Phase 2 — Settings API Surface

- [ ] Add FTPS fields to the settings/status JSON returned to the web UI.
- [ ] Add FTPS fields to the settings-save JSON parser.
- [ ] Update any alternate/legacy settings parser path that still accepts FTP fields.
- [ ] Add validation for:
  - mode enum values
  - port range
  - required trust settings when validation is enabled
- [ ] Prevent silent downgrade from FTPS to plain FTP when invalid FTPS config is submitted.
- [ ] Add dedicated import/replace/clear handling for imported PR4100 trust certificates.

### API Behavior Decisions

- [ ] If `securityMode == explicit TLS` and port is omitted, default to `21`.
- [ ] If `trustMode == fingerprint`, require a fingerprint and normalize it to 64-char uppercase hex.
- [ ] If `trustMode == imported-cert`, require an imported cert to be present.
- [ ] Use exact trust-mode API tokens:
  - `fingerprint`
  - `imported-cert`
- [ ] Fingerprint entry should work through the normal settings-save API.
- [ ] PEM import should use a dedicated endpoint that accepts certificate text and stores it internally at the canonical path.
- [ ] PEM import should accept only one PEM certificate block in v1.
- [ ] PEM import should reject payloads larger than `4096` bytes after normalization.
- [ ] PEM import should preserve the currently stored certificate if parse/write/rename fails.
- [ ] If `ftpHost` is an IP and `trustMode == imported-cert`, require `tlsServerName` unless the certificate SAN explicitly includes that IP.
- [ ] If certificate validation is enabled but trust material is missing, reject save or mark config invalid.
- [ ] Return stable FTPS-specific error categories instead of generic `FTP failed` where possible.

### Files to Touch Later

- [ ] `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
  - settings/status JSON serialization
  - settings POST handlers
  - any legacy server-settings parser

### Exit Criteria

- [ ] UI can read and write FTPS settings.
- [ ] Invalid FTPS payloads fail cleanly.

---

## Phase 3 — Server Settings UI

- [ ] Add a security-mode selector to the embedded Server Settings page.
- [ ] Add a certificate-validation toggle.
- [ ] Add a trust-mode selector.
- [ ] Add TLS server-name input.
- [ ] Add certificate fingerprint input.
- [ ] Add imported PR4100 certificate paste/import control.
- [ ] Add imported certificate present / replace / clear state.
- [ ] Add help text explaining:
  - Explicit TLS is preferred
  - Passive mode is still required
  - enabling cert validation without correct trust info will block connections
- [ ] Make the JS load and save the new FTPS fields.
- [ ] Keep existing FTP password handling behavior intact.

### UX Decisions

- [ ] Auto-fill default port when security mode changes.
- [ ] Show warning if user selects FTPS but leaves validation disabled.
- [x] Do not expose Implicit TLS in the UI for this plan.
- [ ] Make local web-UI trust enrollment the primary workflow.
- [ ] Support direct fingerprint paste in the settings page.
- [ ] Support PEM paste/import in the settings page without exposing filesystem paths.
- [ ] Accept fingerprint input with optional separators, but display/store the normalized format consistently.
- [ ] If `ftpHost` is a hostname and `tlsServerName` is blank, default the TLS server name automatically.
- [ ] If `trustMode == imported-cert` and time is not synced, show a specific validation warning before save or test.
- [ ] If a file-upload control is added later, treat it as convenience only; store internally at the canonical path.
- [ ] Treat `fetch presented certificate` as an optional assisted-enrollment enhancement, not a v1 requirement.
- [ ] Treat Notehub-based trust provisioning as deferred remote-management work, not a v1 requirement.

### Files to Touch Later

- [ ] `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`
  - embedded HTML
  - embedded JS DOM cache
  - settings load logic
  - settings save logic

### Exit Criteria

- [ ] The UI can fully configure the FTPS mode chosen for v1.
- [ ] The UI does not break existing plain-FTP deployments.

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

- [ ] `FtpSession`
- [ ] socket ownership/lifetime management
- [ ] error propagation for handshake/validation failures

### Files to Touch Later

- [ ] `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`

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

- [ ] Update `ftpQuit()` for secure control-channel shutdown.
- [ ] Review timeout values for TLS handshakes and slower NAS responses.
- [ ] Review buffer sizes for TLS overhead and archive-size cases.
- [ ] Review watchdog kicks around TLS-heavy operations.
- [ ] Add clear serial/API diagnostics for FTPS-specific failures.
- [ ] Keep FTPS error categories stable across serial logs, API responses, and UI toasts where machine-readable status already exists.

### Exit Criteria

- [ ] Helper layer is stable enough to be reused across all existing FTP call sites.

---

## Phase 8 — Manual Backup and Restore Paths

- [ ] Validate `performFtpBackupDetailed()` over FTPS.
- [ ] Validate `performFtpRestoreDetailed()` over FTPS.
- [ ] Confirm error reporting remains readable in the API/UI.
- [ ] Confirm partial-failure reporting still works.

### Files to Touch Later

- [ ] `TankAlarm-112025-Server-BluesOpta/TankAlarm-112025-Server-BluesOpta.ino`

### Exit Criteria

- [ ] Manual backup and restore succeed against the PR4100 in Explicit TLS mode.

---

## Phase 9 — Client Config Backup/Restore Helpers

- [ ] Validate manifest upload over FTPS.
- [ ] Validate per-client config upload over FTPS.
- [ ] Validate manifest download over FTPS.
- [ ] Validate per-client config restore over FTPS.

### Call Sites to Re-Test Later

- [ ] `ftpBackupClientConfigs()`
- [ ] `ftpRestoreClientConfigs()`

### Exit Criteria

- [ ] All client config cache backup/restore flows still work over FTPS.

---

## Phase 10 — Cold-Tier History and Archive Flows

- [ ] Validate monthly archive upload over FTPS.
- [ ] Validate monthly archive download over FTPS.
- [ ] Validate cached archive-history load path over FTPS.
- [ ] Validate archived client JSON export over FTPS.
- [ ] Validate browser archive-download endpoint over FTPS-backed retrieval.

### Call Sites to Re-Test Later

- [ ] `archiveMonthToFtp()`
- [ ] archived month loader / cache path
- [ ] archived client export uploader
- [ ] FTP-backed archive download endpoint

### Exit Criteria

- [ ] Cold-tier history remains fully functional after the FTPS transport swap.

---

## Phase 11 — Documentation and Cleanup

- [ ] Update README backup-security wording.
- [ ] Replace comments that still say secure transport is future work.
- [ ] Add operator guidance for PR4100 setup:
  - select Explicit TLS
  - keep passive mode enabled
  - capture fingerprint or import the PR4100 PEM trust certificate
- [ ] Add release-note summary for FTPS support.

### Exit Criteria

- [ ] User-facing docs and code comments match actual behavior.

---

## Test Checklist

## PR4100 Interoperability

- [ ] Explicit TLS enabled on PR4100
- [ ] Passive mode enabled
- [ ] Valid username/password configured
- [ ] Compatible TLS version / cipher suite negotiated with the chosen Opta transport
- [ ] No client certificate / mTLS requirement blocks the session
- [ ] Cert fingerprint captured correctly
- [ ] Imported PR4100 trust cert path tested, if that trust mode is enabled
- [ ] Backup succeeds
- [ ] Restore succeeds

## Negative Cases

- [ ] Wrong password returns auth-specific failure
- [ ] Invalid fingerprint format is rejected before connect
- [ ] Wrong fingerprint returns certificate-specific failure
- [ ] Wrong or stale imported trust cert returns certificate-specific failure
- [ ] Oversized PEM import is rejected cleanly
- [ ] Malformed PEM import is rejected cleanly without overwriting the existing cert
- [ ] Imported-cert mode with unsynced clock fails with certificate-time-specific error
- [ ] Imported-cert mode with IP host and missing required `tlsServerName` fails clearly
- [ ] TLS required on PR4100 while TankAlarm is set to plain FTP fails closed
- [ ] FTPS selected with no trust data fails clearly when validation is enabled
- [ ] Missing host or port still fails with clear message

## Stability

- [ ] Repeated backup cycles do not leak memory
- [ ] Watchdog does not fire during handshake + transfer
- [ ] Large archive downloads still complete within timeout
- [ ] Failure paths always close sockets cleanly

---

## Deferred Items

- [ ] Revisit Implicit TLS only if a later compatibility requirement emerges
- [ ] Broader CA-bundle trust UX beyond fingerprint pinning and imported PR4100 cert trust
- [ ] Dedicated “Test FTPS Connection” UI button
- [ ] Automatic certificate-rotation UX
- [ ] Removal of plain FTP mode after transition period

---

## Recommended Order of Work Later

1. Run the `TLSSocketWrapper` go/no-go spike and either confirm it or select the documented fallback.
2. Mirror the locked trust/validation defaults in code.
3. Add schema/defaults and API fields.
4. Add UI controls.
5. Refactor `FtpSession` and low-level helpers.
6. Implement Explicit TLS control-channel flow.
7. Implement TLS passive data-channel flow.
8. Re-test manual backup/restore.
9. Re-test client config manifest flows.
10. Re-test cold-tier archive flows.
11. Update docs and release notes.

---

## Resume Point

When work resumes, start at **Phase 0** and do not begin code changes until the TLS client/library choice is settled. That is the single biggest technical uncertainty in the FTPS migration.
