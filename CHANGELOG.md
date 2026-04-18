# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Web file manager hardening in `examples/WebFileManagerLiveTest`:
  safer placeholder FTPS defaults, non-default harness passcode, and
  boot self-test disabled by default for normal runs.
- Live WebFileManager validation pass on real Opta (`2026-04-18`) against
  local pyftpdlib FTPS: connect, remote list, upload, download, copy,
  move, delete, token-auth enforcement, bad-fingerprint rejection,
  bad-password rejection, and reconnect success after server restart.

### Changed
- Updated README to reflect current `WebFileManagerLiveTest` behavior on
  `arduino:mbed_opta` 4.5.0, including the temporary local-listing
  limitation due missing `<dirent.h>` in the shipped toolchain.

### Fixed
- **FTPS backup hang / watchdog reset on Arduino Opta (Mbed OS)**
  `MbedSecureSocketFtpsTransport::closeData()` and `closeControl()` previously
  routed through Mbed's synchronous `TLSSocketWrapper::close()` +
  `TCPSocket::close()` path, which does NOT honor `set_timeout()` or
  `set_blocking(false)` during the TLS `close_notify` + TCP `FIN` exchange.
  On some FTPS servers this blocked 60+ seconds, tripping the caller's
  watchdog and rebooting the device. The close path is now:

  1. Request abortive close on the underlying `TCPSocket` via
     `setsockopt(NSAPI_LINGER, {l_onoff=1, l_linger=0})`. On platforms
     that honor `SO_LINGER` this turns close into an immediate TCP RST
     instead of a FIN/ACK round-trip. The transport now traces
     `xport:linger-set` on success and
     `xport:linger-unsupported:<nsapi>` when the platform ignores it
     (Arduino Opta returns `-3002` `NSAPI_ERROR_UNSUPPORTED`).
  2. Flip the underlying `TCPSocket` to non-blocking with zero timeout
     as a safety net.
  3. Send TLS `close_notify` directly via
     `mbedtls_ssl_close_notify(tls->get_ssl_context())` so the FTPS
     server emits its final `226 Transfer complete` reply promptly
     without invoking Mbed's blocking close path.
  4. Tear the pair down in this order: **close the `TCPSocket` first,
     then `delete` the `TLSSocketWrapper`, then `delete` the
     `TCPSocket`.** The historical `delete tls` hang is avoided
     because the TCP layer is already in CLOSED state before the TLS
     destructor unwinds its BIO callbacks. Each step emits a trace
     (`xport:cleanup:tcp-close` / `tcp-closed` / `tls-delete` /
     `tls-deleted` / `tcp-delete` / `tcp-deleted`) so any future hang
     can be pinpointed.

  An earlier `FTPS_ABANDON_ON_CLOSE` macro (default `1`) deliberately
  leaked one TLSSocketWrapper per transfer to dodge the historical
  hang. With the reordered cleanup proven safe at runtime, the macro
  now defaults to `0` (full cleanup); the abandon code path is
  retained behind the macro for emergency fallback only.

  Live verification on an Arduino Opta (mbed_opta core 4.5.0):
  - Backup now returns `HTTP 200` in ~8-30 s (was hanging 60-90 s →
    watchdog reboot).
  - The `STOR` final-reply round-trip collapsed from ~15.4 s → ~370 ms
    because the server no longer waits for its own `close_notify`
    timeout before sending `226`.
  - Device remains alive across backup cycles; no watchdog resets.

- **Surface socket-pool exhaustion on data-channel failures.** When
  `socket.open()` or `socket.connect()` fails the transport now traces
  `xport:open-failed:<nsapi>` / `xport:connect-failed:<nsapi>` so the
  integrator can distinguish socket-pool exhaustion (`-3005`
  `NSAPI_ERROR_NO_SOCKET`) from transient network errors.

### Multi-file FTPS backup verified on Arduino Opta (2026-04-17)

Following the close-path fix above, multi-file backup runs to a pyftpdlib
FTPS server were proven end-to-end on real hardware: **8 files uploaded,
0 failed** in a single session. The library-side changes that made this
possible are already itemized in the Fixed section. One additional
integration-side observation surfaced during this work:

- On Arduino Opta the LWIP socket pool is hard-baked at 4 PCBs. The
  `MBED_CONF_LWIP_SOCKET_MAX` / `MBED_CONF_LWIP_TCP_SOCKET_MAX` macros
  in `variants/OPTA/mbed_config.h` cannot be raised because
  `libmbed.a` is shipped as a precompiled archive. Combined with the
  Opta's lack of `SO_LINGER` support (every close goes through
  `TIME_WAIT` for ~60 s), back-to-back FTPS file uploads need
  application-level pacing.

The matching application-side workarounds (release the listening server
socket during backup, wait for `TIME_WAIT` to drain between files) are
integrator concerns, not library concerns, and are described in the
integrating Tank Alarm Server project documentation
(`ArduinoSMSTankAlarm/CODE REVIEW/OPTA_LWIP_BACKUP_RECIPE_04172026.md`).

The earlier "first file succeeds, subsequent files fail with
`ConnectionFailed`" limitation is **resolved** at the library level. The
remaining application-visible cost is throughput: with the integrator
workarounds in place, an Opta backup of N files takes ~`N * 65` seconds
because each file must wait a full `TIME_WAIT` interval before opening
the next data socket.

### Added
- New `PyftpdlibLiveTest` example: end-to-end FTPS test against a bundled pyftpdlib server, including `gen_cert.py` and `ftps_server.py` scripts.
- New `WDMyCloudLiveTest` example: end-to-end FTPS test against a WD My Cloud NAS running My Cloud OS 5, with step-by-step NAS setup guide.

### Changed
- Updated README to reflect hardware-validated status (all 10 test steps pass on real Opta).
- Added static IP configuration option to BasicUpload, BasicDownload, and FileZillaLiveTest examples.
- Added trace callback to FileZillaLiveTest for diagnostic output and watchdog integration.
- Documented static IP and watchdog patterns in README Hardware Notes section.

## [0.1.1] - 2026-04-16

### Fixed
- Improved control-channel resilience in `FtpsClient` command/reply paths (`PASV`, `STOR`, `RETR`, `MKD`, `SIZE`): when command dispatch or reply read fails, the client now closes sockets and marks the session disconnected immediately instead of leaving stale connected state.
- Prevented follow-on transfer attempts from running against a dead FTPS control channel by failing fast with `ConnectionFailed` after control link loss.

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
