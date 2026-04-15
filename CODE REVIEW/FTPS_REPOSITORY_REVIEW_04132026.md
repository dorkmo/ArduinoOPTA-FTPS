# Arduino Opta FTPS Repository Creation Plan

**Date:** April 14, 2026  
**Scope:** Bootstrap and release plan for a standalone general-purpose Explicit FTPS client library for Arduino Opta  
**Status:** Updated after the April 14 implementation-plan, checklist, and spike-plan reviews  
**Current recommendation:** Repository bootstrap is complete. Use this document as an implementation-readiness tracker, and keep the first working transport implementation and any public release gated on the Opta Phase 0 spike.

---

## Executive Summary

This repository is an **Opta-first FTPS library project**.

The repository goal is to:

1. provide a reusable Explicit FTPS client layer for **Arduino Opta** devices
2. keep the public API focused on FTPS transport and transfer behavior, not any particular application's workflow logic
3. validate the first implementation against the PR4100 as the initial reference server
4. remain general enough to support other standards-compliant FTPS servers later

Publishing a release or claiming working FTPS support should wait until the Opta transport spike succeeds.

The repository now includes:

- planning docs
- tracked TODOs
- proposed API and file structure
- working library sources, live-test examples, and a full spike sketch
- metadata scaffolding

The first actual backend implementation is now in the repository. The main remaining gate is live Opta validation against the target servers.

---

## Current Status

### Decisions already locked

- [x] Arduino Opta is the primary target for v1.
- [x] The library should be **Explicit FTPS** only for v1.
- [x] Passive mode only for v1.
- [x] Binary transfer support only for v1.
- [x] The preferred backend remains the Mbed secure-socket path:
  - `Ethernet.getNetwork()`
  - `NetworkInterface`
  - `TCPSocket`
  - `TLSSocketWrapper`
- [x] Two trust modes are locked for v1:
  - SHA-256 fingerprint pinning
  - imported PEM certificate trust
- [x] The library should fail closed when FTPS is selected.
- [x] PR4100 remains the initial validation target, but the repo should be positioned for general Arduino Opta FTPS use.

### Verified technical facts

- [x] Installed Opta core research confirmed exposure of `NetworkInterface`, `TCPSocket`, `TLSSocket`, and `TLSSocketWrapper` in `mbed_opta 4.5.0`.
- [x] The current spike-plan samples were corrected to use `TLSSocketWrapper::TRANSPORT_KEEP`.
- [x] The implementation docs now treat data-channel TLS session reuse/resumption as a **server-dependent interop risk**, not an unconditional assumption.
- [x] The repo and implementation docs are now aligned on trust-mode enum ordinals:
  - `0 = Fingerprint`
  - `1 = ImportedCert`

### Current blockers and open items

| Item | Priority | Status | What must happen |
|------|----------|--------|------------------|
| Opta transport spike (`TLSSocketWrapper`) | Blocking | Open | Run the real Opta-to-PR4100 `AUTH TLS` / `PBSZ 0` / `PROT P` / `PASV` / `STOR` or `RETR` spike. |
| Data-channel TLS session reuse | High | Open | Verify whether the target server requires reuse/resumption and whether the chosen backend can satisfy it. |
| Transport diagnostics hooks | High | Open | Add peer-cert fingerprint and TLS-error exposure to the transport boundary before locking the public API. |
| `ftpReadResponse()` implementation model | High | Open | Implement response reading using `IFtpsTransport &` instead of `EthernetClient &`. |
| Data-channel lifetime management | High | Open | Manage data sockets inside the transport object rather than in per-function locals. |
| TLS timeout budget | Medium | Open | Split or raise timeouts for TLS control/data handshakes based on spike timings. |
| Public support statement | Medium | Open | Document clearly that PR4100 is the first tested server, not the only intended server. |

---

## Repository Posture

### What can be done now

The new repository can be created immediately if it starts as one of these:

- a private working repository
- a public repository clearly marked as experimental and unreleased
- a public repository that contains planning docs plus an explicitly experimental implementation

