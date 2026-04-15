# Final Pass — ArduinoOPTA-FTPS

**Date:** 2026-04-15  
**Scope:** Complete repository — all source, examples, spike test, web harness, configuration, and documentation.  
**Prior reviews:** CODE_REVIEW_04152026.md, REPOSITORY_CODE_REVIEW_04152026.md, FIX_TRACKER_04152026.md

---

## Prior-Review Fix Verification

All bugs and pitfalls from the earlier reviews were cross-checked against the current source. The FIX_TRACKER is accurate. Summary:

| Finding | Verdict |
|---|---|
| BUG-1 overlapping snprintf | **Fixed** — `failWith` now has `message != error` guard |
| BUG-2 missing Mbed include | **Fixed** — `netsocket/nsapi_types.h` explicitly included |
| BUG-3 PASV range validation | **Fixed** — overflow-safe 0–255 check during parsing |
| PITFALL-1 retrieve WOULD_BLOCK | **Fixed** — retry loop with `FTPS_DATA_IO_TIMEOUT_MS` and explicit negative-result error path |
| PITFALL-2 writeAll WOULD_BLOCK | **Fixed** — retry loop with timeout |
| PITFALL-3 shallow config pointers | **Fixed** — deep copy to owned `_activeHost[]`, `_activeUser[]`, etc. |
| PITFALL-4 TLS session reuse | **Partial** — best-effort session caching added; still may fail with `require_ssl_reuse=YES` servers |
| PITFALL-5 quit() result ignored | **Fixed** — all examples now check `lastError()` after `quit()` |
| REPO-1 control-channel desync | **Fixed** — `drainFinalTransferReply()` added on all post-transfer failure paths |
| REPO-2 PASV host redirect | **Fixed** — `dataEndpoint.host = _activeConfig.host` overrides server-supplied address |
| REPO-4 command truncation | **Fixed** — `formatCommandWithArg()` checks `snprintf` return |
| REPO-5 isIpLiteral misclass | **Fixed** — now delegates to `SocketAddress::set_ip_address()` |
| REPO-6 validateServerCert contract | **Fixed** — fail-closed: rejects `validateServerCert=false` |
| REPO-7 PEM validation structural only | **Fixed** — `mbedtls_x509_crt_parse()` added for semantic validation |
| OBS-1 dead dataPort | **Fixed** — removed from `parsePasv` signature |
| OBS-4 redundant hostname | **Fixed** — `configureTlsSocket` no longer calls `set_hostname()` |
| OBS-5 spike cert flag naming | **Fixed** — renamed to `SPIKE_CERT_VALIDATION_DISABLED` |
| OBS-6 Mbed TLS 3.x compat | **Fixed** — compile-time guard returns false on 3.x |
| OBS-7 README doc links | **Fixed** — all linked files exist in CODE REVIEW/ |

No fixed findings have regressed.

---

## New Findings

### NEW-1: Web harness auth token uses a predictable PRNG

**File:** `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino` — `generateAuthToken()`  
**Severity:** OBSERVATION (LAN lab tool scope)

The token generator seeds from `millis() ^ micros()` and applies an xorshift. An observer on the same LAN who knows the approximate uptime of the device can predict the token.

The sketch's inline comments and documentation already describe this as a lightweight LAN gate, so this is consistent with the stated threat model. If the harness is ever exposed beyond an isolated lab segment, this should be replaced with hardware RNG (Opta's STM32H7 has an RNG peripheral).

### NEW-2: Auth token comparison is timing-vulnerable

**File:** `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino` — `authTokenMatches()`  
**Severity:** OBSERVATION

`strcmp(gAuthToken, incomingToken)` leaks token length/content via timing side channel. For a LAN lab harness this is low risk; a constant-time compare would be the defensive fix:

```cpp
bool authTokenMatches(const char *incomingToken) {
  if (!kEnableAuthGate) return true;
  if (!hasValue(gAuthToken) || !hasValue(incomingToken)) return false;

  size_t len = strlen(gAuthToken);
  if (strlen(incomingToken) != len) return false;

  unsigned char diff = 0;
  for (size_t i = 0; i < len; ++i) {
    diff |= static_cast<unsigned char>(gAuthToken[i] ^ incomingToken[i]);
  }
  return diff == 0;
}
```

### NEW-3: `urlDecode` does not reject embedded null bytes

**File:** `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino` — `urlDecode()`  
**Severity:** OBSERVATION

`%00` in a URL-encoded parameter produces a null byte in the decoded output. All downstream consumers use C string functions that stop at the first null, so a parameter like `host=%00attacker.com` would decode to an empty host string — which is rejected by `hasValue()`. No exploit path is apparent, but defence in depth says to reject `%00`:

