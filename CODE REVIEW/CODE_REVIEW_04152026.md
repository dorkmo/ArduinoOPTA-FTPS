# Code Review — ArduinoOPTA-FTPS

**Date:** 2026-04-15  
**Scope:** Full repository — all source files, examples, spike test, configuration, and documentation.  
**Focus:** Errors, logical pitfalls, and design concerns.

---

## Summary

The library is a well-structured, functional Explicit FTPS client for Arduino Opta. The protocol flow, error enum design, and transport abstraction are sound. This review identifies **3 bugs**, **5 logical pitfalls**, and **7 design/quality observations** across the codebase.

Severity scale used below:

| Severity | Meaning |
|----------|---------|
| **BUG** | Incorrect behavior at runtime or compile time |
| **PITFALL** | Correct today but fragile or likely to break under realistic conditions |
| **OBSERVATION** | Not wrong, but worth addressing for robustness or maintainability |

---

## Bugs

### BUG-1: `snprintf` with overlapping source and destination in `failWith` pattern

**File:** `src/FtpsClient.cpp` — anonymous namespace `failWith()` and all callers  
**Severity:** BUG — undefined behavior per C/C++ standard

The `failWith` helper writes the error message into the caller's error buffer:

```cpp
bool failWith(FtpsError &lastError, FtpsError code,
              char *error, size_t errorSize,
              const char *message) {
  lastError = code;
  if (error != nullptr && errorSize > 0) {
    snprintf(error, errorSize, "%s", message);
  }
  return false;
}
```

Multiple call sites pass `error` as both the destination buffer and the `message` source when a prior function already wrote into `error`:

```cpp
// Example from connect():
if (!_transport->connectControl(endpoint, tlsCfg, error, errorSize)) {
  return failWith(_lastError, FtpsError::ConnectionFailed,
                  error, errorSize,
                  hasValue(error) ? error : "TCP control connection failed.");
}
```

When `hasValue(error)` is true, this calls `snprintf(error, errorSize, "%s", error)` — an overlapping read/write. The C standard (§7.21.6.5) and C++ standard explicitly state that if copying takes place between overlapping objects, the behavior is undefined. In practice this often "works" on common implementations, but it can silently corrupt the error string depending on buffer size and compiler optimizations.

**Fix:** Either remove the redundant `snprintf` when `message == error`, or copy through a temporary buffer:

```cpp
bool failWith(FtpsError &lastError, FtpsError code,
              char *error, size_t errorSize,
              const char *message) {
  lastError = code;
  if (error != nullptr && errorSize > 0 && message != error) {
    snprintf(error, errorSize, "%s", message);
  }
  return false;
}
```

### BUG-2: `NSAPI_ERROR_WOULD_BLOCK` used in `FtpsClient.cpp` without guaranteed include

**File:** `src/FtpsClient.cpp` — `ftpReadResponse()`  
**Severity:** BUG — potential compilation failure on some toolchain configurations

`ftpReadResponse()` references `NSAPI_ERROR_WOULD_BLOCK`, which is defined in Mbed's `netsocket/nsapi_types.h`. `FtpsClient.cpp` does not directly include any Mbed headers. It currently compiles only because the Opta board's `Arduino.h` transitively pulls in `mbed.h`, but this is an implementation detail of the board support package, not a guaranteed include path.

If the Arduino Mbed core ever refactors its includes (or if someone ports this to a different Mbed-based board), `FtpsClient.cpp` will fail to compile.

**Fix:** Add an explicit include or define the constant locally:

```cpp
// Option A: Include the Mbed header directly
#include "netsocket/nsapi_types.h"

// Option B: Define the constant locally if isolating from Mbed is preferred
#ifndef NSAPI_ERROR_WOULD_BLOCK
#define NSAPI_ERROR_WOULD_BLOCK -3001
#endif
```

### BUG-3: `parsePasv` does not validate IP octet or port-byte ranges

**File:** `src/FtpsClient.cpp` — `parsePasv()`  
**Severity:** BUG — integer overflow / malformed address with non-compliant server responses

The PASV response parser accumulates digits into `int parts[6]` without bounds checking:

```cpp
parts[index] = (parts[index] * 10) + (*cursor - '0');
```

A non-RFC-compliant or malicious server could send `227 Entering Passive Mode (999,999,999,999,999,999)`, which would:
- Overflow `int` for the IP octets (undefined behavior for signed overflow)
- Produce an invalid IP address string via `snprintf(hostBuf, ..., "%d.%d.%d.%d", ...)`
- Produce an incorrect data port via `(parts[4] << 8) | parts[5]` if either exceeds 255

**Fix:** Clamp or validate each part to the 0–255 range after parsing:

```cpp
for (int i = 0; i < 6; i++) {
  if (parts[i] < 0 || parts[i] > 255) return false;
}
```

---

## Logical Pitfalls

### PITFALL-1: `retrieve()` read loop does not handle `NSAPI_ERROR_WOULD_BLOCK`