### What should not happen yet

Do **not** do any of the following until the transport spike passes and the extracted code is stable:

- publish a release tag
- publish to Arduino Library Manager
- claim broad FTPS compatibility
- claim that Explicit FTPS is already proven on Opta
- copy application-specific code into the public API surface

### Recommended positioning statement

The repo README should describe the project like this:

> An Arduino Opta FTPS client library, focused on Mbed-based Ethernet/TLS transport, with v1 initially targeting validation against a WD My Cloud PR4100 and intended to support other standards-compliant FTPS servers and additional FTPS modes over time.

That statement is narrow enough to be honest and broad enough to support general Opta use.

---

## v1 Scope

### In scope for v1

- Explicit FTPS only (`AUTH TLS`)
- passive mode only
- `PBSZ 0`
- `PROT P`
- `USER`, `PASS`, `TYPE I`, `PASV`, `STOR`, `RETR`, `QUIT`
- upload and download primitives
- SHA-256 fingerprint pinning
- imported PEM trust certificate support
- error categories suitable for field diagnostics
- Arduino Opta examples using Ethernet
- server compatibility notes based on actual testing

### Out of scope for v1

- Implicit FTPS
- active mode
- full directory browsing API
- recursive sync
- file delete/rename/mkdir helpers
- web UI
- persistent settings storage
- application-specific backup/restore policy
- product-specific cloud integration
- generic support claims for every Arduino board

### Candidate v2 scope

- Implicit FTPS as an optional mode alongside Explicit FTPS
- optional active-mode transfers in addition to passive mode
- capability discovery and directory helpers such as `FEAT`, `PWD`, `MLSD`, and `NLST`
- file-management helpers such as `DELE`, `RNFR`/`RNTO`, `MKD`, and `RMD`
- stream-oriented upload/download APIs in addition to buffer helpers
- broader certificate and trust-management options
- a documented compatibility matrix across more FTPS servers
- reconsidering additional Arduino Mbed targets only after Opta support is stable

### Prioritization note

Implicit FTPS should be the first v2 protocol expansion. Active mode is reasonable as a v2 compatibility feature, but it should remain secondary because it increases firewall and NAT complexity and is less likely to matter for the initial Opta-to-NAS use cases.

### Positioning note

The library is for **general Arduino Opta FTPS use**, but the first shipping scope should still be intentionally small. That is the only realistic way to make the repo maintainable.

---

## Product Boundary

### What belongs in the library

- FTP control-channel handling
- Explicit TLS upgrade logic
- protected passive data-channel handling
- trust configuration
- reply parsing helpers
- upload/download primitives
- transport abstraction
- stable FTPS error categories

### What must stay out of the library

- Application-specific config structs or settings storage
- Application-specific HTML/JS settings UI
- Filesystem persistence rules tied to a particular product
- Application-specific archive or file naming conventions
- Application-specific logging globals
- Application-specific watchdog orchestration
- Product-specific cloud or IoT workflows
- Server-specific application behavior

### Key rule

If a piece of code cannot be explained without mentioning a specific application's business logic, it should stay out of this library.

---

## Recommended Architecture Baseline

### Preferred backend

The preferred first backend remains:

- `MbedSecureSocketFtpsTransport`

Built on:

- `NetworkInterface *net = Ethernet.getNetwork();`
- `TCPSocket` for the underlying control and data TCP sockets
- `TLSSocketWrapper` for the control upgrade and protected data channel

### Why this is still the best first path

- Explicit FTPS needs a STARTTLS-like upgrade after `AUTH TLS`.
- `TLSSocketWrapper` is the strongest currently identified fit for wrapping an already-open socket.
- The Opta core exposes the relevant Mbed socket classes.
- This path avoids taking on a third-party TLS dependency before it is justified.

### Fallback policy

Fallbacks should be described carefully:

1. Lower-level custom Mbed TLS socket wrapping is the most realistic direct fallback for Explicit FTPS if `TLSSocketWrapper` cannot do what is needed.
2. `TLSSocket` is a separate research path, not an automatic protocol-equivalent fallback.
3. `SSLClient` is a separate research path, not an automatic protocol-equivalent fallback.
4. Allowing Implicit FTPS would be a scope change, not a technical fallback for the same protocol path.

---

## Public API Baseline

The public API should be intentionally small.

### Recommended config types

```cpp
enum class FtpsTrustMode : uint8_t {
  Fingerprint = 0,
  ImportedCert = 1,
};

struct FtpsServerConfig {
  const char *host = nullptr;
  uint16_t port = 21;
  const char *user = nullptr;
  const char *password = nullptr;
  const char *tlsServerName = nullptr;
  FtpsTrustMode trustMode = FtpsTrustMode::Fingerprint;
  const char *fingerprint = nullptr;
  const char *rootCaPem = nullptr;
  bool validateServerCert = true;
};
```

### Recommended error surface

```cpp
enum class FtpsError : uint8_t {
  None = 0,
  NoNetwork,
  ControlConnectFailed,
  BannerReadFailed,
  AuthTlsRejected,
  ControlTlsHandshakeFailed,
  CertValidationFailed,
  PbszRejected,
  ProtPRejected,
  LoginRejected,
  PasvParseFailed,
  DataConnectFailed,
  DataTlsHandshakeFailed,
  SessionReuseRequired,
  TransferFailed,
  FinalReplyFailed,
  QuitFailed,
  NotSupported,
};
```

### Recommended client surface

```cpp
class FtpsClient {
public:
  bool connect(const FtpsServerConfig &config, char *error, size_t errorSize);
  bool store(const char *remotePath,
             const uint8_t *data,
             size_t len,
             char *error,
             size_t errorSize);
  bool retrieve(const char *remotePath,
                uint8_t *buffer,
                size_t capacity,
                size_t &bytesRead,
                char *error,
                size_t errorSize);
  void quit();
};
```

### Recommended transport boundary

The transport boundary should include the diagnostics methods that were called out as missing in the latest reviews.

```cpp
class IFtpsTransport {
public:
  virtual ~IFtpsTransport() {}

  virtual bool connectControl(const FtpEndpoint &endpoint,
                              const FtpTlsConfig &tls,
                              char *error,
                              size_t errorSize) = 0;

  virtual bool upgradeControlToTls(const FtpTlsConfig &tls,
                                   char *error,
                                   size_t errorSize) = 0;

  virtual bool openProtectedDataChannel(const FtpEndpoint &endpoint,
                                        const FtpTlsConfig &tls,
                                        char *error,
                                        size_t errorSize) = 0;

  virtual int ctrlWrite(const uint8_t *data, size_t len) = 0;
  virtual int ctrlRead(uint8_t *buf, size_t len) = 0;
  virtual int dataWrite(const uint8_t *data, size_t len) = 0;
  virtual int dataRead(uint8_t *buf, size_t len) = 0;
  virtual bool ctrlConnected() = 0;
  virtual bool dataConnected() = 0;
  virtual void closeData() = 0;
  virtual void closeAll() = 0;

  virtual bool getPeerCertFingerprint(char *out, size_t outLen) {
    return false;
  }

  virtual int getLastTlsError() {
    return 0;
  }
};
```

### API rules that should be written down now

- Fail closed. Never silently fall back to plain FTP.
- `USER` handling must accept the real-world control flow already reflected in the corrected implementation note:
  - `331` means send `PASS`
  - `230` means already logged in
  - `232` is valid if security data exchange authorizes the user
- Timeouts for TLS handshake should be separate from ordinary plain-FTP read timeouts.
- Public structs should describe in-memory config only. They should not expose application-specific file paths or JSON storage concerns.

---

## Implementation Work Required

### Phase A - Prove the transport path

