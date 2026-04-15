# ArduinoOPTA-FTPS Naming Conventions

**Date:** April 15, 2026  
**Purpose:** Document naming rules and collision analysis against the Arduino FTP ecosystem.

---

## 1. Competing Library Survey

| Library | Class | Header Guard | Methods Style | Notes |
|---|---|---|---|---|
| ESP32_FTPClient (ldab) | `ESP32_FTPClient` | `ESP32_FTPCLIENT_H` | PascalCase (`OpenConnection`, `DownloadFile`) | Private: `client`, `dclient`, `userName`, `passWord`, `_isConnected` |
| FTPClientServer (dplasa) | `FTPClient`, `FTPCommon`, `FTPServer` | `FTP_CLIENT_H`, `FTP_COMMON_H` | camelCase (`begin`, `transfer`, `check`) | Macros: `FTP_CTRL_PORT`, `FTP_DATA_PORT_PASV`, `FTP_TIME_OUT`, `FTP_CMD_SIZE`. Enum: `FTP_PUT`, `FTP_GET` |
| FTPClient_Generic (khoih-prog) | `FTPClient_Generic` | `FTPCLIENT_GENERIC_H` | PascalCase (`OpenConnection`, `DownloadFile`, `NewFile`) | Constants: `COMMAND_XFER_TYPE_ASCII`, `COMMAND_XFER_TYPE_BINARY`. Fork of ldab's library |
| ESP-FTP-Server-Lib (peterus) | `FTPServer` | — | — | Server-only, less relevant |

---

## 2. Collision Analysis

### No collisions detected.

Our `Ftps` prefix cleanly separates every public symbol:

| Our Symbol | Nearest Match | Safe? |
|---|---|---|
| `FtpsClient` | `FTPClient` (dplasa), `FTPClient_Generic` (khoih) | ✅ "s" differentiates |
| `FTPS_CLIENT_H` | `FTP_CLIENT_H` (dplasa) | ✅ "S_" differentiates |
| `FtpsError` | (none) | ✅ |
| `FtpsServerConfig` | `ServerInfo` (dplasa) | ✅ |
| `FtpsTrustMode` | (none) | ✅ |
| `ftpsTrust*()` functions | (none) | ✅ |
| `IFtpsTransport` | (none) | ✅ |
| `store()` / `retrieve()` | `DownloadFile`/`NewFile`/`Write` (ldab/khoih), `transfer()` (dplasa) | ✅ |

### Macro risk

dplasa defines bare `FTP_*` macros (`FTP_CTRL_PORT 21`, `FTP_DATA_PORT_PASV 0`, etc.). We avoid `#define` for constants entirely, using `enum class` instead. If we ever need macros, always use `FTPS_` prefix.

---

## 3. Naming Rules

### 3.1 Public Classes and Structs

- **PascalCase** with `Ftps` prefix: `FtpsClient`, `FtpsServerConfig`
- Transport-internal types use `Ftp` prefix (no "s"): `FtpEndpoint`, `FtpTlsConfig`
- Interface prefix: `I` + PascalCase: `IFtpsTransport`

### 3.2 Enums

- **`enum class`** (scoped), never plain `enum` or `#define`
- PascalCase name with `Ftps` prefix for public enums: `FtpsError`, `FtpsTrustMode`
- Transport-internal enums may use `Ftp` prefix when they are not part of the public API: `FtpTlsSecurityMode`
- PascalCase values: `ExplicitTls`, `ConnectionFailed`

### 3.3 Methods

- **camelCase**, following Arduino convention: `begin()`, `connect()`, `lastError()`
- Transfer operations use **FTP RFC 959 verb names**: `store()` (STOR), `retrieve()` (RETR)
- Rationale: direct mapping to protocol commands; avoids ambiguity with file-system operations; unique in the Arduino FTP ecosystem

### 3.4 Private Members

- **`_underscorePrefix`**: `_transport`, `_lastError`
- Consistent with ldab, khoih, and general Arduino embedded convention

### 3.5 Free Functions

- **camelCase** with `ftps` prefix: `ftpsTrustNormalizeFingerprint()`, `ftpsTrustFingerprintsMatch()`

### 3.6 Header Guards

- **ALL_CAPS** with `_H` suffix: `FTPS_CLIENT_H`, `FTPS_TYPES_H`, `IFTPS_TRANSPORT_H`

### 3.7 Constants and Macros

- Prefer `enum class` or `static const` over `#define`
- If `#define` is required, use **ALL_CAPS** with `FTPS_` prefix: e.g., `FTPS_VERSION`

### 3.8 File Names

- PascalCase `.h`/`.cpp`: `FtpsClient.h`, `FtpsTypes.h`
- Transport implementations in `transport/` subdirectory
- Interface files start with `I`: `IFtpsTransport.h`

---

## 4. Why These Choices Work

1. **`Ftps` prefix as namespace substitute** — Arduino sketches don't use C++ namespaces well. A consistent prefix avoids collisions without requiring `using namespace`.

2. **RFC verb names (`store`/`retrieve`)** — Every other Arduino FTP library invents its own vocabulary (`DownloadFile`, `DownloadString`, `transfer`). Using the RFC names is unambiguous and guarantees no collision.

3. **`enum class` for type safety** — dplasa's bare `FTP_PUT`/`FTP_GET` macros pollute the global namespace. Our scoped enums (`FtpsError::ConnectionFailed`) cannot collide with anything.

4. **`begin()` follows Arduino convention** — `Serial.begin()`, `Wire.begin()`, `Ethernet.begin()`, dplasa's `FTPClient.begin()`. Users expect it.

---

## 5. Resolved Design Note

The public API no longer exposes `FtpsSecurityMode` while v1 remains fixed to Explicit FTPS. That removes the duplicate-enum problem from the public surface and avoids advertising unsupported modes.

`FtpTlsSecurityMode` remains transport-internal in `IFtpsTransport.h` so the backend can still model future protocol expansion without locking those switches into the user-facing config prematurely.
