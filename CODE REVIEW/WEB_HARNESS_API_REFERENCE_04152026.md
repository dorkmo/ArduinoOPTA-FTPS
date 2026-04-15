# Web Harness API Reference — April 15, 2026

This document describes the lightweight HTTP API exposed by:
- examples/WebHarnessLiveTest/WebHarnessLiveTest.ino

Use this API to drive live FTPS tests from scripts, terminals, and VS Code workflows.

---

## Scope and Safety

- Intended for LAN test environments.
- Not intended as a production internet-facing service.
- Auth gate is lightweight and passcode/token based.

Before broader network use:
1. Change the sketch passcode constant.
2. Keep the device on an isolated test VLAN or lab LAN.

---

## Base URL

- http://<opta-ip>/

Example:
- http://192.168.1.77/

---

## Auth Model

When auth gate is enabled in the sketch:
1. Call `POST /auth` with form field `passcode`.
2. Receive JSON with a token.
3. Send token in `X-Harness-Token` header on protected endpoints.

Protected endpoints:
- `GET /status`
- `POST /set` (and legacy `GET /set` fallback)
- `GET /action?op=...`
- `GET /report`

Unprotected endpoints:
- `GET /` (UI page)
- `POST /auth`

---

## Endpoint Summary

| Method | Path | Purpose | Auth Required |
|---|---|---|---|
| GET | / | Web UI | No |
| POST | /auth | Exchange passcode for token | No |
| GET | /status | Current state, config snapshot, logs | Yes |
| POST | /set | Update harness config | Yes |
| GET | /set? ... | Legacy fallback config update | Yes |
| GET | /action?op=connect\|upload\|download\|quit | Run FTPS operation | Yes |
| GET | /report | Download text report snapshot | Yes |

---

## Request and Response Details

### 1) POST /auth

Request content type:
- application/x-www-form-urlencoded

Form fields:
- `passcode` (required)

Successful response JSON example:

```json
{
  "ok": true,
  "message": "Harness unlocked.",
  "token": "AB12..."
}
```

Failure response JSON example:

```json
{
  "ok": false,
  "message": "Invalid harness passcode."
}
```

### 2) POST /set

Request content type:
- application/x-www-form-urlencoded

Fields:
- `host`
- `port`
- `user`
- `pass`
- `sni`
- `trust` (`fingerprint` or `imported`)
- `fp` (fingerprint text)
- `path` (remote file path)
- `payload` (small upload text)

Response:
- Same status JSON shape as `/status`.

### 3) GET /action?op=...

Accepted op values:
- `connect`
- `upload`
- `download`
- `quit`

Response:
- Same status JSON shape as `/status`.

### 4) GET /status

Response fields include:
- `busy`, `connected`, `lastSuccess`
- `lastError`, `lastBytes`, `downloadBytes`, `lastDurationMs`
- `lastAction`, `lastMessage`, `downloadPreview`
- `config` object snapshot
- `logs` array

### 5) GET /report

Response:
- text/plain attachment
- Includes a compact snapshot of status, config, preview, and logs.

---

## PowerShell Quickstart

```powershell
$OptaIp = "192.168.1.77"
$Base = "http://$OptaIp"

# 1) Unlock
$auth = Invoke-RestMethod -Method Post `
  -Uri "$Base/auth" `
  -ContentType "application/x-www-form-urlencoded" `
  -Body "passcode=change-me-opta"

$token = $auth.token
$hdr = @{ "X-Harness-Token" = $token }

# 2) Set config
$body = @(
  "host=192.168.1.100",
  "port=21",
  "user=testuser",
  "pass=testpass",
  "sni=",
  "trust=fingerprint",
  "fp=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "path=/ftps_test/opta_web_harness.txt",
  "payload=hello%20from%20powershell"
) -join "&"

Invoke-RestMethod -Method Post `
  -Uri "$Base/set" `
  -Headers $hdr `
  -ContentType "application/x-www-form-urlencoded" `
  -Body $body | Out-Null

# 3) Connect
Invoke-RestMethod -Method Get -Uri "$Base/action?op=connect" -Headers $hdr

# 4) Upload and download
Invoke-RestMethod -Method Get -Uri "$Base/action?op=upload" -Headers $hdr
Invoke-RestMethod -Method Get -Uri "$Base/action?op=download" -Headers $hdr

# 5) Export report
Invoke-WebRequest -Method Get -Uri "$Base/report" -Headers $hdr -OutFile ".\opta-ftps-report.txt"

# 6) Quit
Invoke-RestMethod -Method Get -Uri "$Base/action?op=quit" -Headers $hdr
```

---

## cURL Quickstart

```bash
BASE="http://192.168.1.77"

TOKEN=$(curl -s -X POST "$BASE/auth" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "passcode=change-me-opta" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')

curl -s -X POST "$BASE/set" \
  -H "X-Harness-Token: $TOKEN" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "host=192.168.1.100&port=21&user=testuser&pass=testpass&trust=fingerprint&fp=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&path=/ftps_test/opta_web_harness.txt&payload=hello"

curl -s "$BASE/action?op=connect" -H "X-Harness-Token: $TOKEN"
curl -s "$BASE/action?op=upload" -H "X-Harness-Token: $TOKEN"
curl -s "$BASE/action?op=download" -H "X-Harness-Token: $TOKEN"
curl -s "$BASE/status" -H "X-Harness-Token: $TOKEN"
curl -s "$BASE/report" -H "X-Harness-Token: $TOKEN" -o opta-ftps-report.txt
curl -s "$BASE/action?op=quit" -H "X-Harness-Token: $TOKEN"
```

---

## Notes for VS Code Copilot Workflows

- Use `POST /auth` once per session and cache the returned token.
- Use `POST /set` before each action batch if you modify config values.
- Use `/status` between actions to gate decisions in automation.
- Save `/report` artifacts to compare runs across server settings.

---

## Common Failures

- `401 Unauthorized`: token missing, wrong, or not unlocked.
- `400 Bad Request`: malformed form body or missing required parameter.
- FTPS action failure with `lastError != 0`: check `lastMessage`, then export `/report` for full context.