**File:** `src/FtpsClient.cpp` — `retrieve()`

The retrieve read loop treats any non-positive return value from `dataRead()` as end-of-stream:

```cpp
while (filled < bufferSize) {
  int n = _transport->dataRead(buffer + filled, bufferSize - filled);
  if (n <= 0) break;
  filled += (size_t)n;
}
```

In contrast, `ftpReadResponse()` explicitly checks for and retries on `NSAPI_ERROR_WOULD_BLOCK`. On the Mbed networking stack, a socket with a positive timeout set via `set_timeout()` will return `NSAPI_ERROR_WOULD_BLOCK` (-3001) when the timeout elapses without data arriving. For large file transfers on slow or congested links, a transient timeout on one `recv()` call would cause `retrieve()` to return a partial file without error.

**Impact:** Silent data truncation — `retrieve()` returns `true` with only a partial download.

**Fix:** Add a would-block retry loop mirroring `ftpReadResponse`:

```cpp
while (filled < bufferSize) {
  int n = _transport->dataRead(buffer + filled, bufferSize - filled);
  if (n == NSAPI_ERROR_WOULD_BLOCK) continue;
  if (n <= 0) break;
  filled += (size_t)n;
}
```

Optionally add an outer timeout to prevent infinite retries.

### PITFALL-2: `writeAll` does not handle `NSAPI_ERROR_WOULD_BLOCK`

**File:** `src/FtpsClient.cpp` — anonymous namespace `writeAll()`

Same issue as PITFALL-1 but for the write path. If `ctrlWrite()` or `dataWrite()` returns `NSAPI_ERROR_WOULD_BLOCK`, `writeAll` treats it as a fatal failure (`written <= 0`). On a congested link, `store()` could fail with `TransferFailed` even though the connection is healthy.

**Fix:** Add a would-block retry:

```cpp
while (offset < size) {
  int written = writeFn(data + offset, size - offset);
  if (written == NSAPI_ERROR_WOULD_BLOCK) continue;
  if (written <= 0) return false;
  offset += (size_t)written;
}
```

### PITFALL-3: `_activeConfig` stores shallow copies of caller-owned string pointers

**File:** `src/FtpsClient.cpp` — `connect()`

```cpp
_activeConfig = config;
```

`FtpsServerConfig` contains `const char*` members (`host`, `user`, `password`, `tlsServerName`, `fingerprint`, `rootCaPem`). This copy duplicates the pointer values, not the strings. If the caller's strings go out of scope after `connect()` returns, subsequent `store()` or `retrieve()` calls that reference `_activeConfig` will dereference dangling pointers.

In the provided examples, all strings are `static const char*` literals, so this is safe. But a user constructing config from dynamic strings (e.g., read from EEPROM or Serial) would hit undefined behavior.

**Impact:** Dangling pointer dereference in non-trivial usage patterns.

**Options:**
1. Document that config string pointers must outlive the `FtpsClient` session (minimum fix)
2. Copy strings into internal buffers in `connect()` (defensive fix, costs ~256 bytes RAM)

### PITFALL-4: No TLS session reuse between control and data channels

**File:** `src/transport/MbedSecureSocketFtpsTransport.cpp` — `openProtectedDataChannel()`

The data channel creates an independent `TLSSocketWrapper` with no session ticket or ID propagated from the control channel's TLS session. Servers configured with `require_ssl_reuse=YES` (vsftpd default) or equivalent will reject the data channel handshake.

Since vsftpd is listed as a v1 reference server in the README and implementation documents, this is a concrete interoperability gap — not a theoretical concern.

**Impact:** `store()` and `retrieve()` will fail against default vsftpd configurations with `DataTlsHandshakeFailed` or `SessionReuseRequired`.

**Note:** Mbed TLS does not expose a trivial API for session ticket export/import across `TLSSocketWrapper` instances. This is a known hard problem. The implementation docs already acknowledge it, but it is worth tracking as a blocking issue for the vsftpd reference server target.

### PITFALL-5: `quit()` does not propagate errors to the caller

**File:** `src/FtpsClient.cpp` — `quit()`

`quit()` returns `void` and communicates failure only via `_lastError`. If a caller doesn't check `lastError()` after `quit()`, a failed session teardown goes entirely unnoticed. The FileZilla live test example does check `lastError()` after `quit()`, but the BasicUpload and BasicDownload examples do not.

**Impact:** Low — `quit()` failure is rarely actionable (the connection is being torn down anyway). But for logging/diagnostic purposes, the inconsistency between the examples could mislead users.

---

## Design and Quality Observations

### OBS-1: Dead variable `dataPort` in `store()` and `retrieve()`

**File:** `src/FtpsClient.cpp`

Both `store()` and `retrieve()` declare `uint16_t dataPort = 0;` and pass it to `parsePasv()`, but never use `dataPort` after the call. The port is already stored in `dataEndpoint.port` by `parsePasv()`, making the separate `dataPort` output parameter redundant.