- [ ] Run the Phase 0 `TLSSocketWrapper` spike on real Opta hardware.
- [ ] Confirm `AUTH TLS` control upgrade works after a plain TCP connect.
- [ ] Confirm `PBSZ 0` and `PROT P` work on the target server.
- [ ] Confirm at least one protected `STOR` and one protected `RETR` succeed.
- [ ] Record control-handshake and data-handshake timing.
- [ ] Record whether the server requires data-channel TLS session reuse/resumption.
- [ ] Record whether the server requires SNI or special certificate handling.
- [ ] Decide whether Option C remains viable after real hardware testing.

### Phase B - Build the library source tree

- [ ] Implement FTPS code in internal files:
  - `FtpsTypes.h`
  - `FtpsErrors.h`
  - `FtpsTrust.h`
  - `transport/IFtpsTransport.h`
  - `transport/MbedSecureSocketFtpsTransport.h`
  - `transport/MbedSecureSocketFtpsTransport.cpp`
  - `FtpsClient.h`
  - `FtpsClient.cpp`
- [ ] Keep application integration concerns outside the library.
- [ ] Implement a robust PASV parser that starts parsing after `(` in the `227` response.
- [ ] Implement FTP reply parsing as reusable helper functions.
- [ ] Implement FTPS command orchestration in a reusable client layer.

### Phase C - Transport layer implementation

- [ ] Implement `ftpReadResponse(IFtpsTransport &, ...)` using the transport abstraction.
- [ ] Implement `ftpSendCommand()` so it operates on `transport.ctrlWrite()` and `transport.ctrlRead()`.
- [ ] Manage data socket lifetime inside the transport object rather than in per-function locals.
- [ ] Ensure `transport.closeData()` is the only data-channel cleanup path used by the FTPS layer.
- [ ] Add transport diagnostics methods:
  - `getPeerCertFingerprint()`
  - `getLastTlsError()`
- [ ] Introduce a TLS handshake timeout separate from the plain control timeout.
- [ ] Document the failure-path cleanup contract for `closeAll()`.

### Phase D - Ensure clean library boundaries

- [ ] Library must not depend on any global config structs.
- [ ] Library must not depend on any embedded web UI.
- [ ] Library must not encode application-specific filesystem paths.
- [ ] Library must not encode application-specific file naming conventions.
- [ ] Library must not depend on application-specific logging globals.
- [ ] Library must not depend on application-specific watchdog orchestration.
- [ ] Use reusable error categories plus human-readable details instead of application-specific error text.

---

## New Repository Bootstrap Checklist

This is what should be created in the new repo immediately, even before the backend implementation is declared done.

### Metadata and repo hygiene

- [x] Choose the repo name.
- [ ] Decide private vs public initial visibility.
- [x] Select the license.
- [x] Add `README.md`.
- [x] Add `LICENSE`.
- [x] Add `library.properties`.
- [x] Add `.gitignore` appropriate for Arduino and VS Code.
- [x] Add `CHANGELOG.md`.
- [ ] Add `CONTRIBUTING.md`.
- [ ] Add `.github/ISSUE_TEMPLATE/`.
- [ ] Add `.github/pull_request_template.md` if the repo will take PRs.

### Source tree

- [x] Create `src/FtpsClient.h`.
- [x] Create `src/FtpsClient.cpp`.
- [x] Create `src/FtpsTypes.h`.
- [x] Create `src/FtpsErrors.h`.
- [x] Create `src/FtpsTrust.h`.
- [x] Create `src/transport/IFtpsTransport.h`.
- [x] Create `src/transport/MbedSecureSocketFtpsTransport.h`.
- [x] Create `src/transport/MbedSecureSocketFtpsTransport.cpp`.
- [ ] Decide whether reply-parser helpers live in `src/internal/` or stay in `FtpsClient.cpp` for v1.

### Examples

- [x] Add `examples/BasicUpload/`.
- [x] Add `examples/BasicDownload/`.
- [x] Add `FtpsSpikeTest/FtpsSpikeTest.ino`.
- [ ] Add a dedicated fingerprint-validation example.
- [ ] Add a dedicated imported-PEM-validation example.
- [ ] Add one PR4100-oriented example or walkthrough.
- [ ] Keep example names generic enough that the repo does not look PR4100-only.

