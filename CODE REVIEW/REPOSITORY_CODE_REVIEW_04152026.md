# Repository Code Review — April 15, 2026

## Scope
Review focused on current FTPS library code and sketches in:
- `src/`
- `examples/`
- `FtpsSpikeTest/`

Compile-level verification with `arduino-cli` was not run because `arduino-cli` is not installed in this environment.

---

## Findings (Ordered by Severity)

### 1) High — Control-channel reply desynchronization after transfer failures
**Where:**
- `src/FtpsClient.cpp:534`
- `src/FtpsClient.cpp:648`
- `src/FtpsClient.cpp:668`
- Success-path final reply handling at `src/FtpsClient.cpp:538` and `src/FtpsClient.cpp:672`

**Issue:**
`store()` and `retrieve()` return immediately on data-channel failure cases (write failure, read failure, buffer-too-small) after closing the data socket, but without reading the final server reply on the control channel.

**Why it matters:**
FTPS servers commonly emit a final control reply (often error status) even after a failed transfer. If that reply is left unread, the next command can consume stale data and the session can become logically desynchronized.

**Recommendation:**
On all post-`STOR`/`RETR` failure paths, read and discard/classify the final control reply before returning, or hard-reset the control session (`closeAll()` + reconnect policy).

---

### 2) Medium — PASV host is trusted directly from server response
**Where:**
- `src/FtpsClient.cpp:186`
- Data connection usage at `src/FtpsClient.cpp:502` and `src/FtpsClient.cpp:615`

**Issue:**
The PASV parser sets `endpoint.host` directly from the `227` tuple host. The client then connects to that host without additional policy checks.

**Why it matters:**
This can allow server-directed data-channel redirection (FTP bounce/SSRF-style behavior), especially on untrusted networks or misconfigured servers.

**Recommendation:**
Add a policy to either:
- ignore PASV host and reuse control host by default, or
- enforce strict validation (same host/subnet/allowlist) before opening data connections.

---

### 3) Medium — PASV tuple component bounds are not validated
**Where:**
- `src/FtpsClient.cpp:174`

**Issue:**
`parsePasv()` accumulates tuple values but does not verify each part is in valid range (`0..255`).

**Why it matters:**
Malformed replies can produce invalid addresses/ports and lead to undefined or misleading connection behavior.

**Recommendation:**
Validate all six tuple parts are within `0..255` before constructing host/port.

---

### 4) Medium — Remote path command truncation is not detected
**Where:**
- `src/FtpsClient.cpp:515`
- `src/FtpsClient.cpp:628`

**Issue:**
`snprintf(command, sizeof(command), "STOR %s", remotePath)` and `RETR` equivalent do not check truncation.

**Why it matters:**
Long paths can be silently truncated, causing transfers to target unexpected remote files and producing hard-to-debug behavior.

**Recommendation:**
Check `snprintf` return value for truncation (`>= sizeof(command)`) and return a deterministic error.

---

### 5) Medium — `isIpLiteral()` can misclassify hostnames
**Where:**
- `src/FtpsClient.cpp:43`
- `src/FtpsClient.cpp:57`
- `src/FtpsClient.cpp:65`
- Constraint enforcement using this result at `src/FtpsClient.cpp:300`

**Issue:**
`isIpLiteral()` accepts any string containing only `[0-9a-fA-F.:]` with at least one separator. Hostnames such as `face.b00c` would be treated as IP literals.

**Why it matters:**
This can trigger incorrect `tlsServerName` requirements and alter TLS/SNI behavior unexpectedly.

**Recommendation:**
Replace heuristic parsing with proper IPv4/IPv6 literal validation.

---

### 6) Medium — Validation mode semantics are inconsistent for ImportedCert flow
**Where:**
- Trust material requirement logic at `src/FtpsClient.cpp:269` and `src/FtpsClient.cpp:283`
- TLS auth mode override at `src/transport/MbedSecureSocketFtpsTransport.cpp:119`

**Issue:**
`connect()` requires trust material by trust mode, but transport disables certificate verification when `validateServerCert == false`.

**Why it matters:**
In `ImportedCert` mode with `validateServerCert=false`, a PEM is still required by API-level validation but effectively not used for authentication by TLS auth mode.

**Recommendation:**
Define and enforce one consistent contract:
- either fail-closed by rejecting `validateServerCert=false`, or
- allow it explicitly and avoid requiring trust material in that mode, with clear warnings.

---

### 7) Low — PEM validation helper is structural but not semantic
**Where:**
- `src/FtpsTrust.cpp:68`
- `src/FtpsTrust.cpp:81`
- `src/FtpsTrust.cpp:82`
- `src/FtpsTrust.cpp:95`

**Issue:**
`ftpsTrustValidatePem()` checks marker presence/order and duplication, but not certificate parse validity.

**Why it matters:**
Malformed content can pass pre-validation and fail later at handshake time, reducing quality of user-facing diagnostics.

**Recommendation:**
If feasible, parse PEM with Mbed X509 APIs during validation to fail early with a precise error.

---

### 8) Low — Repeated dynamic allocation in transport hot paths
**Where:**
- `src/transport/MbedSecureSocketFtpsTransport.cpp:76`
- `src/transport/MbedSecureSocketFtpsTransport.cpp:198`
- `src/transport/MbedSecureSocketFtpsTransport.cpp:276`

**Issue:**
Sockets/wrappers are allocated and destroyed repeatedly across connect/transfer operations.

**Why it matters:**
On long-running embedded workloads this can increase heap fragmentation risk.

**Recommendation:**
Consider pre-allocated members or pooled objects where practical, especially for repeated transfer loops.

---

## Open Questions / Assumptions
- Assumed `validateServerCert=false` is intended only for controlled debug scenarios, not production.
- Assumed passive-mode server interoperability includes servers that may return non-routable or mismatched PASV host values.

---

## Testing Gaps
- No repository CI workflow currently compiles examples against `arduino:mbed_opta`.
- No automated regression tests currently verify control-channel state after transfer failure paths.

---

## Quick Wins (Suggested First Fixes)
1. Fix reply-drain behavior on all `store()`/`retrieve()` failure paths after transfer commands.
2. Add PASV tuple range validation and data-endpoint policy enforcement.
3. Detect and reject `STOR`/`RETR` command truncation.
4. Tighten IP-literal detection logic and clarify `validateServerCert` contract.