**Fix:** Remove `dataPort` from `parsePasv`'s signature and the caller's local variable.

### OBS-2: Significant code duplication between `store()` and `retrieve()`

**File:** `src/FtpsClient.cpp`

Both methods repeat the same ~30-line PASV → parse → open-data-channel → close-data-channel → read-final-reply sequence. Extracting a private helper (e.g., `openPassiveDataChannel()` and `closeDataChannelAndReadReply()`) would reduce duplication and make the transfer commands shorter and easier to audit.

### OBS-3: Inconsistent indentation — tabs vs. spaces

**Files:** `src/transport/MbedSecureSocketFtpsTransport.cpp` uses tabs; all other `.cpp` and `.h` files use spaces.

This is a formatting-only issue, but it makes diffs noisy and can cause merge conflicts. An `.editorconfig` or a note in the repository would prevent future drift.

### OBS-4: Hostname set twice during TLS upgrade

**File:** `src/transport/MbedSecureSocketFtpsTransport.cpp` — `upgradeControlToTls()` and `openProtectedDataChannel()`

The `TLSSocketWrapper` constructor receives the hostname for SNI, and then `configureTlsSocket()` calls `set_hostname()` again with the same value. The second call is redundant.

### OBS-5: Spike test `VALIDATE_CERT` flag is misleading

**File:** `FtpsSpikeTest/FtpsSpikeTest.ino`

The `VALIDATE_CERT = false` configuration variable prints a warning message but does **not** actually call `mbedtls_ssl_conf_authmode(MBEDTLS_SSL_VERIFY_NONE)`. The inline comments acknowledge this, but the variable name creates a false expectation for users who skim the configuration block without reading the comments. A rename to something like `CERT_VALIDATION_NOTE` or removing the variable entirely in favor of a comment would reduce confusion.

### OBS-6: `fingerprintFromSocket` uses `mbedtls_ssl_get_peer_cert` which was removed in Mbed TLS 3.x

**File:** `src/transport/MbedSecureSocketFtpsTransport.cpp`

The function already has a version check for the `mbedtls_sha256` API change between Mbed TLS 2.x and 3.x. However, `mbedtls_ssl_get_peer_cert()` was deprecated in 2.x and **removed** in 3.x (replaced by `mbedtls_ssl_get_peer_cert_info()` or `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` config). If the Arduino Opta board support package ever upgrades to Mbed TLS 3.x, this function will fail to compile.

**Note:** The current Opta core ships Mbed TLS 2.x, so this is not an immediate issue.

### OBS-7: README lists a Serial Monitor Output Guide that is not linked in the file listing

**File:** `README.md` — Project Documentation section

The README links to `CODE REVIEW/SERIAL_MONITOR_OUTPUT_04152026.md` and `CODE REVIEW/NAMING_CONVENTIONS_04152026.md` is referenced in other docs, but neither appears in the current repository file listing. Either the files are missing or the links are stale.

---

## File-by-File Summary

| File | Issues |
|------|--------|
| `src/FtpsClient.cpp` | BUG-1, BUG-2, BUG-3, PITFALL-1, PITFALL-2, PITFALL-3, OBS-1, OBS-2 |
| `src/FtpsClient.h` | Clean |
| `src/FtpsTypes.h` | PITFALL-3 (struct design) |
| `src/FtpsErrors.h` | Clean |
| `src/FtpsTrust.h` | Clean |
| `src/FtpsTrust.cpp` | Clean |
| `src/transport/IFtpsTransport.h` | Clean |
| `src/transport/MbedSecureSocketFtpsTransport.h` | Clean |
| `src/transport/MbedSecureSocketFtpsTransport.cpp` | PITFALL-4, OBS-3, OBS-4, OBS-6 |
| `examples/BasicUpload/BasicUpload.ino` | PITFALL-5 (no post-quit error check) |
| `examples/BasicDownload/BasicDownload.ino` | PITFALL-5 (no post-quit error check) |
| `examples/FileZillaLiveTest/FileZillaLiveTest.ino` | Clean |
| `FtpsSpikeTest/FtpsSpikeTest.ino` | OBS-5 |
| `library.properties` | Clean |
| `README.md` | OBS-7 |
| `CHANGELOG.md` | Clean |

---

## Recommended Fix Priority

1. **BUG-1** — `failWith` overlapping `snprintf` — trivial one-line fix, eliminates undefined behavior
2. **PITFALL-1 + PITFALL-2** — Would-block handling in `retrieve()` and `writeAll()` — prevents silent data corruption on slow links
3. **BUG-3** — PASV range validation — one-line bounds check, prevents malformed addresses
4. **PITFALL-3** — Document or defend against dangling config pointers
5. **BUG-2** — Explicit Mbed include — one-line fix, prevents future compilation breakage
6. **PITFALL-4** — TLS session reuse — hard problem, blocks vsftpd compatibility, may need upstream Mbed investigation
7. **OBS-1 through OBS-7** — Quality improvements, address at convenience
