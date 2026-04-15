// ArduinoOPTA-FTPS - Opta Web Harness Live Test
// SPDX-License-Identifier: CC0-1.0
//
// Hosts a small web UI on Arduino Opta for iterative FTPS testing without
// reflashing after each server setting change.

#include <Arduino.h>

#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #error "This example is designed for Arduino Opta only"
#endif

#include <FtpsClient.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

static const uint16_t kHttpPort = 80;

static const size_t kRequestLineLen = 768;
static const size_t kHeaderLineLen = 320;
static const size_t kQueryLen = 1024;
static const size_t kBodyLen = 1024;
static const size_t kParamKeyLen = 32;
static const size_t kParamValueLen = 512;

static const size_t kHostLen = 128;
static const size_t kUserLen = 96;
static const size_t kPasswordLen = 128;
static const size_t kTlsNameLen = 128;
static const size_t kFingerprintLen = 128;
static const size_t kPathLen = 192;
static const size_t kPayloadLen = 256;

static const size_t kStatusMessageLen = 192;
static const size_t kActionNameLen = 24;

static const size_t kLogLineCount = 24;
static const size_t kLogLineLen = 120;

static const size_t kDownloadBufferSize = 512;
static const size_t kDownloadPreviewLen = 384;

static const uint32_t kHttpReadTimeoutMs = 1500;

static const bool kEnableAuthGate = true;
static const char *kHarnessPasscode = "change-me-opta";
static const size_t kAuthPasscodeLen = 64;
static const size_t kAuthTokenLen = 33;

static const char *kDefaultHost = "192.168.1.100";
static const uint16_t kDefaultPort = 21;
static const char *kDefaultUser = "testuser";
static const char *kDefaultPassword = "testpass";
static const char *kDefaultTlsServerName = "";
static const char *kDefaultFingerprint =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
static const char *kDefaultRemotePath = "/ftps_test/opta_web_harness.txt";
static const char *kDefaultUploadPayload = "Hello from Opta FTPS web harness.\r\n";

// Optional imported-cert trust root for web harness runs.
// Keep this nullptr unless you intentionally test ImportedCert mode.
static const char *kImportedRootCaPem = nullptr;

struct HarnessConfig {
  char host[kHostLen] = {};
  uint16_t port = kDefaultPort;
  char user[kUserLen] = {};
  char password[kPasswordLen] = {};
  char tlsServerName[kTlsNameLen] = {};
  FtpsTrustMode trustMode = FtpsTrustMode::Fingerprint;
  char fingerprint[kFingerprintLen] = {};
  char remotePath[kPathLen] = {};
  char uploadPayload[kPayloadLen] = {};
};

struct HarnessStatus {
  bool busy = false;
  bool connected = false;
  bool lastSuccess = false;
  FtpsError lastError = FtpsError::None;
  size_t lastBytes = 0;
  size_t downloadBytes = 0;
  unsigned long lastDurationMs = 0;
  char lastAction[kActionNameLen] = "idle";
  char lastMessage[kStatusMessageLen] = "Ready.";
  char downloadPreview[kDownloadPreviewLen] = {};
};

static EthernetServer gWebServer(kHttpPort);
static FtpsClient gFtps;
static bool gFtpsBegun = false;
static HarnessConfig gConfig;
static HarnessStatus gStatus;

static char gLogLines[kLogLineCount][kLogLineLen] = {};
static size_t gLogHead = 0;
static size_t gLogCount = 0;
static char gAuthToken[kAuthTokenLen] = {};