### Docs

- [ ] Add `docs/protocol-flow.md`.
- [ ] Add `docs/trust-model.md`.
- [ ] Add `docs/supported-targets.md`.
- [ ] Add `docs/limitations.md`.
- [ ] Add `docs/server-compatibility.md`.
- [ ] Add `docs/debugging.md`.
- [ ] Add `docs/release-checklist.md`.
- [ ] Add `extras/test-notes/` for hardware run logs.

### CI and automation

- [ ] Add a workflow that compiles all examples for the Opta board target.
- [ ] Pin or document the minimum supported `arduino:mbed_opta` version.
- [ ] Fail CI if examples stop compiling.
- [ ] Add a manual checklist for hardware validation runs.

---

## Proposed Repository Layout

```text
ArduinoOptaFTPS/
  README.md
  LICENSE
  library.properties
  CHANGELOG.md
  CONTRIBUTING.md
  src/
    FtpsClient.h
    FtpsClient.cpp
    FtpsTypes.h
    FtpsErrors.h
    FtpsTrust.h
    transport/
      IFtpsTransport.h
      MbedSecureSocketFtpsTransport.h
      MbedSecureSocketFtpsTransport.cpp
  examples/
    BasicUpload/
    BasicDownload/
  FtpsSpikeTest/
    FtpsSpikeTest.ino
  // Planned additions:
  //   examples/OptaFingerprintValidation/
  //   examples/OptaImportedCertValidation/
  //   examples/OptaPr4100Reference/
  docs/
    protocol-flow.md
    trust-model.md
    supported-targets.md
    limitations.md
    server-compatibility.md
    debugging.md
    release-checklist.md
  extras/
    test-notes/
  .github/
    workflows/
    ISSUE_TEMPLATE/
```

---

## Documentation Backlog For The New Repo

These items should exist before a first public release.

### README minimum content

- [ ] What the library does.
- [ ] What the library does **not** do.
- [ ] Supported board statement.
- [ ] Supported FTPS mode statement.
- [ ] Initial test-server statement.
- [ ] Quick-start example.
- [ ] Trust setup summary.
- [ ] Known limitations.
- [ ] Experimental-status note until release gates pass.

### `supported-targets.md`

- [ ] State that Arduino Opta is the only supported v1 board.
- [ ] State the minimum tested board/core version.
- [ ] State that PR4100 is the first validated server.
- [ ] State that other FTPS servers may work if they follow Explicit FTPS passive-mode behavior, but they are not all tested yet.

### `limitations.md`

- [ ] Explicit FTPS only.
- [ ] Passive mode only.
- [ ] Memory and timeout considerations.
- [ ] Possible incompatibility with servers that enforce unsupported session reuse behavior.
- [ ] No directory sync or advanced file management in v1.

### `debugging.md`

- [ ] Common FTP reply failures.
- [ ] Certificate validation failure guidance.
- [ ] TLS handshake timeout guidance.
- [ ] Data-channel TLS failure guidance.
- [ ] Session reuse troubleshooting guidance.
- [ ] Guidance for collecting serial logs from Opta examples.

---

## Test And Validation Backlog

### Compile validation

- [ ] Compile every example with `arduino:mbed_opta`.
- [ ] Confirm include paths for `mbed.h`, `TCPSocket`, and `TLSSocketWrapper` are stable.
- [ ] Confirm no hidden application-specific dependencies remain.

### Hardware validation

- [ ] Validate upload against PR4100.
- [ ] Validate download against PR4100.
- [ ] Validate fingerprint mode.
- [ ] Validate imported PEM mode.
- [ ] Validate reconnect after previous transfer.
- [ ] Validate repeated transfers in a loop.
- [ ] Validate behavior after temporary network interruption.

### Negative tests

