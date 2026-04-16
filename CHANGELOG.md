# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Changed
- Updated README to reflect hardware-validated status (all 10 test steps pass on real Opta).
- Added static IP configuration option to BasicUpload, BasicDownload, and FileZillaLiveTest examples.
- Added trace callback to FileZillaLiveTest for diagnostic output and watchdog integration.
- Documented static IP and watchdog patterns in README Hardware Notes section.

## [0.1.0] - 2026-04-16

### Fixed
- Data and control socket lifecycle: `closeData()` and `closeControl()` now delete both the TLS wrapper and the underlying `TCPSocket`, preventing a hard fault on Mbed OS when a closed socket was reused for a subsequent transfer or reconnect.
- TLS close blocking: data and control TLS sockets are given a 3-second timeout before `close()` to prevent indefinite hangs during the TLS shutdown handshake.

### Changed
- Moved trace callback responsibility entirely to `FtpsClient`; removed transport-layer trace phases to keep diagnostic output clean and high-level.
- Cleaned up minor indentation inconsistencies in `FtpsClient.cpp`.

### Added
- Initial repository structure with design documents.
- Phase 0 transport spike plan.
- Library scaffolding (headers, types, transport interface).
- Serial monitor output guide for FTPS examples and Opta testing.
- First-pass FTPS implementation for Explicit TLS, protected passive upload, and protected passive download.
- `src/FtpsTrust.cpp` with fingerprint normalization/comparison and PEM validation helpers.
- `examples/FileZillaLiveTest/FileZillaLiveTest.ino` for first live Opta-to-FileZilla validation.
- `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino` with a lightweight Opta-hosted LAN UI for FTPS config input, action triggering, and live status/log monitoring.
- `CODE REVIEW/WEB_HARNESS_API_REFERENCE_04152026.md` documenting harness auth flow, endpoint contracts, and ready-to-run PowerShell/cURL examples for scripted testing.
- `CODE REVIEW/APPLICATION_INTEGRATION_REQUIREMENTS_04152026.md` documenting the generic Arduino application integration requirements and the near-term library additions needed to support them.
- `CODE REVIEW/HARDWARE_AND_FOLLOWUP_CHECKLIST_04152026.md` consolidating the remaining hardware validation and post-validation update work.
- `FtpsClient::mkd()` and `FtpsClient::size()` for remote directory creation and remote size preflight.

### Changed
- Updated README status language to reflect scaffold-present, implementation-pending reality.
- Updated implementation note wording and findings status to align with current `IFtpsTransport` scaffold and diagnostics hooks.
- Updated implementation checklist semantics for existing examples and `quit()`-based cleanup expectations.
- Updated repository review transport interface signatures and file-path references to match current `src/transport/` scaffolding.
- Updated spike-plan sample text to use `MbedSecureSocketFtpsTransport` naming and current test directory path conventions.
- Tightened `FtpsServerConfig` to the currently supported v1 surface by removing unsupported public security and passive-mode toggles.
- Replaced scaffold `FtpsClient` behavior with an initial Explicit FTPS implementation using `TCPSocket` and `TLSSocketWrapper`.
- Wired transport diagnostics to expose peer certificate fingerprints and TLS error codes.
- Converted the upload/download examples from scaffold messaging to working FTPS examples with structured serial diagnostics.
- Updated the README and serial monitor guide to describe the implemented state and the new FileZilla live-test path.
- Hardened `FtpsClient` command/transfer reliability with truncation checks, would-block-aware I/O retries, PASV parser range validation, and control-reply draining on transfer-failure paths.
- Changed `FtpsClient` connection config handling to use owned internal string storage instead of shallow pointer copies from caller-owned buffers.
- Added stricter FTPS security behavior in `connect()`: non-zero port requirement, explicit trust-mode validation, and fail-closed enforcement for `validateServerCert`.
- Improved TLS failure mapping for data-channel setup to surface `SessionReuseRequired` when the data handshake fails with `NSAPI_ERROR_AUTH_FAILURE`.
- Updated example sketches to report `quit()` status using `lastError()` consistently.
- Updated README trust-model wording to reflect required certificate validation in the current build.
- Strengthened `ftpsTrustValidatePem()` by adding Mbed TLS X.509 parsing so malformed PEM content is rejected before handshake time.
- Added best-effort TLS session reuse hinting in `MbedSecureSocketFtpsTransport` by caching the control-channel TLS session and applying it to data-channel handshakes when available.
- Reduced transport heap churn by reusing allocated `TCPSocket` instances across reconnects/transfers instead of deleting and reallocating on every close.
- Added an Mbed TLS 3.x compatibility guard for peer-certificate fingerprint extraction paths.
- Enhanced `WebHarnessLiveTest` to use POST for config updates, add a lightweight passcode/token auth gate for protected routes, and expose a downloadable text report endpoint for run logs and status snapshots.
- Updated the implementation checklist and README to track near-term integration requirements for nested remote path creation, remote size preflight, and conditional follow-up work around keepalives, watchdog hooks, and streaming transfers.
- Refactored `FtpsClient` arg-based FTP command handling so `USER`, `PASS`, `STOR`, `RETR`, `MKD`, and `SIZE` share command-formatting and dispatch logic.
- Extended `examples/FileZillaLiveTest/FileZillaLiveTest.ino` to exercise `mkd()` and `size()` alongside upload and download flows.