```cpp
if (ch == '\0') return false;  // reject embedded nulls
```

### NEW-4: Web harness `gStatus.connected` can become stale

**File:** `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino`  
**Severity:** OBSERVATION

If the FTPS server drops the connection between harness operations, `gStatus.connected` remains `true` until the next `store()`/`retrieve()` fails. The status panel can display "connected" when the session is actually dead.

This is a display accuracy issue, not a correctness bug. A possible improvement would be to probe `ctrlConnected()` (if exposed) during `/status` polling, but this may not be worth the complexity for a lab tool.

### NEW-5: `FtpsClient` object size is ~4.6 KB due to owned string buffers

**File:** `src/FtpsClient.h`  
**Severity:** OBSERVATION

The deep-copy fix for PITFALL-3 added internal buffers:

| Buffer | Size |
|---|---|
| `_activeHost` | 128 |
| `_activeUser` | 96 |
| `_activePassword` | 128 |
| `_activeTlsServerName` | 128 |
| `_activeRootCaPem` | 4097 |
| `_normalizedFingerprint` | 65 |

Total: **~4,642 bytes**. On the Opta's 1 MB SRAM this is acceptable, but sketches that allocate `FtpsClient` on the stack should be aware of the size. All current examples allocate it in `setup()` which puts it on the main stack — fine for Opta's 8 KB default stack. If a user creates one inside a deeply nested function or inside `loop()`, this could overflow the stack.

A future option is to move `_activeRootCaPem` to a heap allocation only when `ImportedCert` mode is used.

---

## Remaining Open Items From Prior Reviews

| Item | Status | Notes |
|---|---|---|
| OBS-2: store/retrieve duplication | Open | ~60 lines of shared PASV+TLS+data setup/teardown are duplicated. Acceptable at current code size but worth extracting before adding more transfer commands. |
| OBS-3: tabs vs. spaces | Open | `MbedSecureSocketFtpsTransport.cpp` uses tabs; all other files use spaces. An `.editorconfig` would prevent further drift. |
| REPO-8: TLSSocketWrapper allocation per transfer | Partial | `TCPSocket` reuse is implemented. `TLSSocketWrapper` still requires per-transfer allocation due to API constraints. Acceptable for v1. |

---

## Cross-File Consistency Check

| Check | Result |
|---|---|
| `FtpsError` enum values match across header, client, transport, and examples | ✅ All consistent |
| `FtpsTrustMode` values match between `FtpsTypes.h` and all examples | ✅ |
| `FtpsServerConfig` fields match between header, client `connect()`, and examples | ✅ |
| `IFtpsTransport` virtual interface matches `MbedSecureSocketFtpsTransport` overrides | ✅ All 13 methods accounted for |
| README API usage example matches actual method signatures | ✅ |
| README doc links match `CODE REVIEW/` directory contents | ✅ All 6 linked docs exist |
| CHANGELOG entries match observed code changes | ✅ |
| `library.properties` `architectures` matches target board | ✅ `mbed_opta` |
| `library.properties` `depends` matches actual includes | ✅ `PortentaEthernet,Ethernet` |
| Web harness API doc matches actual sketch endpoints | ✅ 6 endpoints match |
| Naming conventions doc matches actual symbol names in code | ✅ |

---

## Documentation Gaps

1. **CODE REVIEW/ docs not linked from README:** `CODE_REVIEW_04152026.md`, `REPOSITORY_CODE_REVIEW_04152026.md`, `FIX_TRACKER_04152026.md`, and `NAMING_CONVENTIONS_04152026.md` exist in CODE REVIEW/ but are not in the README's Project Documentation list. These are internal review artifacts, so omitting them from the README is arguably correct. If they are intended to be discoverable, add links.

2. **No `CONTRIBUTING.md` or style guide:** The coding conventions are documented in the naming conventions doc but there is no contribution guide. Not critical for a pre-release repository.

---

## Overall Assessment

The codebase is in good shape for its stated maturity level (experimental, pre-hardware-validation). Every bug and high-severity pitfall from the prior reviews has been addressed. The remaining open items are quality improvements (OBS-2, OBS-3) and known partial fixes (TLS session reuse, TLSSocketWrapper allocation).

The new `WebHarnessLiveTest` is well-structured for a lab tool. Its security posture is appropriate for the stated LAN-only scope, with clearly documented limitations.

**Blockers before first hardware validation:** None. The code is ready for on-device testing.

**Blockers before experimental release:** TLS session reuse behavior needs to be characterized against all three reference servers. If vsftpd's default `require_ssl_reuse=YES` blocks data channels, that needs to be documented as a known limitation or resolved at the transport layer.