- [ ] Wrong fingerprint rejected.
- [ ] Missing PEM trust rejected when imported-cert mode is selected.
- [ ] Wrong password rejected cleanly.
- [ ] `AUTH TLS` rejection surfaced cleanly.
- [ ] `PROT P` rejection surfaced cleanly.
- [ ] PASV parse failure surfaced cleanly.
- [ ] Data-channel TLS handshake failure surfaced cleanly.
- [ ] Final transfer reply timeout surfaced cleanly.

### Stability checks

- [ ] Record handshake timings.
- [ ] Record rough memory behavior.
- [ ] Confirm no silent downgrade to plaintext FTP.
- [ ] Confirm cleanup works after partial failure.
- [ ] Confirm repeated connect/transfer/quit cycles do not leak state.

---

## Release Gates

Require all of the following before the first public release:

- [ ] Real Opta hardware spike passed.
- [ ] Control-channel TLS upgrade proven after `AUTH TLS`.
- [ ] Protected passive data channel proven.
- [ ] `STOR` and `RETR` both validated.
- [ ] Fingerprint trust flow validated.
- [ ] Imported PEM trust flow validated.
- [ ] TLS timeout budget documented.
- [ ] Session reuse/resumption behavior documented for the tested server.
- [ ] No application-specific globals or schema remain in the library code.
- [ ] Examples compile cleanly.
- [ ] README and limitations match the actually tested scope.
- [ ] Error model is stable enough for field debugging.

If any of those are missing, the repo can still exist, but it should remain clearly experimental.

---

## Naming Recommendation

The best current working name is still:

- `ArduinoOptaFTPS`

Why this is still the best fit:

- it is honest about the board target
- it avoids over-claiming support for all Arduino boards
- it is easy to understand in search results

Avoid these until the scope is actually broader:

- `ArduinoFTPS`
- `GenericArduinoFtps`
- any name that implies broad multi-board support that is not yet tested

If the project later broadens beyond Opta to more Mbed-based Arduino boards, the name can be revisited.

---

## Suggested Milestones

### Milestone 0 - Repo bootstrap

- [x] Create the repository.
- [x] Add the docs, layout, metadata, and tracked TODOs.
- [x] Add placeholder source files and examples without over-promising functionality.

### Milestone 1 - Transport proof

- [ ] Run and record the Opta Phase 0 spike.
- [ ] Decide whether `TLSSocketWrapper` remains the shipping backend.
- [ ] Decide whether session reuse is a blocker.

### Milestone 2 - Internal extraction

- [ ] Organize FTPS implementation into library-shaped internal files.
- [ ] Resolve transport diagnostics and helper migration gaps.
- [ ] Keep product integration separate.

### Milestone 3 - MVP library implementation

- [ ] Implement `FtpsClient` and the Mbed transport backend.
- [ ] Implement and validate upload and download examples.
- [ ] Add trust-mode examples.

### Milestone 4 - Validation and docs

- [ ] Run hardware validation.
- [ ] Finalize limitations and debugging docs.
- [ ] Finalize supported-targets and compatibility statements.

### Milestone 5 - First release candidate

- [ ] Confirm release gates are all met.
- [ ] Tag a release candidate only after the tested scope is fully documented.

---

## Main Risks

1. Creating a repo that reflects application-specific design instead of a reusable FTPS client boundary.
2. Treating PR4100 validation as proof of universal FTPS compatibility.
3. Underestimating data-channel TLS session reuse/resumption behavior on real servers.
4. Publishing before the Opta backend is proven on hardware.
5. Locking a public API before transport diagnostics and helper migration gaps are resolved.

---

## Bottom-Line Recommendation

The repository creation step is complete.

Keep it positioned as an **Opta-first Explicit FTPS library project with a documented backlog**, not as a finished library and not as a raw dump of application-specific code.

The practical sequence should be:

1. run the Opta Phase 0 spike and record the result
2. refactor the FTPS logic into clean internal library modules
3. implement the generic FTPS pieces in this repo
4. release only after hardware validation, trust validation, and documentation gates are complete

That gives the project the best chance of ending up with a genuinely reusable Arduino Opta FTPS library instead of a product-specific one-off.