static const char kWebPage[] = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Opta FTPS Live Harness</title>
  <style>
    :root {
      --ink: #101820;
      --paper: #f6f2e8;
      --accent: #006d77;
      --accent-soft: #83c5be;
      --hot: #ee6c4d;
      --ok: #2a9d8f;
      --warn: #e9c46a;
      --err: #b00020;
      --panel: rgba(255, 255, 255, 0.88);
      --line: rgba(16, 24, 32, 0.16);
      --mono: "IBM Plex Mono", "Consolas", monospace;
      --display: "Space Grotesk", "Trebuchet MS", sans-serif;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      font-family: var(--display);
      background:
        radial-gradient(circle at 14% 16%, #ffe7b3 0, #ffe7b300 28%),
        radial-gradient(circle at 85% 6%, #b8f2e6 0, #b8f2e600 23%),
        linear-gradient(135deg, #f5f3ef 0%, #f9f5ec 48%, #efe8d8 100%);
      min-height: 100vh;
      padding: 22px;
    }
    .shell {
      max-width: 1100px;
      margin: 0 auto;
      border: 1px solid var(--line);
      border-radius: 20px;
      background: var(--panel);
      box-shadow: 0 22px 42px rgba(16, 24, 32, 0.14);
      overflow: hidden;
      animation: rise 380ms ease-out;
    }
    @keyframes rise {
      from { transform: translateY(10px); opacity: 0; }
      to { transform: translateY(0); opacity: 1; }
    }
    .head {
      padding: 18px 20px;
      border-bottom: 1px solid var(--line);
      background: linear-gradient(90deg, #f9efe0, #f7f6f0 45%, #edf9f6);
    }
    .head h1 {
      margin: 0 0 6px;
      font-size: 1.35rem;
      letter-spacing: 0.01em;
    }
    .head p {
      margin: 0;
      font-size: 0.95rem;
      opacity: 0.84;
    }
    .grid {
      display: grid;
      grid-template-columns: 1.3fr 1fr;
      gap: 14px;
      padding: 16px;
    }
    @media (max-width: 960px) {
      .grid { grid-template-columns: 1fr; }
    }
    .card {
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      background: #fffdf9;
    }
    .card h2 {
      margin: 0 0 10px;
      font-size: 1rem;
      letter-spacing: 0.01em;
    }
    .fields {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    @media (max-width: 640px) {
      .fields { grid-template-columns: 1fr; }
    }
    label {
      display: block;
      font-size: 0.8rem;
      margin-bottom: 4px;
      opacity: 0.86;
    }
    input, select, textarea {
      width: 100%;
      border: 1px solid #c8c4b9;
      border-radius: 10px;
      padding: 9px 10px;
      font-family: var(--mono);
      font-size: 0.9rem;
      background: #fffcf4;
      color: var(--ink);
    }
    textarea {
      min-height: 84px;
      resize: vertical;
    }
    .row-span {
      grid-column: 1 / -1;
    }
    .btns {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 10px;
      margin-top: 12px;
    }
    @media (max-width: 640px) {
      .btns { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    }
    button {
      border: 1px solid rgba(16, 24, 32, 0.24);
      border-radius: 11px;
      background: #f6f3ea;
      color: var(--ink);
      padding: 11px 10px;
      font-family: var(--display);
      font-size: 0.93rem;
      cursor: pointer;
      transition: transform 120ms ease, box-shadow 140ms ease;
    }
    button:hover {
      transform: translateY(-1px);
      box-shadow: 0 6px 14px rgba(16, 24, 32, 0.12);
    }
    button:disabled {
      cursor: not-allowed;
      opacity: 0.56;
      transform: none;
      box-shadow: none;
    }
    .accent {
      border-color: #005f68;
      background: linear-gradient(180deg, #0a7680, #00656f);
      color: #e9faf8;
    }
    .status-top {
      display: grid;
      gap: 8px;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      margin-bottom: 10px;
    }
    .pill {
      border-radius: 999px;
      padding: 5px 10px;
      font-size: 0.8rem;
      display: inline-block;
      border: 1px solid transparent;
    }
    .p-ok { background: #e2f7f0; border-color: #85cab9; }
    .p-warn { background: #fff3d5; border-color: #f0d58f; }
    .p-err { background: #ffe5e8; border-color: #f3a5b1; }
    .kv {
      display: grid;
      grid-template-columns: 110px 1fr;
      gap: 5px 8px;
      font-family: var(--mono);
      font-size: 0.83rem;
      margin-bottom: 10px;
    }
    .kv b { font-family: var(--display); font-weight: 600; }
    pre {
      margin: 0;
      border: 1px solid var(--line);
      border-radius: 11px;
      background: #0f1720;
      color: #c7f9f0;
      padding: 10px;
      font-size: 0.77rem;
      font-family: var(--mono);
      min-height: 170px;
      max-height: 310px;
      overflow: auto;
      white-space: pre-wrap;
      word-break: break-word;
    }
    .note {
      margin-top: 8px;
      font-size: 0.76rem;
      opacity: 0.8;
    }
    .auth-grid {
      display: grid;
      grid-template-columns: 1fr auto auto;
      gap: 8px;
      align-items: end;
      margin-bottom: 10px;
    }
    @media (max-width: 640px) {
      .auth-grid { grid-template-columns: 1fr; }
    }
    .tiny {
      font-size: 0.75rem;
      opacity: 0.75;
      margin: 5px 0 2px;
    }
  </style>
</head>
<body>
  <main class="shell">
    <header class="head">
      <h1>Opta FTPS Live Harness</h1>
      <p>Use this LAN page to iterate FTPS settings, run small transfers, and monitor outcomes without reflashing.</p>
    </header>
    <section class="grid">
      <article class="card">
        <h2>Connection and Transfer Inputs</h2>
        <div class="auth-grid">
          <div>
            <label for="auth-pass">Harness Passcode</label>
            <input id="auth-pass" type="password" placeholder="change-me-opta">
          </div>
          <button class="accent" id="btn-auth">Unlock</button>
          <span id="auth-state" class="pill p-warn">Locked</span>
        </div>
        <p class="tiny">Auth gate is lightweight and LAN-focused. Change kHarnessPasscode in this sketch before broader test exposure.</p>
        <div class="fields">
          <div>
            <label for="host">Host</label>
            <input id="host" placeholder="192.168.1.100">
          </div>
          <div>
            <label for="port">Port</label>
            <input id="port" type="number" min="1" max="65535" value="21">
          </div>
          <div>
            <label for="user">User</label>
            <input id="user" placeholder="testuser">
          </div>
          <div>
            <label for="pass">Password</label>
            <input id="pass" type="password" placeholder="testpass">
          </div>
          <div>
            <label for="trust">Trust Mode</label>
            <select id="trust">
              <option value="fingerprint">Fingerprint</option>
              <option value="imported">ImportedCert</option>
            </select>
          </div>
          <div>
            <label for="sni">TLS Server Name (optional)</label>
            <input id="sni" placeholder="nas.local">
          </div>
          <div class="row-span">
            <label for="fp">SHA-256 Fingerprint</label>
            <input id="fp" placeholder="64 hex chars; separators allowed">
          </div>
          <div class="row-span">
            <label for="path">Remote Path</label>
            <input id="path" placeholder="/ftps_test/opta_web_harness.txt">
          </div>
          <div class="row-span">
            <label for="payload">Upload Payload (small text)</label>
            <textarea id="payload"></textarea>
          </div>
        </div>
        <div class="btns">
          <button class="accent" id="btn-connect">Connect</button>
          <button id="btn-quit">Quit</button>
          <button id="btn-upload">Upload</button>
          <button id="btn-download">Download</button>
          <button id="btn-export">Export Report</button>
        </div>
        <p class="note">ImportedCert mode expects a compile-time PEM in this sketch. Config updates use POST so credentials are not sent in the URL.</p>
      </article>

      <article class="card">
        <h2>Live Status</h2>
        <div class="status-top">
          <div><span id="pill-conn" class="pill p-warn">Connection: unknown</span></div>
          <div><span id="pill-op" class="pill p-warn">Operation: idle</span></div>
        </div>

        <div class="kv">
          <b>Action</b><span id="last-action">idle</span>
          <b>Success</b><span id="last-success">false</span>
          <b>FtpsError</b><span id="last-error">0</span>
          <b>Duration</b><span id="last-duration">0 ms</span>
          <b>Bytes</b><span id="last-bytes">0</span>
          <b>Message</b><span id="last-message">Ready.</span>
        </div>

        <label for="download-preview">Last Download Preview</label>
        <pre id="download-preview"></pre>

        <label for="logs" style="margin-top:10px; display:block;">Recent Logs</label>
        <pre id="logs"></pre>
      </article>
    </section>
  </main>

  <script>
    const s = {
      hydrated: false,
      pending: false,
      authorized: false,
      authToken: '',
      ids: {
        authPass: document.getElementById('auth-pass'),
        authState: document.getElementById('auth-state'),
        authBtn: document.getElementById('btn-auth'),
        exportBtn: document.getElementById('btn-export'),
        host: document.getElementById('host'),
        port: document.getElementById('port'),
        user: document.getElementById('user'),
        pass: document.getElementById('pass'),
        trust: document.getElementById('trust'),
        sni: document.getElementById('sni'),
        fp: document.getElementById('fp'),
        path: document.getElementById('path'),
        payload: document.getElementById('payload'),
        connPill: document.getElementById('pill-conn'),
        opPill: document.getElementById('pill-op'),
        lastAction: document.getElementById('last-action'),
        lastSuccess: document.getElementById('last-success'),
        lastError: document.getElementById('last-error'),
        lastDuration: document.getElementById('last-duration'),
        lastBytes: document.getElementById('last-bytes'),
        lastMessage: document.getElementById('last-message'),
        downloadPreview: document.getElementById('download-preview'),
        logs: document.getElementById('logs'),
        actionButtons: [
          document.getElementById('btn-connect'),
          document.getElementById('btn-quit'),
          document.getElementById('btn-upload'),
          document.getElementById('btn-download'),
          document.getElementById('btn-export')
        ]
      }
    };

    function authHeaders() {
      const headers = {};
      if (s.authToken) {
        headers['X-Harness-Token'] = s.authToken;
      }
      return headers;
    }

    function renderAuthState(message) {
      if (s.authorized) {
        s.ids.authState.textContent = 'Unlocked';
        s.ids.authState.className = 'pill p-ok';
      } else {
        s.ids.authState.textContent = message || 'Locked';
        s.ids.authState.className = 'pill p-warn';
      }
    }

    function setActionButtonsDisabled(disabled) {
      const block = disabled || !s.authorized;
      s.ids.actionButtons.forEach((btn) => { btn.disabled = block; });
    }

    function configParams() {
      const p = new URLSearchParams();
      p.set('host', s.ids.host.value.trim());
      p.set('port', s.ids.port.value.trim());
      p.set('user', s.ids.user.value);
      p.set('pass', s.ids.pass.value);
      p.set('sni', s.ids.sni.value.trim());
      p.set('trust', s.ids.trust.value);
      p.set('fp', s.ids.fp.value.trim());
      p.set('path', s.ids.path.value.trim());
      p.set('payload', s.ids.payload.value);
      return p;
    }

    function hydrateForm(data) {
      if (s.hydrated || !data) {
        return;
      }
      s.ids.host.value = data.host || '';
      s.ids.port.value = data.port || 21;
      s.ids.user.value = data.user || '';
      s.ids.trust.value = data.trustMode || 'fingerprint';
      s.ids.sni.value = data.tlsServerName || '';
      s.ids.fp.value = data.fingerprint || '';
      s.ids.path.value = data.remotePath || '';
      s.ids.payload.value = data.uploadPayload || '';
      s.hydrated = true;
    }

    function applyUnauthorized(message) {
      s.authorized = false;
      s.authToken = '';
      renderAuthState(message || 'Locked');
      setActionButtonsDisabled(false);
      s.ids.lastMessage.textContent = message || 'Auth required.';
    }

    function renderStatus(data) {
      if (!data) {
        return;
      }

      hydrateForm(data.config);

      s.ids.lastAction.textContent = data.lastAction;
      s.ids.lastSuccess.textContent = String(data.lastSuccess);
      s.ids.lastError.textContent = String(data.lastError);
      s.ids.lastDuration.textContent = String(data.lastDurationMs) + ' ms';
      s.ids.lastBytes.textContent = String(data.lastBytes);
      s.ids.lastMessage.textContent = data.lastMessage;
      s.ids.downloadPreview.textContent = data.downloadPreview || '';
      s.ids.logs.textContent = (data.logs || []).join('\n');

      s.ids.connPill.textContent = data.connected ? 'Connection: connected' : 'Connection: disconnected';
      s.ids.connPill.className = 'pill ' + (data.connected ? 'p-ok' : 'p-warn');

      const opPillClass = data.lastSuccess ? 'p-ok' : (data.lastError === 0 ? 'p-warn' : 'p-err');
      s.ids.opPill.textContent = 'Operation: ' + data.lastAction;
      s.ids.opPill.className = 'pill ' + opPillClass;

      if (!s.pending) {
        setActionButtonsDisabled(Boolean(data.busy));
      }
    }

    async function unlockHarness() {
      if (s.pending) {
        return;
      }

      const passcode = s.ids.authPass.value;
      if (!passcode) {
        renderAuthState('Passcode required');
        return;
      }

      s.pending = true;
      s.ids.authBtn.disabled = true;

      try {
        const body = new URLSearchParams();
        body.set('passcode', passcode);

        const res = await fetch('/auth', {
          method: 'POST',
          cache: 'no-store',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body.toString()
        });

        const data = await res.json();
        if (res.ok && data.ok && data.token) {
          s.authToken = data.token;
          s.authorized = true;
          renderAuthState(data.message || 'Unlocked');
          await refreshStatus();
        } else {
          applyUnauthorized(data.message || 'Unlock failed');
        }
      } catch (error) {
        console.error(error);
        applyUnauthorized('Unlock failed');
      } finally {
        s.pending = false;
        s.ids.authBtn.disabled = false;
      }
    }

    async function saveConfig() {
      const headers = authHeaders();
      headers['Content-Type'] = 'application/x-www-form-urlencoded';

      const res = await fetch('/set', {
        method: 'POST',
        cache: 'no-store',
        headers,
        body: configParams().toString()
      });

      if (res.status === 401) {
        const data = await res.json();
        applyUnauthorized(data.message || 'Auth required');
        throw new Error('Unauthorized');
      }

      return res.json();
    }

    async function doAction(op) {
      if (s.pending || !s.authorized) {
        return;
      }

      s.pending = true;
      setActionButtonsDisabled(true);

      try {
        const setResult = await saveConfig();
        renderStatus(setResult);

        const actionRes = await fetch('/action?op=' + encodeURIComponent(op), {
          cache: 'no-store',
          headers: authHeaders()
        });

        if (actionRes.status === 401) {
          const authData = await actionRes.json();
          applyUnauthorized(authData.message || 'Auth required');
          return;
        }

        const actionData = await actionRes.json();
        renderStatus(actionData);
      } catch (error) {
        console.error(error);
      } finally {
        s.pending = false;
        await refreshStatus();
      }
    }

    async function exportReport() {
      if (s.pending || !s.authorized) {
        return;
      }

      s.pending = true;
      setActionButtonsDisabled(true);

      try {
        const res = await fetch('/report', {
          cache: 'no-store',
          headers: authHeaders()
        });

        if (res.status === 401) {
          const authData = await res.json();
          applyUnauthorized(authData.message || 'Auth required');
          return;
        }

        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'opta-ftps-report.txt';
        document.body.appendChild(a);
        a.click();
        a.remove();
        URL.revokeObjectURL(url);
      } catch (error) {
        console.error(error);
      } finally {
        s.pending = false;
        await refreshStatus();
      }
    }

    async function refreshStatus() {
      if (!s.authorized) {
        setActionButtonsDisabled(false);
        return;
      }

      try {
        const res = await fetch('/status', {
          cache: 'no-store',
          headers: authHeaders()
        });

        if (res.status === 401) {
          const authData = await res.json();
          applyUnauthorized(authData.message || 'Auth required');
          return;
        }

        const data = await res.json();
        renderStatus(data);
      } catch (error) {
        console.error(error);
      }
    }

    document.getElementById('btn-auth').addEventListener('click', () => unlockHarness());
    document.getElementById('btn-connect').addEventListener('click', () => doAction('connect'));
    document.getElementById('btn-quit').addEventListener('click', () => doAction('quit'));
    document.getElementById('btn-upload').addEventListener('click', () => doAction('upload'));
    document.getElementById('btn-download').addEventListener('click', () => doAction('download'));
    document.getElementById('btn-export').addEventListener('click', () => exportReport());

    renderAuthState('Locked');
    setActionButtonsDisabled(false);
    setInterval(refreshStatus, 1800);
  </script>
</body>
</html>
)HTML";

bool hasValue(const char *value) {
  return value != nullptr && value[0] != '\0';
}

bool copyStringToBuffer(const char *input, char *output, size_t outputSize) {
  if (output == nullptr || outputSize == 0) {
    return false;
  }

  if (input == nullptr) {
    output[0] = '\0';
    return true;
  }

  int len = snprintf(output, outputSize, "%s", input);
  if (len < 0 || static_cast<size_t>(len) >= outputSize) {
    output[0] = '\0';
    return false;
  }

  return true;
}

const char *trustModeName(FtpsTrustMode mode) {
  return mode == FtpsTrustMode::Fingerprint ? "fingerprint" : "imported";
}

void generateAuthToken() {
  static const char kHex[] = "0123456789ABCDEF";

  uint32_t state = millis() ^ micros() ^ 0xA5A5F00DU;
  for (size_t i = 0; i < kAuthTokenLen - 1U; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    gAuthToken[i] = kHex[state & 0x0FU];
  }

  gAuthToken[kAuthTokenLen - 1U] = '\0';
}

bool authTokenMatches(const char *incomingToken) {
  if (!kEnableAuthGate) {
    return true;
  }

  if (!hasValue(gAuthToken) || !hasValue(incomingToken)) {
    return false;
  }

  return strcmp(gAuthToken, incomingToken) == 0;
}

bool routeRequiresAuth(const char *path) {
  if (!kEnableAuthGate) {
    return false;
  }

  if (path == nullptr) {
    return true;
  }

  return strcmp(path, "/status") == 0 ||
         strcmp(path, "/set") == 0 ||
         strcmp(path, "/action") == 0 ||
         strcmp(path, "/report") == 0;
}

void resetConfigToDefaults() {
  copyStringToBuffer(kDefaultHost, gConfig.host, sizeof(gConfig.host));
  gConfig.port = kDefaultPort;
  copyStringToBuffer(kDefaultUser, gConfig.user, sizeof(gConfig.user));
  copyStringToBuffer(kDefaultPassword, gConfig.password, sizeof(gConfig.password));
  copyStringToBuffer(kDefaultTlsServerName, gConfig.tlsServerName, sizeof(gConfig.tlsServerName));
  gConfig.trustMode = FtpsTrustMode::Fingerprint;
  copyStringToBuffer(kDefaultFingerprint, gConfig.fingerprint, sizeof(gConfig.fingerprint));
  copyStringToBuffer(kDefaultRemotePath, gConfig.remotePath, sizeof(gConfig.remotePath));
  copyStringToBuffer(kDefaultUploadPayload, gConfig.uploadPayload, sizeof(gConfig.uploadPayload));
}

void pushLog(const char *format, ...) {
  char line[kLogLineLen] = {};
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  size_t slot = (gLogHead + gLogCount) % kLogLineCount;
  if (gLogCount == kLogLineCount) {
    slot = gLogHead;
    gLogHead = (gLogHead + 1U) % kLogLineCount;
  } else {
    ++gLogCount;
  }

  unsigned long elapsedSec = millis() / 1000UL;
  snprintf(gLogLines[slot], sizeof(gLogLines[slot]), "%lus %s", elapsedSec, line);
  Serial.print("[LOG] ");
  Serial.println(gLogLines[slot]);
}

void setOutcome(const char *action,
                bool success,
                FtpsError error,
                const char *message,
                size_t bytes,
                unsigned long durationMs) {
  gStatus.lastSuccess = success;
  gStatus.lastError = error;
  gStatus.lastBytes = bytes;
  gStatus.lastDurationMs = durationMs;
  copyStringToBuffer(action, gStatus.lastAction, sizeof(gStatus.lastAction));
  copyStringToBuffer(message, gStatus.lastMessage, sizeof(gStatus.lastMessage));

  pushLog("%s %s (FtpsError=%d) %s",
          action,
          success ? "PASS" : "FAIL",
          static_cast<int>(error),
          message);
}

bool readHttpLine(EthernetClient &client, char *buffer, size_t bufferSize) {
  if (buffer == nullptr || bufferSize == 0) {
    return false;
  }

  size_t pos = 0;
  unsigned long start = millis();

  while (millis() - start < kHttpReadTimeoutMs) {
    while (client.available() > 0) {
      char ch = static_cast<char>(client.read());

      if (ch == '\r') {
        continue;
      }

      if (ch == '\n') {
        buffer[pos] = '\0';
        return true;
      }

      if (pos + 1U < bufferSize) {
        buffer[pos++] = ch;
      }
    }

    if (!client.connected()) {
      break;
    }

    delay(1);
  }

  buffer[pos] = '\0';
  return pos > 0;
}

bool startsWithIgnoreCase(const char *text, const char *prefix) {
  if (text == nullptr || prefix == nullptr) {
    return false;
  }

  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    char a = text[i];
    char b = prefix[i];
    if (a == '\0') {
      return false;
    }

    if (tolower(static_cast<unsigned char>(a)) !=
        tolower(static_cast<unsigned char>(b))) {
      return false;
    }
  }

  return true;
}

bool extractHeaderValue(const char *line,
                        const char *headerName,
                        char *valueOut,
                        size_t valueOutSize) {
  if (!startsWithIgnoreCase(line, headerName)) {
    return false;
  }

  const char *cursor = line + strlen(headerName);
  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }

  return copyStringToBuffer(cursor, valueOut, valueOutSize);
}

bool readHttpBody(EthernetClient &client,
                  char *buffer,
                  size_t bufferSize,
                  size_t bodyLength) {
  if (buffer == nullptr || bufferSize == 0) {
    return false;
  }

  if (bodyLength + 1U > bufferSize) {
    return false;
  }

  size_t read = 0;
  unsigned long start = millis();
  while (read < bodyLength && (millis() - start) < kHttpReadTimeoutMs) {
    while (client.available() > 0 && read < bodyLength) {
      buffer[read++] = static_cast<char>(client.read());
    }

    if (read >= bodyLength) {
      break;
    }

    if (!client.connected()) {
      break;
    }

    delay(1);
  }

  if (read != bodyLength) {
    return false;
  }

  buffer[read] = '\0';
  return true;
}

bool hexToNibble(char ch, uint8_t &out) {
  if (ch >= '0' && ch <= '9') {
    out = static_cast<uint8_t>(ch - '0');
    return true;
  }

  if (ch >= 'A' && ch <= 'F') {
    out = static_cast<uint8_t>(10 + (ch - 'A'));
    return true;
  }

  if (ch >= 'a' && ch <= 'f') {
    out = static_cast<uint8_t>(10 + (ch - 'a'));
    return true;
  }

  return false;
}

bool urlDecode(const char *source, char *dest, size_t destLen) {
  if (source == nullptr || dest == nullptr || destLen == 0) {
    return false;
  }

  size_t out = 0;
  for (size_t i = 0; source[i] != '\0'; ++i) {
    char ch = source[i];

    if (ch == '+') {
      ch = ' ';
    } else if (ch == '%' && source[i + 1] != '\0' && source[i + 2] != '\0') {
      uint8_t high = 0;
      uint8_t low = 0;
      if (!hexToNibble(source[i + 1], high) || !hexToNibble(source[i + 2], low)) {
        return false;
      }
      ch = static_cast<char>((high << 4U) | low);
      i += 2;
    }

    if (out + 1U >= destLen) {
      return false;
    }

    dest[out++] = ch;
  }

  dest[out] = '\0';
  return true;
}

bool parsePort(const char *text, uint16_t &portOut) {
  if (!hasValue(text)) {
    return false;
  }

  char *end = nullptr;
  unsigned long parsed = strtoul(text, &end, 10);
  if (end == nullptr || *end != '\0' || parsed == 0 || parsed > 65535UL) {
    return false;
  }

  portOut = static_cast<uint16_t>(parsed);
  return true;
}

bool applyConfigFromQuery(const char *query, char *message, size_t messageSize) {
  if (!hasValue(query)) {
    copyStringToBuffer("No config parameters supplied.", message, messageSize);
    return true;
  }

  char queryCopy[kQueryLen] = {};
  if (!copyStringToBuffer(query, queryCopy, sizeof(queryCopy))) {
    copyStringToBuffer("Config query is too long.", message, messageSize);
    return false;
  }

  bool changedAny = false;
  char *token = strtok(queryCopy, "&");
  while (token != nullptr) {
    char *equals = strchr(token, '=');
    if (equals != nullptr) {
      *equals = '\0';

      char key[kParamKeyLen] = {};
      char value[kParamValueLen] = {};
      if (!urlDecode(token, key, sizeof(key)) || !urlDecode(equals + 1, value, sizeof(value))) {
        copyStringToBuffer("Malformed URL-encoded query value.", message, messageSize);
        return false;
      }

      if (strcmp(key, "host") == 0) {
        if (!copyStringToBuffer(value, gConfig.host, sizeof(gConfig.host))) {
          copyStringToBuffer("host value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "port") == 0) {
        uint16_t parsedPort = 0;
        if (!parsePort(value, parsedPort)) {
          copyStringToBuffer("port must be an integer in range 1..65535.", message, messageSize);
          return false;
        }
        gConfig.port = parsedPort;
        changedAny = true;
      } else if (strcmp(key, "user") == 0) {
        if (!copyStringToBuffer(value, gConfig.user, sizeof(gConfig.user))) {
          copyStringToBuffer("user value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "pass") == 0) {
        if (!copyStringToBuffer(value, gConfig.password, sizeof(gConfig.password))) {
          copyStringToBuffer("pass value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "sni") == 0) {
        if (!copyStringToBuffer(value, gConfig.tlsServerName, sizeof(gConfig.tlsServerName))) {
          copyStringToBuffer("sni value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "fp") == 0) {
        if (!copyStringToBuffer(value, gConfig.fingerprint, sizeof(gConfig.fingerprint))) {
          copyStringToBuffer("fp value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "path") == 0) {
        if (!copyStringToBuffer(value, gConfig.remotePath, sizeof(gConfig.remotePath))) {
          copyStringToBuffer("path value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "payload") == 0) {
        if (!copyStringToBuffer(value, gConfig.uploadPayload, sizeof(gConfig.uploadPayload))) {
          copyStringToBuffer("payload value is too long.", message, messageSize);
          return false;
        }
        changedAny = true;
      } else if (strcmp(key, "trust") == 0) {
        if (strcmp(value, "fingerprint") == 0) {
          gConfig.trustMode = FtpsTrustMode::Fingerprint;
          changedAny = true;
        } else if (strcmp(value, "imported") == 0) {
          gConfig.trustMode = FtpsTrustMode::ImportedCert;
          changedAny = true;
        } else {
          copyStringToBuffer("trust must be fingerprint or imported.", message, messageSize);
          return false;
        }
      }
    }

    token = strtok(nullptr, "&");
  }

  copyStringToBuffer(changedAny ? "Configuration updated." : "Configuration unchanged.",
                     message,
                     messageSize);
  return true;
}

bool readQueryValue(const char *query,
                    const char *wantedKey,
                    char *valueOut,
                    size_t valueOutSize) {
  if (!hasValue(query) || !hasValue(wantedKey) || valueOut == nullptr || valueOutSize == 0) {
    return false;
  }

  char queryCopy[kQueryLen] = {};
  if (!copyStringToBuffer(query, queryCopy, sizeof(queryCopy))) {
    return false;
  }

  char *token = strtok(queryCopy, "&");
  while (token != nullptr) {
    char *equals = strchr(token, '=');
    if (equals != nullptr) {
      *equals = '\0';

      char key[kParamKeyLen] = {};
      if (urlDecode(token, key, sizeof(key)) && strcmp(key, wantedKey) == 0) {
        return urlDecode(equals + 1, valueOut, valueOutSize);
      }
    }

    token = strtok(nullptr, "&");
  }

  return false;
}

bool ensureFtpsBegun(char *error, size_t errorSize) {
  if (gFtpsBegun) {
    return true;
  }

  if (!gFtps.begin(Ethernet.getNetwork(), error, errorSize)) {
    return false;
  }

  gFtpsBegun = true;
  return true;
}

bool runConnectAction(char *message, size_t messageSize, size_t &bytesOut) {
  bytesOut = 0;

  if (!hasValue(gConfig.host) || !hasValue(gConfig.user) || !hasValue(gConfig.password)) {
    copyStringToBuffer("host, user, and pass are required.", message, messageSize);
    return false;
  }

  if (gConfig.trustMode == FtpsTrustMode::Fingerprint && !hasValue(gConfig.fingerprint)) {
    copyStringToBuffer("Fingerprint trust mode requires fp.", message, messageSize);
    return false;
  }

  if (gConfig.trustMode == FtpsTrustMode::ImportedCert && !hasValue(kImportedRootCaPem)) {
    copyStringToBuffer("ImportedCert trust mode selected, but kImportedRootCaPem is not set in sketch.",
                       message,
                       messageSize);
    return false;
  }

  char error[192] = {};
  if (!ensureFtpsBegun(error, sizeof(error))) {
    copyStringToBuffer(hasValue(error) ? error : "FtpsClient.begin() failed.", message, messageSize);
    gStatus.connected = false;
    return false;
  }

  FtpsServerConfig config;
  config.host = gConfig.host;
  config.port = gConfig.port;
  config.user = gConfig.user;
  config.password = gConfig.password;
  config.tlsServerName = hasValue(gConfig.tlsServerName) ? gConfig.tlsServerName : nullptr;
  config.trustMode = gConfig.trustMode;
  config.fingerprint = (gConfig.trustMode == FtpsTrustMode::Fingerprint) ? gConfig.fingerprint : nullptr;
  config.rootCaPem = (gConfig.trustMode == FtpsTrustMode::ImportedCert) ? kImportedRootCaPem : nullptr;
  config.validateServerCert = true;

  if (!gFtps.connect(config, error, sizeof(error))) {
    copyStringToBuffer(hasValue(error) ? error : "connect() failed.", message, messageSize);
    gStatus.connected = false;
    return false;
  }

  gStatus.connected = true;
  copyStringToBuffer("Control channel connected.", message, messageSize);
  return true;
}

bool runUploadAction(char *message, size_t messageSize, size_t &bytesOut) {
  bytesOut = 0;

  if (!gStatus.connected) {
    copyStringToBuffer("Connect first.", message, messageSize);
    return false;
  }

  if (!hasValue(gConfig.remotePath)) {
    copyStringToBuffer("Remote path is required.", message, messageSize);
    return false;
  }

  if (!hasValue(gConfig.uploadPayload)) {
    copyStringToBuffer("Upload payload is empty.", message, messageSize);
    return false;
  }

  char error[192] = {};
  size_t payloadLen = strlen(gConfig.uploadPayload);
  if (!gFtps.store(gConfig.remotePath,
                   reinterpret_cast<const uint8_t *>(gConfig.uploadPayload),
                   payloadLen,
                   error,
                   sizeof(error))) {
    copyStringToBuffer(hasValue(error) ? error : "store() failed.", message, messageSize);
    return false;
  }

  bytesOut = payloadLen;
  copyStringToBuffer("Upload completed.", message, messageSize);
  return true;
}

void buildDownloadPreview(const uint8_t *buffer, size_t bytesRead) {
  size_t maxCopy = bytesRead;
  if (maxCopy > sizeof(gStatus.downloadPreview) - 1U) {
    maxCopy = sizeof(gStatus.downloadPreview) - 1U;
  }

  size_t out = 0;
  for (size_t i = 0; i < maxCopy; ++i) {
    char ch = static_cast<char>(buffer[i]);
    if ((ch >= 32 && ch <= 126) || ch == '\r' || ch == '\n' || ch == '\t') {
      gStatus.downloadPreview[out++] = ch;
    } else {
      gStatus.downloadPreview[out++] = '.';
    }
  }

  gStatus.downloadPreview[out] = '\0';
}

bool runDownloadAction(char *message, size_t messageSize, size_t &bytesOut) {
  bytesOut = 0;
  gStatus.downloadPreview[0] = '\0';
  gStatus.downloadBytes = 0;

  if (!gStatus.connected) {
    copyStringToBuffer("Connect first.", message, messageSize);
    return false;
  }

  if (!hasValue(gConfig.remotePath)) {
    copyStringToBuffer("Remote path is required.", message, messageSize);
    return false;
  }

  char error[192] = {};
  uint8_t buffer[kDownloadBufferSize] = {};
  size_t bytesRead = 0;
  if (!gFtps.retrieve(gConfig.remotePath,
                      buffer,
                      sizeof(buffer),
                      bytesRead,
                      error,
                      sizeof(error))) {
    copyStringToBuffer(hasValue(error) ? error : "retrieve() failed.", message, messageSize);
    return false;
  }

  buildDownloadPreview(buffer, bytesRead);
  gStatus.downloadBytes = bytesRead;
  bytesOut = bytesRead;
  copyStringToBuffer("Download completed.", message, messageSize);
  return true;
}

bool runQuitAction(char *message, size_t messageSize, size_t &bytesOut) {
  bytesOut = 0;

  gFtps.quit();
  gStatus.connected = false;

  if (gFtps.lastError() == FtpsError::QuitFailed) {
    copyStringToBuffer("QUIT did not receive a 221 reply before shutdown.", message, messageSize);
    return false;
  }

  copyStringToBuffer("Connection closed.", message, messageSize);
  return true;
}

void jsonPrintEscaped(EthernetClient &client, const char *value) {
  client.print('"');

  if (value != nullptr) {
    for (size_t i = 0; value[i] != '\0'; ++i) {
      char ch = value[i];
      if (ch == '"') {
        client.print(F("\\\""));
      } else if (ch == '\\') {
        client.print(F("\\\\"));
      } else if (ch == '\n') {
        client.print(F("\\n"));
      } else if (ch == '\r') {
        client.print(F("\\r"));
      } else if (ch == '\t') {
        client.print(F("\\t"));
      } else {
        client.print(ch);
      }
    }
  }

  client.print('"');
}

void sendHeader(EthernetClient &client, const char *status, const char *contentType) {
  client.print(F("HTTP/1.1 "));
  client.println(status);
  client.print(F("Content-Type: "));
  client.println(contentType);
  client.println(F("Cache-Control: no-store, no-cache, must-revalidate"));
  client.println(F("Pragma: no-cache"));
  client.println(F("Connection: close"));
  client.println();
}

void sendAuthJson(EthernetClient &client,
                  const char *statusCode,
                  bool ok,
                  const char *message,
                  const char *token = nullptr) {
  sendHeader(client, statusCode, "application/json");

  client.print(F("{\"ok\":"));
  client.print(ok ? F("true") : F("false"));
  client.print(F(",\"message\":"));
  jsonPrintEscaped(client, message);
  if (ok && token != nullptr) {
    client.print(F(",\"token\":"));
    jsonPrintEscaped(client, token);
  }
  client.print(F("}"));
}

void sendReportText(EthernetClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/plain; charset=utf-8"));
  client.println(F("Content-Disposition: attachment; filename=opta-ftps-report.txt"));
  client.println(F("Cache-Control: no-store, no-cache, must-revalidate"));
  client.println(F("Pragma: no-cache"));
  client.println(F("Connection: close"));
  client.println();

  client.println(F("Opta FTPS Web Harness Report"));
  client.println(F("============================"));
  client.print(F("uptime_sec="));
  client.println(millis() / 1000UL);
  client.print(F("connected="));
  client.println(gStatus.connected ? F("true") : F("false"));
  client.print(F("busy="));
  client.println(gStatus.busy ? F("true") : F("false"));
  client.print(F("last_action="));
  client.println(gStatus.lastAction);
  client.print(F("last_success="));
  client.println(gStatus.lastSuccess ? F("true") : F("false"));
  client.print(F("last_error="));
  client.println(static_cast<int>(gStatus.lastError));
  client.print(F("last_duration_ms="));
  client.println(gStatus.lastDurationMs);
  client.print(F("last_bytes="));
  client.println(static_cast<unsigned long>(gStatus.lastBytes));
  client.print(F("last_message="));
  client.println(gStatus.lastMessage);

  client.println();
  client.println(F("Config snapshot"));
  client.println(F("---------------"));
  client.print(F("host="));
  client.println(gConfig.host);
  client.print(F("port="));
  client.println(gConfig.port);
  client.print(F("user="));
  client.println(gConfig.user);
  client.print(F("password_set="));
  client.println(hasValue(gConfig.password) ? F("true") : F("false"));
  client.print(F("trust_mode="));
  client.println(trustModeName(gConfig.trustMode));
  client.print(F("tls_server_name="));
  client.println(gConfig.tlsServerName);
  client.print(F("remote_path="));
  client.println(gConfig.remotePath);

  client.println();
  client.println(F("Download preview"));
  client.println(F("----------------"));
  client.println(gStatus.downloadPreview);

  client.println();
  client.println(F("Recent logs"));
  client.println(F("-----------"));
  for (size_t i = 0; i < gLogCount; ++i) {
    size_t index = (gLogHead + i) % kLogLineCount;
    client.println(gLogLines[index]);
  }
}

void sendStatusJson(EthernetClient &client, const char *statusCode = "200 OK") {
  sendHeader(client, statusCode, "application/json");

  client.print(F("{"));

  client.print(F("\"busy\":"));
  client.print(gStatus.busy ? F("true") : F("false"));
  client.print(F(",\"connected\":"));
  client.print(gStatus.connected ? F("true") : F("false"));
  client.print(F(",\"lastSuccess\":"));
  client.print(gStatus.lastSuccess ? F("true") : F("false"));

  client.print(F(",\"lastError\":"));
  client.print(static_cast<int>(gStatus.lastError));

  client.print(F(",\"lastBytes\":"));
  client.print(static_cast<unsigned long>(gStatus.lastBytes));

  client.print(F(",\"downloadBytes\":"));
  client.print(static_cast<unsigned long>(gStatus.downloadBytes));

  client.print(F(",\"lastDurationMs\":"));
  client.print(gStatus.lastDurationMs);

  client.print(F(",\"lastAction\":"));
  jsonPrintEscaped(client, gStatus.lastAction);

  client.print(F(",\"lastMessage\":"));
  jsonPrintEscaped(client, gStatus.lastMessage);

  client.print(F(",\"downloadPreview\":"));
  jsonPrintEscaped(client, gStatus.downloadPreview);

  client.print(F(",\"config\":{"));
  client.print(F("\"host\":"));
  jsonPrintEscaped(client, gConfig.host);

  client.print(F(",\"port\":"));
  client.print(gConfig.port);

  client.print(F(",\"user\":"));
  jsonPrintEscaped(client, gConfig.user);

  client.print(F(",\"passwordSet\":"));
  client.print(hasValue(gConfig.password) ? F("true") : F("false"));

  client.print(F(",\"tlsServerName\":"));
  jsonPrintEscaped(client, gConfig.tlsServerName);

  client.print(F(",\"trustMode\":"));
  jsonPrintEscaped(client, trustModeName(gConfig.trustMode));

  client.print(F(",\"fingerprint\":"));
  jsonPrintEscaped(client, gConfig.fingerprint);

  client.print(F(",\"remotePath\":"));
  jsonPrintEscaped(client, gConfig.remotePath);

  client.print(F(",\"uploadPayload\":"));
  jsonPrintEscaped(client, gConfig.uploadPayload);

  client.print(F("}"));

  client.print(F(",\"logs\":["));
  for (size_t i = 0; i < gLogCount; ++i) {
    if (i > 0) {
      client.print(',');
    }
    size_t index = (gLogHead + i) % kLogLineCount;
    jsonPrintEscaped(client, gLogLines[index]);
  }
  client.print(F("]"));

  client.print(F("}"));
}

void sendHtmlPage(EthernetClient &client) {
  sendHeader(client, "200 OK", "text/html; charset=utf-8");
  client.print(kWebPage);
}

void sendTextResponse(EthernetClient &client, const char *statusCode, const char *text) {
  sendHeader(client, statusCode, "text/plain; charset=utf-8");
  client.println(text);
}

void handleAuthRequest(const char *method,
                       const char *body,
                       EthernetClient &client) {
  if (!kEnableAuthGate) {
    sendAuthJson(client, "200 OK", true, "Auth gate disabled.", "no-auth-needed");
    return;
  }

  if (strcmp(method, "POST") != 0) {
    sendAuthJson(client, "405 Method Not Allowed", false, "Use POST /auth.");
    return;
  }

  char passcode[kAuthPasscodeLen] = {};
  if (!readQueryValue(body, "passcode", passcode, sizeof(passcode))) {
    sendAuthJson(client, "400 Bad Request", false, "Missing passcode field.");
    return;
  }

  if (strcmp(passcode, kHarnessPasscode) != 0) {
    sendAuthJson(client, "401 Unauthorized", false, "Invalid harness passcode.");
    return;
  }

  generateAuthToken();
  pushLog("Harness auth unlocked for current token session.");
  sendAuthJson(client, "200 OK", true, "Harness unlocked.", gAuthToken);
}

void handleActionRequest(const char *query, EthernetClient &client) {
  char op[kParamValueLen] = {};
  if (!readQueryValue(query, "op", op, sizeof(op))) {
    copyStringToBuffer("Missing op query parameter.", gStatus.lastMessage, sizeof(gStatus.lastMessage));
    setOutcome("action", false, FtpsError::ConnectionFailed, gStatus.lastMessage, 0, 0);
    sendStatusJson(client, "400 Bad Request");
    return;
  }

  if (gStatus.busy) {
    setOutcome("busy", false, FtpsError::ConnectionFailed, "Another action is currently running.", 0, 0);
    sendStatusJson(client, "409 Conflict");
    return;
  }

  gStatus.busy = true;
  unsigned long startMs = millis();
  bool ok = false;
  size_t bytes = 0;
  char message[kStatusMessageLen] = {};

  if (strcmp(op, "connect") == 0) {
    ok = runConnectAction(message, sizeof(message), bytes);
    setOutcome("connect", ok, gFtps.lastError(), message, bytes, millis() - startMs);
  } else if (strcmp(op, "upload") == 0) {
    ok = runUploadAction(message, sizeof(message), bytes);
    setOutcome("upload", ok, gFtps.lastError(), message, bytes, millis() - startMs);
  } else if (strcmp(op, "download") == 0) {
    ok = runDownloadAction(message, sizeof(message), bytes);
    setOutcome("download", ok, gFtps.lastError(), message, bytes, millis() - startMs);
  } else if (strcmp(op, "quit") == 0) {
    ok = runQuitAction(message, sizeof(message), bytes);
    setOutcome("quit", ok, gFtps.lastError(), message, bytes, millis() - startMs);
  } else {
    setOutcome("action", false, FtpsError::ConnectionFailed, "Unknown op. Use connect, upload, download, or quit.", 0, millis() - startMs);
    gStatus.busy = false;
    sendStatusJson(client, "400 Bad Request");
    return;
  }

  gStatus.busy = false;
  sendStatusJson(client, ok ? "200 OK" : "500 Internal Server Error");
}

void handleSetRequest(const char *query, EthernetClient &client) {
  if (gStatus.busy) {
    setOutcome("set", false, FtpsError::ConnectionFailed, "Cannot update config while action is running.", 0, 0);
    sendStatusJson(client, "409 Conflict");
    return;
  }

  char message[kStatusMessageLen] = {};
  bool ok = applyConfigFromQuery(query, message, sizeof(message));
  setOutcome("set", ok, FtpsError::None, message, 0, 0);
  sendStatusJson(client, ok ? "200 OK" : "400 Bad Request");
}

void handleClient(EthernetClient &client) {
  char requestLine[kRequestLineLen] = {};
  if (!readHttpLine(client, requestLine, sizeof(requestLine))) {
    client.stop();
    return;
  }

  char method[8] = {};
  char target[kRequestLineLen] = {};
  if (sscanf(requestLine, "%7s %767s", method, target) != 2) {
    sendTextResponse(client, "400 Bad Request", "Malformed request line.");
    client.stop();
    return;
  }

  size_t contentLength = 0;
  char authHeaderToken[kAuthTokenLen] = {};

  char headerLine[kHeaderLineLen] = {};
  while (readHttpLine(client, headerLine, sizeof(headerLine))) {
    if (headerLine[0] == '\0') {
      break;
    }

    char headerValue[kParamValueLen] = {};
    if (extractHeaderValue(headerLine, "Content-Length:", headerValue, sizeof(headerValue))) {
      char *end = nullptr;
      unsigned long parsed = strtoul(headerValue, &end, 10);
      if (end == nullptr || *end != '\0') {
        sendTextResponse(client, "400 Bad Request", "Invalid Content-Length header.");
        client.stop();
        return;
      }
      contentLength = static_cast<size_t>(parsed);
    } else if (extractHeaderValue(headerLine,
                                  "X-Harness-Token:",
                                  authHeaderToken,
                                  sizeof(authHeaderToken))) {
      // Token captured for protected routes.
    }
  }

  char body[kBodyLen] = {};
  if (contentLength > 0) {
    if (!readHttpBody(client, body, sizeof(body), contentLength)) {
      sendTextResponse(client, "400 Bad Request", "Failed to read request body.");
      client.stop();
      return;
    }
  }

  if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
    sendTextResponse(client, "405 Method Not Allowed", "Only GET and POST are supported.");
    client.stop();
    return;
  }

  char *query = nullptr;
  char *questionMark = strchr(target, '?');
  if (questionMark != nullptr) {
    *questionMark = '\0';
    query = questionMark + 1;
  }

  if (strcmp(target, "/auth") == 0) {
    handleAuthRequest(method, body, client);
    delay(1);
    client.stop();
    return;
  }

  if (routeRequiresAuth(target) && !authTokenMatches(authHeaderToken)) {
    sendAuthJson(client, "401 Unauthorized", false, "Auth required. Use POST /auth first.");
    delay(1);
    client.stop();
    return;
  }

  if (strcmp(target, "/") == 0) {
    sendHtmlPage(client);
  } else if (strcmp(target, "/status") == 0) {
    sendStatusJson(client);
  } else if (strcmp(target, "/set") == 0) {
    if (strcmp(method, "POST") == 0) {
      handleSetRequest(body, client);
    } else if (strcmp(method, "GET") == 0) {
      // Backward-compatible fallback for manual URL testing.
      handleSetRequest(query, client);
    } else {
      sendTextResponse(client, "405 Method Not Allowed", "Use GET or POST /set.");
    }
  } else if (strcmp(target, "/action") == 0) {
    if (strcmp(method, "GET") == 0) {
      handleActionRequest(query, client);
    } else {
      sendTextResponse(client, "405 Method Not Allowed", "Use GET /action?op=...");
    }
  } else if (strcmp(target, "/report") == 0) {
    if (strcmp(method, "GET") == 0) {
      sendReportText(client);
    } else {
      sendTextResponse(client, "405 Method Not Allowed", "Use GET /report.");
    }
  } else if (strcmp(target, "/favicon.ico") == 0) {
    sendTextResponse(client, "204 No Content", "");
  } else {
    sendTextResponse(client, "404 Not Found", "Route not found.");
  }

  delay(1);
  client.stop();
}

} // namespace

void setup() {
  Serial.begin(115200);
  unsigned long waitStart = millis();
  while (!Serial && millis() - waitStart < 5000) { }

  resetConfigToDefaults();
  gAuthToken[0] = '\0';

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  Opta FTPS Web Harness Live Test");
  Serial.println("==============================================");
  Serial.println("[STEP] Ethernet initialization");

  byte mac[6];
  Ethernet.MACAddress(mac);
  if (Ethernet.begin(mac) == 0) {
    setOutcome("startup", false, FtpsError::NetworkNotInitialized, "Ethernet DHCP failed.", 0, 0);
    pushLog("Ethernet.begin() failed. Web harness is not available.");

    Serial.println("[FAIL] Ethernet.begin(): DHCP failed.");
    return;
  }

  Serial.print("[PASS] Ethernet.begin(): ");
  Serial.println(Ethernet.localIP());

  gWebServer.begin();
  pushLog("Web harness listening on http://%d.%d.%d.%d/",
          Ethernet.localIP()[0],
          Ethernet.localIP()[1],
          Ethernet.localIP()[2],
          Ethernet.localIP()[3]);

  if (kEnableAuthGate) {
    pushLog("Auth gate enabled. Use /auth to unlock protected routes.");
    if (strcmp(kHarnessPasscode, "change-me-opta") == 0) {
      pushLog("WARNING: kHarnessPasscode is still default; change it before non-isolated use.");
    }
  } else {
    pushLog("Auth gate disabled by configuration.");
  }

  setOutcome("startup", true, FtpsError::None, "Web harness ready.", 0, 0);

  Serial.println("[PASS] Web harness started");
  Serial.print("[INFO] Open browser: http://");
  Serial.println(Ethernet.localIP());
}

void loop() {
  EthernetClient client = gWebServer.available();
  if (client) {
    handleClient(client);
  }
}
