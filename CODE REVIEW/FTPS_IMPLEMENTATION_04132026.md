# FTPS Implementation Note — Arduino Opta FTPS Library

**Date:** April 13, 2026  
**Scope:** General-purpose FTPS client library design for Arduino Opta  
**Status:** Design note plus implemented first pass; live hardware validation still pending

---

## Executive Summary

This document describes the design of a general-purpose Explicit FTPS client library for Arduino Opta devices. The library targets Mbed-based Ethernet/TLS transport and is intended for validation against a WD My Cloud PR4100 NAS, vsftpd, and FileZilla Server.

Building an FTPS library for the Opta requires:

1. A TLS-capable Ethernet client layer for the Opta.
2. Config types for FTPS mode and certificate validation.
3. A transport abstraction for FTP control/data connection code.
4. Trust model implementation (fingerprint pinning and imported PEM certificate trust).
5. Integration testing across upload and download paths.

### Nomenclature note (April 15, 2026)

Some legacy option-comparison snippets in this document use older names such as `IFtpTransport` and `MbedSecureSocketFtpTransport`. In the current repository scaffold, these map to `IFtpsTransport` and `MbedSecureSocketFtpsTransport`.

This implementation plan targets **Explicit TLS FTPS only**. It is the most standard option and the closest to the port-21 passive-FTP flow. Implicit TLS is retained in this document only as comparison/background material, not as part of the current implementation scope.

---

## What FTPS Fixes

- Encrypts FTP credentials on the wire.
- Encrypts file transfer payloads on the wire.
- Reduces the risk of passive sniffing on the local network.
- Keeps the existing FTP workflow mostly intact if implemented as **Explicit TLS**.

## What FTPS Does Not Fix

- It does not add HTTPS or other TLS to non-FTP traffic in the host application.
- It does not remove the need for certificate management.
- It does not eliminate application-level bugs or error handling concerns.
- It does not automatically guarantee man-in-the-middle protection unless certificate validation is enforced correctly.

---

## Recommended Mode

### Recommended for first implementation: Explicit TLS

Reasons:

- Most FTPS-capable NAS and server products support **Explicit TLS**.
- It aligns with the standard FTP port-21 workflow.
- It is the most common FTPS mode in practice.
- It minimizes disruption for applications already using plain FTP.

### Out of current scope: Implicit TLS

Reasons it is not part of this plan:

- It is older and less common.
- It typically uses a different port and slightly different connection startup behavior.
- It adds more config branches and more testing burden.
- It does not remove the harder parts of this project, such as protected passive data channels, certificate handling, and integration testing.

### Not recommended: automatic fallback from FTPS to plain FTP

If the application selects FTPS, the library should fail closed and report a TLS/auth error. Silent fallback to plaintext defeats the point of the change.

---

## Typical Plain FTP Surface That FTPS Replaces

A typical Arduino application using plain FTP has a structure like this:

### Configuration

- Host, port, username, password, remote path
- Passive mode toggle
- Defaults around FTP port `21`

### Low-level FTP transport

- An `FtpSession` containing only an `EthernetClient` for the control channel.
- `ftpSendCommand()` writes raw commands to the control socket.
- `ftpConnectAndLogin()` does plain TCP connect, reads banner, sends `USER` / `PASS`, then `TYPE I`.
- `ftpEnterPassive()` uses `PASV` and parses the IPv4 tuple.
- `ftpStoreBuffer()` and `ftpRetrieveBuffer()` open a plain `EthernetClient` data socket.
- `ftpQuit()` sends `QUIT` on the plain control socket.

### Call sites affected by FTPS

Any function in the host application that calls the low-level FTP helpers must work with the TLS-upgraded transport. Typical examples include:

- File upload operations
- File download operations
- Any periodic or event-driven transfer logic

The FTPS library replaces the low-level FTP transport layer. The host application's call sites should only need to change their session initialization, not the transfer logic itself.

---

## Required Library Components

## 1. A TLS-capable Ethernet client layer

### Current state

A typical Arduino Opta sketch includes only `PortentaEthernet.h` and `Ethernet.h` for networking. There is no existing TLS/SSL client abstraction for FTP use.

### Required implementation

Introduce a TLS-capable client implementation for Opta Ethernet, hidden behind a transport abstraction so the FTP protocol code is not hard-wired to `EthernetClient`.

### Minimum acceptable design

Create a transport abstraction along these lines:

```cpp
enum FtpSecurityMode : uint8_t {
  FTP_SECURITY_PLAIN = 0,
  FTP_SECURITY_EXPLICIT_TLS = 1,
  FTP_SECURITY_IMPLICIT_TLS = 2,
};

struct FtpSocket {
  Client *io;
  bool secure;
};

struct FtpSession {
  FtpSocket ctrl;
  FtpSecurityMode securityMode;
  bool dataProtectionEnabled;
};
```

The exact client class is still a build-time decision. The largest unknown is not the FTP logic; it is selecting and validating the TLS-capable Ethernet client implementation for Arduino Opta + Mbed + Ethernet.

### Confirmed core exposure on installed Arduino Opta core 4.5.0

This question was verified against the **installed** Arduino Opta core in:

- `c:\Users\dorkm\AppData\Local\Arduino15\packages\arduino\hardware\mbed_opta\4.5.0`

Confirmed findings:

- The Opta core build injects Mbed netsocket include paths into sketch compilation, including:
  - `mbed/connectivity/netsocket`
  - `mbed/connectivity/netsocket/include`
  - `mbed/connectivity/netsocket/include/netsocket`
- The following headers are present inside the compile-visible Mbed tree:
  - `netsocket/NetworkInterface.h`
  - `netsocket/SocketAddress.h`
  - `netsocket/TCPSocket.h`
  - `netsocket/TLSSocket.h`
  - `netsocket/TLSSocketWrapper.h`
- The bundled Opta Ethernet library already includes and exposes Mbed networking types:
  - `libraries/Ethernet/src/Ethernet.h` includes `netsocket/NetworkInterface.h`
  - `EthernetClass::getNetwork()` returns `NetworkInterface *`
  - the global `Ethernet` instance is constructed from `EthInterface::get_default_instance()`
- The bundled Opta core also ships `EthernetSSLClient`, which routes through the `SocketWrapper` library.
- The bundled `SocketWrapper` implementation uses Mbed `TLSSocket` directly for `connectSSL()`.

### Practical conclusion

Yes: the installed Arduino Opta core exposes `NetworkInterface`, `TCPSocket`, and `TLSSocketWrapper` cleanly enough to be used from a sketch.

What is now confirmed:

- API/header exposure is real, not hypothetical.
- A sketch can use `Ethernet.begin(...)` and then obtain the underlying Mbed network via `Ethernet.getNetwork()`.
- A direct Mbed-socket FTPS transport is a realistic path inside the Arduino sketch environment.

What is **not** fully confirmed by this inspection alone:

- end-to-end sketch compilation of a `TLSSocketWrapper` probe, because `arduino-cli` is not installed in this environment
- runtime behavior on the actual device for explicit FTPS control-channel upgrade and protected passive data channels

So the remaining risk is no longer “does the core expose the APIs?” The remaining risk is “how well does the chosen transport behave for explicit FTPS in practice?”

### Candidate transport comparison

The project has four realistic transport directions. These are not equivalent. The comparison remains useful background, but the current implementation plan still targets **Explicit TLS only**.

| Option | Transport style | Fit for Explicit FTPS | Fit for Implicit FTPS | Main benefit | Main risk | Current recommendation |
|------|------|------|------|------|------|------|
| `SSLClient` over `EthernetClient` | Arduino `Client` wrapper | Weak to uncertain | Good | Lowest code-style disruption from current sketch | Explicit FTPS needs a STARTTLS-style upgrade after `AUTH TLS`, which may not map cleanly onto the library | Feasibility fallback only |
| Mbed `TLSSocket` | Native Mbed TLS socket with its own TCP transport | Medium | Strong | Native TLS support, CA/cert APIs, cleaner long-term direction than Arduino-only wrappers | Best fit for TLS-from-connect flows; explicit FTPS still needs an upgrade path or a wrapper | Confirmed available |
| Mbed `TLSSocketWrapper` / secure-socket path | Wrap an existing `TCPSocket` in TLS after the plain TCP phase | Strong | Strong | Best match for `AUTH TLS` because the control socket can begin in plaintext and then be wrapped | Larger refactor from current `EthernetClient`-based code; still needs compile and on-device validation | **Preferred exploration target** |
| Custom wrapper around Mbed TLS or BearSSL | Project-owned TLS adapter | Strong | Strong | Maximum control over FTP-specific behavior | Highest implementation and maintenance cost | Last resort only |

### Why the Mbed secure-socket path is the most interesting

The key question is not just “can this board do TLS?” The key question is “can this transport do a STARTTLS-style upgrade after `AUTH TLS`, then also protect the passive data channel?”

That is why the Mbed secure-socket path is more promising than it first appears:

- `TLSSocket` is good for TLS-from-connect workflows.
- `TLSSocketWrapper` is specifically designed to wrap an already-open stream socket and then perform a TLS handshake.
- That maps much better to Explicit FTPS, where the control connection starts plaintext, receives a `234` response to `AUTH TLS`, and only then upgrades to TLS.

### Decision guidance

- The installed Arduino Opta core **does** expose Mbed `NetworkInterface`, `TCPSocket`, and `TLSSocketWrapper` cleanly enough for sketch-level use.
- The best first exploration target is therefore a direct Mbed secure-socket path using `Ethernet.getNetwork()` rather than assuming an external TLS library is required.
- If the direct Mbed wrapper path proves awkward at compile time or on-device, evaluate the bundled `EthernetSSLClient` or another TLSSocket-based wrapper before reaching for external libraries.
- If the Mbed socket path still proves impractical in real FTPS testing, evaluate `SSLClient` as the fallback prototype path.

### Required go/no-go transport spike before broad refactor

Do **not** begin the full schema/UI refactor until a small compile/device spike proves the chosen transport can actually support the Explicit FTPS flow.

Current target for that spike:

- `TLSSocketWrapper` / secure-socket path

Minimum proof required:

1. open plain TCP control socket
2. read FTP banner
3. send `AUTH TLS`
4. wrap the already-open control socket in TLS
5. complete `PBSZ 0` and `PROT P`
6. enter passive mode
7. open protected data channel (ensure TLS session resumption works)
8. complete one `STOR` and one `RETR` against the PR4100
9. read final transfer-completion reply cleanly

If this spike fails, pause the broader FTPS refactor and move to the documented fallback order rather than continuing with schema/UI work first.

### Recommended include and entry-point shape for this codebase

The installed core already supports the following pattern:

```cpp
#include <PortentaEthernet.h>
#include <Ethernet.h>
#include "TCPSocket.h"
#include "TLSSocketWrapper.h"

NetworkInterface *net = Ethernet.getNetwork();
```

Notes:

- `Ethernet.h` already bridges into the Mbed networking layer.
- The bundled Opta libraries themselves use direct includes such as `TLSSocket.h` and `TCPSocket.h`.
- `TLSSocketWrapper` is present in the same compile-visible netsocket include set.

### Design-only code scaffolding for each option

The following code is intentionally planning-grade scaffolding. It is **not** implemented in the firmware and is **not** guaranteed to compile unchanged in the current Arduino Opta build environment. Its purpose is to show the shape of the code required for each transport choice.

#### Shared transport abstraction used by all options

```cpp
enum class FtpSecurityMode : uint8_t {
  Plain = 0,
  ExplicitTls = 1,
  ImplicitTls = 2,
};

struct FtpTlsConfig {
  FtpSecurityMode mode = FtpSecurityMode::Plain;
  bool validateServerCert = true;
  const char *serverName = nullptr;
  const char *rootCaPem = nullptr;
  const char *pinnedFingerprint = nullptr;
};

struct FtpEndpoint {
  const char *host = nullptr;
  uint16_t port = 21;
};

class IFtpTransport {
public:
  virtual ~IFtpTransport() {}

  virtual bool connectControl(const FtpEndpoint &endpoint,
                              const FtpTlsConfig &tls,
                              char *error,
                              size_t errorSize) = 0;

  virtual bool upgradeControlToTls(const FtpTlsConfig &tls,
                                   char *error,
                                   size_t errorSize) = 0;

  virtual bool openProtectedDataChannel(const IPAddress &host,
                                        uint16_t port,
                                        const FtpTlsConfig &tls,
                                        char *error,
                                        size_t errorSize) = 0;

  virtual int ctrlWrite(const uint8_t *data, size_t len) = 0;
  virtual int ctrlRead() = 0;
  virtual int dataWrite(const uint8_t *data, size_t len) = 0;
  virtual int dataRead() = 0;
  virtual bool ctrlConnected() const = 0;
  virtual bool dataConnected() const = 0;
  virtual void closeData() = 0;
  virtual void closeAll() = 0;
};
```

#### Option A: `SSLClient` over `EthernetClient`

This is the lowest-churn Arduino-style option, but it is **not** the cleanest fit for Explicit FTPS. The main risk is whether the TLS layer can adopt an already-open plaintext socket after `AUTH TLS`.

```cpp
#include <Ethernet.h>
#include <SSLClient.h>

extern const br_x509_trust_anchor TAs[];
extern const size_t TAs_NUM;

class SslClientFtpTransport : public IFtpTransport {
public:
  SslClientFtpTransport()
    : ctrlTls_(ctrlPlain_, TAs, TAs_NUM, A0),
      dataTls_(dataPlain_, TAs, TAs_NUM, A0) {}

  bool connectControl(const FtpEndpoint &endpoint,
                      const FtpTlsConfig &tls,
                      char *error,
                      size_t errorSize) override {
    closeAll();

    if (tls.mode == FtpSecurityMode::ImplicitTls) {
      if (!ctrlTls_.connect(endpoint.host, endpoint.port)) {
        snprintf(error, errorSize, "implicit TLS control connect failed");
        return false;
      }
      return true;
    }

    if (!ctrlPlain_.connect(endpoint.host, endpoint.port)) {
      snprintf(error, errorSize, "plain control connect failed");
      return false;
    }

    return true;
  }

  bool upgradeControlToTls(const FtpTlsConfig &tls,
                           char *error,
                           size_t errorSize) override {
    if (tls.mode != FtpSecurityMode::ExplicitTls) {
      return true;
    }

    // Critical feasibility question:
    // Explicit FTPS needs the TLS layer to adopt the already-open ctrlPlain_
    // socket immediately after `AUTH TLS` succeeds. If the library cannot do
    // STARTTLS-style adoption, this transport is not viable for Explicit FTPS.
    //
    // If adoption succeeds, set the upgrade flag so I/O routes correctly:
    // ctrlUpgraded_ = true;
    snprintf(error, errorSize,
             "feasibility spike required: confirm SSLClient can wrap an existing control socket after AUTH TLS");
    return false;
  }

  bool openProtectedDataChannel(const IPAddress &host,
                                uint16_t port,
                                const FtpTlsConfig &tls,
                                char *error,
                                size_t errorSize) override {
    closeData();

    if (tls.mode == FtpSecurityMode::Plain) {
      if (!dataPlain_.connect(host, port)) {
        snprintf(error, errorSize, "plain data connect failed");
        return false;
      }
      return true;
    }

    // Same feasibility issue applies to protected data sockets if the server
    // expects a TLS-protected passive channel opened after PASV.
    // If data TLS succeeds: dataUpgraded_ = true;
    snprintf(error, errorSize,
             "feasibility spike required: confirm SSLClient FTPS data-channel behavior");
    return false;
  }

  // REVIEW NOTE (04142026): The original scaffolding used ctrlPlain_.connected()
  // as the routing predicate. That is logically inverted for Explicit FTPS:
  // after AUTH TLS, ctrlPlain_ is still connected (TLS wraps the same TCP
  // socket), so reads/writes would continue going to the plain client instead
  // of the TLS one. Use explicit upgrade flags instead.

  int ctrlWrite(const uint8_t *data, size_t len) override {
    return ctrlUpgraded_ ? ctrlTls_.write(data, len) : ctrlPlain_.write(data, len);
  }

  int ctrlRead() override {
    return ctrlUpgraded_ ? ctrlTls_.read() : ctrlPlain_.read();
  }

  int dataWrite(const uint8_t *data, size_t len) override {
    return dataUpgraded_ ? dataTls_.write(data, len) : dataPlain_.write(data, len);
  }

  int dataRead() override {
    return dataUpgraded_ ? dataTls_.read() : dataPlain_.read();
  }

  bool ctrlConnected() const override {
    return ctrlPlain_.connected() || ctrlTls_.connected();
  }

  bool dataConnected() const override {
    return dataPlain_.connected() || dataTls_.connected();
  }

  void closeData() override {
    dataTls_.stop();
    dataPlain_.stop();
    dataUpgraded_ = false;
  }

  void closeAll() override {
    closeData();
    ctrlTls_.stop();
    ctrlPlain_.stop();
    ctrlUpgraded_ = false;
  }

private:
  EthernetClient ctrlPlain_;
  EthernetClient dataPlain_;
  SSLClient ctrlTls_;
  SSLClient dataTls_;
  bool ctrlUpgraded_ = false;
  bool dataUpgraded_ = false;
};
```

Verdict for this option:

- Best if the project wants the least stylistic change from current Arduino networking.
- Most questionable for Explicit FTPS because `AUTH TLS` is an in-place upgrade, not a fresh TLS connect.
- Reasonable fallback to prototype only if the Mbed-native path is unavailable.

#### Option B: Mbed `TLSSocket`

This is the cleaner native-TLS option when the connection should be TLS from the start. It is a good fit for Implicit FTPS and for any helper that simply needs a direct TLS socket. It is a weaker fit for Explicit FTPS control-channel upgrade.

The installed Opta core also ships a higher-level Arduino wrapper for this model:

- `EthernetSSLClient` in `libraries/Ethernet/src/EthernetSSLClient.h`
- backed by `SocketWrapper/src/MbedSSLClient.h`
- which in turn uses Mbed `TLSSocket` in `SocketWrapper/src/MbedClient.cpp`

That is useful evidence that the TLSSocket path is supported in the Arduino Opta environment, but it is still a **TLS-from-connect** abstraction, not an existing-socket wrapper for `AUTH TLS`.

```cpp
#include "mbed.h"
#include "NetworkInterface.h"
#include "TLSSocket.h"

class MbedTlsSocketFtpTransport : public IFtpTransport {
public:
  bool connectControl(const FtpEndpoint &endpoint,
                      const FtpTlsConfig &tls,
                      char *error,
                      size_t errorSize) override {
    closeAll();

    net_ = Ethernet.getNetwork();
    if (!net_) {
      snprintf(error, errorSize, "no default Mbed network interface");
      return false;
    }

    if (tls.mode == FtpSecurityMode::Plain || tls.mode == FtpSecurityMode::ExplicitTls) {
      snprintf(error, errorSize,
               "TLSSocket alone is not the preferred explicit-FTPS control upgrade path");
      return false;
    }

    ctrlTls_.reset(new TLSSocket());
    if (ctrlTls_->open(net_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "TLSSocket open failed");
      return false;
    }

    if (tls.rootCaPem && ctrlTls_->set_root_ca_cert(tls.rootCaPem) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "set_root_ca_cert failed");
      return false;
    }

    if (tls.serverName) {
      ctrlTls_->set_hostname(tls.serverName);
    }

    SocketAddress addr;
    if (net_->gethostbyname(endpoint.host, &addr) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "DNS lookup failed");
      return false;
    }
    addr.set_port(endpoint.port);

    if (ctrlTls_->connect(addr) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "implicit TLS control connect failed");
      return false;
    }

    return true;
  }

  bool upgradeControlToTls(const FtpTlsConfig &tls,
                           char *error,
                           size_t errorSize) override {
    (void)tls;
    snprintf(error, errorSize,
             "use TLSSocketWrapper instead of TLSSocket for explicit AUTH TLS upgrade");
    return false;
  }

  bool openProtectedDataChannel(const IPAddress &host,
                                uint16_t port,
                                const FtpTlsConfig &tls,
                                char *error,
                                size_t errorSize) override {
    if (!net_) {
      snprintf(error, errorSize, "network interface not initialized");
      return false;
    }

    dataTls_.reset(new TLSSocket());
    if (dataTls_->open(net_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data TLSSocket open failed");
      return false;
    }

    if (tls.rootCaPem && dataTls_->set_root_ca_cert(tls.rootCaPem) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data set_root_ca_cert failed");
      return false;
    }

    if (tls.serverName) {
      dataTls_->set_hostname(tls.serverName);
    }

    SocketAddress addr;
    addr.set_ip_address(host.toString().c_str());
    addr.set_port(port);

    if (dataTls_->connect(addr) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "protected data connect failed");
      return false;
    }

    return true;
  }

  int ctrlWrite(const uint8_t *data, size_t len) override {
    return ctrlTls_ ? ctrlTls_->send(data, len) : NSAPI_ERROR_NO_SOCKET;
  }

  int ctrlRead() override {
    uint8_t ch = 0;
    return (ctrlTls_ && ctrlTls_->recv(&ch, 1) == 1) ? ch : -1;
  }

  int dataWrite(const uint8_t *data, size_t len) override {
    return dataTls_ ? dataTls_->send(data, len) : NSAPI_ERROR_NO_SOCKET;
  }

  int dataRead() override {
    uint8_t ch = 0;
    return (dataTls_ && dataTls_->recv(&ch, 1) == 1) ? ch : -1;
  }

  bool ctrlConnected() const override { return ctrlTls_ != nullptr; }
  bool dataConnected() const override { return dataTls_ != nullptr; }
  void closeData() override { if (dataTls_) { dataTls_->close(); dataTls_.reset(); } }
  void closeAll() override {
    closeData();
    if (ctrlTls_) { ctrlTls_->close(); ctrlTls_.reset(); }
  }

private:
  NetworkInterface *net_ = nullptr;
  std::unique_ptr<TLSSocket> ctrlTls_;
  std::unique_ptr<TLSSocket> dataTls_;
};
```

Verdict for this option:

- Strong if the first FTPS release were Implicit TLS only.
- Less compelling for the currently recommended Explicit TLS-first plan.
- Still useful as part of a Mbed-native exploration, especially for protected data sockets.

#### Option C: Mbed `TLSSocketWrapper` / secure-socket path

This is the most promising path for Explicit FTPS because it can wrap an existing `TCPSocket` after `AUTH TLS`, which matches the FTPS control-flow model directly.

```cpp
#include "mbed.h"
#include "NetworkInterface.h"
#include "TCPSocket.h"
#include "TLSSocketWrapper.h"

class MbedSecureSocketFtpTransport : public IFtpTransport {
public:
  bool connectControl(const FtpEndpoint &endpoint,
                      const FtpTlsConfig &tls,
                      char *error,
                      size_t errorSize) override {
    closeAll();

    net_ = Ethernet.getNetwork();
    if (!net_) {
      snprintf(error, errorSize, "no default Mbed network interface");
      return false;
    }

    if (ctrlPlain_.open(net_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "control socket open failed");
      return false;
    }

    if (!resolveEndpoint(endpoint, ctrlAddr_, error, errorSize)) {
      return false;
    }

    if (ctrlPlain_.connect(ctrlAddr_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "control TCP connect failed");
      return false;
    }

    ctrlOpen_ = true;

    if (tls.mode == FtpSecurityMode::ImplicitTls) {
      return wrapControlSocket(ctrlPlain_, ctrlAddr_, tls, error, errorSize);
    }

    // Plain or Explicit TLS pre-AUTH: control is ready at TCP level
    ctrlReady_ = true;
    return true;
  }

  bool upgradeControlToTls(const FtpTlsConfig &tls,
                           char *error,
                           size_t errorSize) override {
    if (tls.mode != FtpSecurityMode::ExplicitTls) {
      return true;
    }

    return wrapControlSocket(ctrlPlain_, ctrlAddr_, tls, error, errorSize);
  }

  bool openProtectedDataChannel(const IPAddress &host,
                                uint16_t port,
                                const FtpTlsConfig &tls,
                                char *error,
                                size_t errorSize) override {
    if (!net_) {
      snprintf(error, errorSize, "network interface not initialized");
      return false;
    }

    closeData();

    dataPlain_.reset(new TCPSocket());
    if (dataPlain_->open(net_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data socket open failed");
      return false;
    }

    dataAddr_.set_ip_address(host.toString().c_str());
    dataAddr_.set_port(port);

    if (dataPlain_->connect(dataAddr_) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data TCP connect failed");
      return false;
    }

    if (tls.mode == FtpSecurityMode::Plain) {
      return true;
    }

    // REVIEW NOTE (04142026): Same TRANSPORT_CLOSE double-close risk as the
    // control channel. Use TRANSPORT_KEEP for consistent ownership.
    dataTls_.reset(new TLSSocketWrapper(dataPlain_.get(),
                                        tls.serverName,
                      TLSSocketWrapper::TRANSPORT_KEEP));

    if (tls.rootCaPem && dataTls_->set_root_ca_cert(tls.rootCaPem) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data set_root_ca_cert failed");
      return false;
    }

    if (dataTls_->connect() != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "data TLS handshake failed");
      return false;
    }

    return true;
  }

  int ctrlWrite(const uint8_t *data, size_t len) override {
    if (ctrlTls_) {
      return ctrlTls_->send(data, len);
    }
    return ctrlPlain_.send(data, len);
  }

  int ctrlRead() override {
    uint8_t ch = 0;
    nsapi_size_or_error_t rc = ctrlTls_ ? ctrlTls_->recv(&ch, 1) : ctrlPlain_.recv(&ch, 1);
    return rc == 1 ? ch : -1;
  }

  int dataWrite(const uint8_t *data, size_t len) override {
    if (dataTls_) {
      return dataTls_->send(data, len);
    }
    return dataPlain_ ? dataPlain_->send(data, len) : NSAPI_ERROR_NO_SOCKET;
  }

  int dataRead() override {
    uint8_t ch = 0;
    nsapi_size_or_error_t rc = dataTls_ ? dataTls_->recv(&ch, 1)
                                        : (dataPlain_ ? dataPlain_->recv(&ch, 1) : NSAPI_ERROR_NO_SOCKET);
    return rc == 1 ? ch : -1;
  }

  // REVIEW NOTE (04142026): ctrlConnected() was originally hardcoded to true.
  // Now tracks actual state: ctrlReady_ is set true only after a successful
  // TLS handshake (or after plain TCP connect when no TLS is used), and is
  // cleared on every failure and close path. Wrapper existence alone
  // (ctrlTls_ != nullptr) is not sufficient because a failed handshake can
  // leave a non-null wrapper behind.
  bool ctrlConnected() const override {
    return ctrlReady_;
  }
  // REVIEW NOTE (04142026): dataConnected() checks pointer existence as an
  // approximation. A more robust implementation would use an explicit
  // dataReady_ flag set only after successful connect/handshake.
  bool dataConnected() const override { return dataPlain_ != nullptr || dataTls_ != nullptr; }

  void closeData() override {
    if (dataTls_) {
      dataTls_->close();
      dataTls_.reset();
    }
    if (dataPlain_) {
      dataPlain_->close();
      dataPlain_.reset();
    }
  }

  void closeAll() override {
    closeData();
    if (ctrlTls_) {
      ctrlTls_->close();
      ctrlTls_.reset();
    }
    ctrlPlain_.close();
    ctrlOpen_ = false;
    ctrlReady_ = false;
  }

private:
  bool resolveEndpoint(const FtpEndpoint &endpoint,
                       SocketAddress &addr,
                       char *error,
                       size_t errorSize) {
    if (net_->gethostbyname(endpoint.host, &addr) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "DNS lookup failed");
      return false;
    }
    addr.set_port(endpoint.port);
    return true;
  }

  bool wrapControlSocket(TCPSocket &plainSocket,
                         const SocketAddress &addr,
                         const FtpTlsConfig &tls,
                         char *error,
                         size_t errorSize) {
    // REVIEW NOTE (04142026): Using TRANSPORT_CLOSE here means the
    // TLSSocketWrapper takes ownership of the underlying TCPSocket and will
    // close it when the wrapper is destroyed. This creates a double-close
    // risk because closeAll() also explicitly calls ctrlPlain_.close().
    //
    // Two options to fix:
    //   (a) Use TRANSPORT_KEEP and manage TCPSocket lifetime manually.
    //   (b) Keep TRANSPORT_CLOSE but remove the explicit ctrlPlain_.close()
    //       from closeAll() when ctrlTls_ is active.
    //
    // Option (a) is safer — it keeps ownership explicit.
    ctrlTls_.reset(new TLSSocketWrapper(&plainSocket,
                                        tls.serverName ? tls.serverName : addr.get_ip_address(),
                                        TLSSocketWrapper::TRANSPORT_KEEP));

    if (tls.rootCaPem && ctrlTls_->set_root_ca_cert(tls.rootCaPem) != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "control set_root_ca_cert failed");
      return false;
    }

    if (ctrlTls_->connect() != NSAPI_ERROR_OK) {
      snprintf(error, errorSize, "control TLS handshake failed");
      ctrlReady_ = false;
      return false;
    }

    ctrlReady_ = true;
    return true;
  }

  NetworkInterface *net_ = nullptr;
  bool ctrlOpen_ = false;   // TCP connected
  bool ctrlReady_ = false;  // fully ready (TCP + TLS handshake if applicable)
  TCPSocket ctrlPlain_;
  SocketAddress ctrlAddr_;
  std::unique_ptr<TLSSocketWrapper> ctrlTls_;
  std::unique_ptr<TCPSocket> dataPlain_;
  SocketAddress dataAddr_;
  std::unique_ptr<TLSSocketWrapper> dataTls_;
};
```

Verdict for this option:

- Best alignment with the currently recommended Explicit TLS-first FTPS plan.
- Strongest architectural match to the existing `AUTH TLS` control flow.
- Best candidate to explore first if the Arduino Opta Mbed core exposes the necessary classes cleanly.

#### Option D: custom TLS wrapper around Mbed TLS or BearSSL

This path is included for completeness. It is not the preferred starting point, but it is the escape hatch if the higher-level TLS clients do not give enough control.

```cpp
class CustomTlsSocket {
public:
  bool attachPlainSocket(void *nativeSocket,
                         const FtpTlsConfig &tls,
                         char *error,
                         size_t errorSize) {
    // TODO during a real implementation:
    // - initialize TLS context
    // - bind TLS BIO callbacks to the native socket send/recv functions
    // - load CA or pinned cert material
    // - configure hostname / SNI
    // - run handshake
    // - expose read/write/close methods
    snprintf(error, errorSize, "custom TLS adapter not implemented");
    return false;
  }

  int send(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return -1;
  }

  int recv(uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return -1;
  }

  void close() {}
};

class CustomTlsFtpTransport : public IFtpTransport {
public:
  bool connectControl(const FtpEndpoint &endpoint,
                      const FtpTlsConfig &tls,
                      char *error,
                      size_t errorSize) override {
    (void)endpoint;
    (void)tls;
    snprintf(error, errorSize,
             "custom TLS transport reserved as a last-resort option");
    return false;
  }

  bool upgradeControlToTls(const FtpTlsConfig &tls,
                           char *error,
                           size_t errorSize) override {
    (void)tls;
    snprintf(error, errorSize, "custom control upgrade not implemented");
    return false;
  }

  bool openProtectedDataChannel(const IPAddress &host,
                                uint16_t port,
                                const FtpTlsConfig &tls,
                                char *error,
                                size_t errorSize) override {
    (void)host;
    (void)port;
    (void)tls;
    snprintf(error, errorSize, "custom data channel not implemented");
    return false;
  }

  int ctrlWrite(const uint8_t *data, size_t len) override { return ctrlTls_.send(data, len); }
  int ctrlRead() override {
    uint8_t ch = 0;
    return ctrlTls_.recv(&ch, 1) == 1 ? ch : -1;
  }
  int dataWrite(const uint8_t *data, size_t len) override { return dataTls_.send(data, len); }
  int dataRead() override {
    uint8_t ch = 0;
    return dataTls_.recv(&ch, 1) == 1 ? ch : -1;
  }
  bool ctrlConnected() const override { return false; }
  bool dataConnected() const override { return false; }
  void closeData() override { dataTls_.close(); }
  void closeAll() override { ctrlTls_.close(); dataTls_.close(); }

private:
  CustomTlsSocket ctrlTls_;
  CustomTlsSocket dataTls_;
};
```

Verdict for this option:

- Maximum flexibility.
- Maximum engineering cost.
- Only justified if the higher-level TLS stacks cannot support stable FTPS behavior on Opta.

### Sample FTPS control flow using the transport abstraction

Regardless of transport choice, the FTP control logic would converge toward something like this:

```cpp
static bool ftpsConnectAndLogin(FtpsClient &client,
                                const FtpsServerConfig &config,
                                char *error, size_t errorSize) {
  FtpEndpoint endpoint = { config.host, config.port };
  FtpTlsConfig tls;
  tls.mode = FtpSecurityMode::ExplicitTls;
  tls.validateServerCert = config.validateServerCert;
  tls.serverName = config.tlsServerName;
  tls.pinnedFingerprint = config.fingerprint;
  tls.rootCaPem = config.rootCaPem;

  if (!client.transport->connectControl(endpoint, tls, error, errorSize)) {
    return false;
  }

  if (!ftpReadResponse(*client.transport, 220, error, errorSize)) {
    return false;
  }

  if (tls.mode == FtpSecurityMode::ExplicitTls) {
    if (!ftpSendCommand(*client.transport, "AUTH TLS", 234, error, errorSize)) {
      return false;
    }
    if (!client.transport->upgradeControlToTls(tls, error, errorSize)) {
      return false;
    }
    if (!ftpSendCommand(*client.transport, "PBSZ 0", 200, error, errorSize)) {
      return false;
    }
    if (!ftpSendCommand(*client.transport, "PROT P", 200, error, errorSize)) {
      return false;
    }
  }

  // USER handling must support multiple response codes:
  // - 331: server needs a password → send PASS
  // - 230: already logged in
  // - 232: authorized via security data exchange (TLS client cert)
  int userCode = 0;
  if (!ftpSendCommandEx(*client.transport, "USER", config.user,
                        userCode, error, errorSize)) {
    return false;
  }
  if (userCode == 331) {
    if (!ftpSendFormatted(*client.transport, 230, error, errorSize,
                          "PASS %s", config.password)) {
      return false;
    }
  } else if (userCode != 230 && userCode != 232) {
    snprintf(error, errorSize, "USER rejected (%d)", userCode);
    return false;
  }

  if (!ftpSendCommand(*client.transport, "TYPE I", 200, error, errorSize)) {
    return false;
  }

  return true;
}
```

This illustrates the real architectural goal: choose one transport strategy, hide it behind a narrow interface, and keep the connect/login flow focused on FTP semantics instead of TLS-library details.

### Required follow-up

- Add compile guards so FTPS can be disabled cleanly if the TLS library is unavailable.
- Increase timeouts, because TLS handshake and certificate verification will take longer than plain FTP.

---

## 2. Library config types

### Required types

The library must define at least the following public types:

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

Optional debug flag (not part of the public API):

```cpp
bool allowInsecureTls;           // debug-only bring-up switch; default false
```

### Trust-mode enum

```cpp
enum class FtpsTrustMode : uint8_t {
  Fingerprint = 0,
  ImportedCert = 1,
};
```

Normalization rules:

- missing or zero-initialized trust mode defaults to `Fingerprint`
- any value greater than `ImportedCert` should be clamped to `Fingerprint`
- invalid trust-mode values should be rejected rather than silently preserved

### Imported certificate path conventions

If a host application persists imported trust certificates on the filesystem, it should use canonical internal paths:

```cpp
static constexpr const char *FTP_TLS_DIR = "/ftps";
static constexpr const char *FTP_TLS_TRUST_CERT_PATH = "/ftps/server_trust.pem";
static constexpr const char *FTP_TLS_TRUST_CERT_TMP_PATH = "/ftps/server_trust.pem.tmp";
```

Path rules:

- empty cert path means no imported trust certificate is present
- do not allow arbitrary filesystem paths
- import and replace operations should write to a temp path first and then rename atomically

### Recommended validation defaults

- `validateServerCert` should default to `true`
- `allowInsecureTls` should default to `false` and remain debug-only

Production FTPS should fail closed when validation is enabled and trust requirements are not met.

### Fingerprint semantics

V1 fingerprint mode pins the **SHA-256 fingerprint of the presented leaf certificate**.

Canonical stored format:

- 64 uppercase hex characters
- no separators

Accepted input formats:

- uppercase or lowercase hex
- optional `:` separators
- optional spaces or `-` separators

Normalization rules:

- strip spaces, `:`, and `-`
- uppercase `a-f`
- require exactly 64 hex characters after normalization
- reject malformed or empty fingerprints

Comparison rules:

- compare the normalized stored fingerprint to the presented **control-channel** leaf certificate fingerprint
- if the passive data channel presents a different certificate than the control channel, fail the transfer with a certificate-mismatch error

### Clock and hostname rules

The two trust modes should not behave identically.

For `Fingerprint`:

- a trusted current time is **not** required
- `tlsServerName` is optional when connecting by IP address
- if `tlsServerName` is provided, use it for SNI when supported by the chosen TLS path

For `ImportedCert`:

- use normal certificate validation semantics
- require a trusted current time so certificate date validity can be enforced
- if `host` is a hostname and `tlsServerName` is empty, default `tlsServerName` to `host`
- if `host` is an IP address and the certificate identifies a hostname rather than that IP, require `tlsServerName`
- if the certificate SAN already includes the IP address, `tlsServerName` may remain empty
- if no valid clock is available, fail closed with a certificate-time-specific error rather than silently weakening validation

If the chosen TLS path cannot support fingerprint mode without an already-valid clock, treat that as a transport-selection problem to catch in the Phase 0 spike, not as a reason to weaken the fingerprint design.

### Why these fields are needed

- `trustMode` is required so the application can choose between fingerprint pinning and imported PEM certificate trust.
- `validateServerCert` is required so FTPS is not just encryption-without-authentication.
- `tlsServerName` is needed when connecting by IP address but the certificate is issued for a hostname.
- `rootCaPem` is needed so imported trust material can be provided directly in memory.
- `fingerprint` is the preferred small-footprint trust model for a self-signed NAS certificate.

### Host application persistence guidance

The library itself does not persist configuration. Host applications that persist FTPS settings should:

- Store trust-mode metadata alongside other FTP connection settings.
- Store imported trust certificates at a canonical path if using filesystem storage.
- Not store PEM certificates inline in main config JSON.
- Expose only `cert present` / `replace` / `clear` state to the user rather than raw filesystem paths.

---

## 3. Trust model implementation

### Recommended v1 trust options

The library should support **both** of the following trust models:

1. **SHA-256 fingerprint pinning** — preferred default for self-signed NAS certificates
2. **Imported PEM certificate trust** — fallback for environments where fingerprint verification is awkward

### Why fingerprint pinning is a good first step

- Many NAS and embedded deployments use self-signed certificates.
- Shipping CA bundles on an embedded device adds complexity.
- Fingerprint pinning is small, predictable, and easy to configure.

### Imported PEM certificate trust

This should also be supported because:

- The Mbed TLS socket APIs provide a direct `set_root_ca_cert(...)` path.
- Importing the server's self-signed PEM certificate may be simpler than custom fingerprint verification in the first release.
- It provides authenticated TLS rather than encryption-without-authentication.

### Recommended behavior

- Default workflow: fingerprint pinning using `FtpsTrustMode::Fingerprint`
- Supported fallback: provide PEM certificate data via `rootCaPem` and use `FtpsTrustMode::ImportedCert`
- If the server certificate changes, transfers will fail until the fingerprint or imported trust certificate is updated

### Certificate import limits for v1

- Accept **one** PEM certificate block only
- Reject multiple concatenated `BEGIN CERTIFICATE` blocks in v1
- Reject binary DER input in v1
- Normalize CRLF to LF before storage
- Trim leading and trailing whitespace before validation
- Reject normalized certificate payloads larger than `4096` bytes

### Recommended error categories

Use stable FTPS-specific error categories:

- `FtpsError::AuthTlsRejected`
- `FtpsError::ControlTlsHandshakeFailed`
- `FtpsError::CertValidationFailed`
- `FtpsError::DataTlsHandshakeFailed`
- `FtpsError::SessionReuseRequired`
- `FtpsError::LoginRejected`
- `FtpsError::TransferFailed`
- `FtpsError::FinalReplyFailed`

Avoid collapsing these into a generic `FTP failed` message when a specific category is available.

---

## 4. Error reporting

### Required error surface

The library must provide detailed failure information for:

- TLS handshake failed
- certificate mismatch
- `AUTH TLS` unsupported/rejected
- `PBSZ 0` failed
- `PROT P` failed
- passive data TLS connection failed
- transfer completed on data channel but final control reply failed

### Recommended diagnostics

Include the security mode in serial diagnostics:

- `ftp`
- `ftps-explicit`

This makes field debugging much easier for host applications.

The transport boundary should also expose:

- `getPeerCertFingerprint()` — for diagnostics and assisted trust enrollment
- `getLastTlsError()` — for debugging TLS-specific failures

---

## 5. Refactor the low-level FTP transport flow

This is the core of the implementation.

### Current plain FTP sequence

Today the code does roughly this:

1. TCP connect
2. read welcome banner
3. `USER`
4. `PASS`
5. `TYPE I`
6. `PASV`
7. open plain data socket
8. `STOR` or `RETR`

### Required FTPS sequence for Explicit TLS

Recommended sequence:

1. TCP connect to FTP control port
2. read welcome banner
3. send `AUTH TLS`
4. upgrade control connection to TLS
5. verify certificate
6. send `PBSZ 0`
7. send `PROT P`
8. send `USER`
9. send `PASS`
10. send `TYPE I`
11. send `PASV`
12. open **TLS data connection** to the returned passive host/port
13. perform `STOR` or `RETR`
14. close data channel
15. read transfer completion reply
16. `QUIT`

### Reference only: Implicit TLS sequence

This sequence is retained only as background if project scope changes later:

1. connect TLS immediately on the control port
2. read welcome banner after TLS is established
3. verify certificate
4. send `PBSZ 0`
5. send `PROT P`
6. continue with `USER`, `PASS`, `TYPE I`, `PASV`, secure data channel, `STOR`/`RETR`

### Concrete library methods

#### `FtpsClient`

The main client class. Wraps the transport and exposes high-level operations:

- `connect(config)` — connect and login with the given `FtpsServerConfig`
- `store(remotePath, data, length)` — upload data to a remote file
- `retrieve(remotePath, buffer, bufferSize, &bytesRead)` — download a remote file into a buffer
- `quit()` — clean disconnect

#### Transport abstraction

Must operate on an abstracted socket interface rather than assuming a plain Ethernet socket.

#### `connect()`

Must branch on security mode:

- plain FTP: TCP connect, banner, USER/PASS
- explicit TLS: TCP connect, banner, `AUTH TLS`, TLS handshake, `PBSZ 0`, `PROT P`, then USER/PASS

Implicit TLS is out of scope for v1 and should not add code branches to the first release.

Must produce specific error codes for:

- TLS handshake failure
- certificate validation failure
- `AUTH TLS` rejection
- `PBSZ` rejection
- `PROT P` rejection

#### `enterPassive()`

The passive address/port parsing can remain mostly the same, but the returned host/port must be used to open a **secure** data channel whenever FTPS is selected.

#### `store()` and `retrieve()`

These are the most important operations after `connect()`.

Required FTPS behavior:

- open a TLS-wrapped data socket when explicit TLS is active
- ensure data protection is `PROT P`, not `PROT C`
- increase timeout and error reporting around handshake and transfer completion

#### `quit()`

Must close the TLS-wrapped control channel cleanly, then close the underlying socket.

---

## 6. Validation and regression testing

After the transport layer changes, all library operations must be validated:

### Required test targets

- `store()` — upload a file, verify it appears on the server
- `retrieve()` — download a file, verify contents match
- `connect()` then `quit()` — clean session lifecycle
- Repeated `store()`/`retrieve()` cycles — memory stability
- Large file transfers — verify no truncation or timeout
- Invalid credentials — verify clean error
- Invalid fingerprint — verify clean rejection
- Expired/wrong certificate — verify clean rejection

### Reference server interoperability

Test against all three v1 reference servers:

1. **WD My Cloud PR4100** — NAS with built-in FTPS
2. **vsftpd** — Linux, widely deployed
3. **FileZilla Server** — cross-platform, GUI-managed

For each server, confirm:

- `AUTH TLS` is accepted
- passive data channels work with `PROT P`
- TLS session reuse behavior (if the server requires it)
- certificate presentation matches expectations

---

## 7. Documentation

Update at least:

- `README.md` — usage instructions, trust model overview, examples reference
- `CHANGELOG.md` — version history entries
- Any existing comments that describe secure transport as a future enhancement

---

## Pre-Implementation Design Decisions (April 15 2026)

The following decisions were made during the pre-implementation review to close gaps between documentation and code scaffolding. They are recorded here as the authoritative reference.

### 1. Method names: `store()` / `retrieve()`

The public API uses FTP-protocol-native names (`store`, `retrieve`) rather than Arduino-flavored names (`uploadBuffer`, `downloadBuffer`). All headers, docs, and examples have been aligned.

### 2. `begin()` initializes the transport

`FtpsClient::begin(NetworkInterface *network, ...)` must be called once before `connect()`. It creates the internal `MbedSecureSocketFtpsTransport` using the caller's `NetworkInterface*`. This follows the standard Arduino `begin()` pattern.

### 3. Transport ownership: `FtpsClient` owns it internally

`FtpsClient` creates and destroys its `IFtpsTransport` instance. There is no dependency injection for v1 — the Mbed transport is the only supported path. The transport interface (`IFtpsTransport`) exists so unit tests can mock it and so alternative transports can be added later without changing the public API.

### 4. Public config fixed to Explicit FTPS for v1

`FtpsServerConfig` no longer exposes `securityMode` or `passiveMode` in the public API. While v1 remains Explicit FTPS plus protected passive transfers only, those behaviors are fixed rather than configurable.

The transport layer keeps `FtpTlsSecurityMode` internally in `IFtpsTransport.h` so backend work can still model future modes without overpromising unsupported public options.

### 5. Error return pattern: `bool` + `char*` + `lastError()`

The primary return is `bool` (success/fail) with a human-readable message in the `char*` buffer. For programmatic error handling, `FtpsClient::lastError()` returns the `FtpsError` enum from the most recent failed operation. The first implementation pass now includes specific FTPS failure categories such as `NetworkNotInitialized`, `BannerReadFailed`, `PassiveModeRejected`, `DataConnectionFailed`, and `ConnectionFailed`.

### 6. Trust helpers declared in `FtpsTrust.h`

`FtpsTrust.h` now declares:

- `ftpsTrustNormalizeFingerprint()` — strip separators, uppercase, validate length
- `ftpsTrustFingerprintsMatch()` — constant-time comparison
- `ftpsTrustValidatePem()` — structural validation of PEM blocks

These are utility functions, not class methods. Implementation is pending Phase 2.

### 7. Naming conventions verified against Arduino FTP ecosystem (April 15 2026)

A collision scan was performed against four major Arduino FTP libraries: **ESP32_FTPClient** (ldab), **FTPClientServer** (dplasa), **FTPClient_Generic** (khoih-prog), and **ESP-FTP-Server-Lib** (peterus). **No collisions were found.** The `Ftps` prefix on all public symbols, combined with `enum class` instead of `#define` macros, cleanly separates our library from every existing one.

Full analysis and naming rules are documented in `CODE REVIEW/NAMING_CONVENTIONS_04152026.md`.

The duplicate public/internal security-mode enum issue is now avoided by keeping `FtpTlsSecurityMode` transport-internal until non-v1 modes are actually implemented.

---

## Server-Side Assumptions

The library assumes the following about the target FTPS server unless testing disproves them:

- Explicit TLS is enabled on the server's FTP control port
- passive mode is available and allowed
- the server supports standard `AUTH TLS`, `PBSZ 0`, and `PROT P`
- the server does **not** require client certificates or mTLS
- the server presents one stable certificate suitable for fingerprint pinning or PEM import
- the chosen Opta TLS path can negotiate a compatible TLS version/cipher set with the server
- if the server certificate is hostname-bound and the client connects by IP, the application can supply `tlsServerName`
- if the server requires data-channel TLS session reuse, that behavior will be detected during the transport spike

These are planning assumptions, not verified facts. The transport spike and server interoperability testing are where they must be confirmed.

---

## Pros and Cons

## Pros

- Encrypts FTP credentials and file data in transit.
- Compatible with common NAS FTPS feature sets.
- Keeps existing FTP-based workflows intact; no new gateway is required.
- Passive-mode remote file storage continues to work without redesigning storage architecture.

## Cons

- The implementation cost is higher than plain FTP.
- A TLS-capable Ethernet client for Opta must be selected, integrated, and validated.
- TLS handshakes will increase RAM use, flash use, latency, and timeout pressure.
- Self-signed server certificates introduce certificate-management overhead.
- Some FTPS servers are picky about data-channel protection and TLS session reuse; interoperability must be tested, not assumed.

---

## Risk Ranking

The major FTPS risks are not all equal. The current ranking is:

1. **Highest risk:** Explicit FTPS transport behavior on-device
  - `AUTH TLS` control-channel upgrade must work reliably with the chosen transport.
  - Protected passive data channels must work for both `STOR` and `RETR`.
  - If this cannot be made stable on the Opta, the FTPS library cannot ship.

2. **High risk:** TLS session reuse on the passive data channel *(added 04142026)*
  - Many FTPS servers require the data channel to present the same TLS session ID/ticket as the control channel.
  - This is one of the most common real-world FTPS interoperability failures.
  - If the target server requires session reuse and the chosen Mbed TLS transport does not support it, every `STOR` and `RETR` will fail even though the control channel works.
  - This must be an explicit test in the Phase 0 spike, not a deferred discovery.

3. **High risk:** Server interoperability details
  - `AUTH TLS`, `PBSZ 0`, `PROT P`, passive-mode behavior, and transfer-completion replies must all behave as expected against the actual target server.
  - Server quirks are a bigger risk than generic FTPS theory.

4. **High risk:** Resource pressure under repeated TLS transfers
  - TLS handshakes will increase RAM use, latency, timeout pressure, and watchdog sensitivity.
  - Repeated transfers are the most likely place for this to show up.

5. **Medium risk, not a likely fatal blocker:** Certificate trust handling
  - Fingerprint pinning is planned and still preferred.
  - Imported PEM certificate trust is an allowed v1 trust model and reduces the chance that certificate handling alone blocks the release.
  - Certificate handling becomes fatal only if neither fingerprint pinning nor imported-cert trust can be made reliable in the chosen TLS path.

---

## Plain FTP vs Explicit FTPS vs Implicit FTPS

| Mode | Pros | Cons | Recommendation |
|------|------|------|----------------|
| Plain FTP | No code change, simplest runtime behavior | Credentials and data are plaintext on the wire | Legacy only |
| Explicit FTPS | Standard, works on port 21, closest to current flow | Requires control-channel upgrade and secure data-channel handling | **Recommended** |
| Implicit FTPS | Supported by some NAS devices, always-encrypted start | Older mode, extra test matrix, different default port | Out of current plan |

---

## Suggested Implementation Phasing

## Phase 0 — Transport and policy gate

- Run the `TLSSocketWrapper` compile/device spike first.
- Prove `AUTH TLS` -> TLS control upgrade -> `PBSZ 0` -> `PROT P` -> protected passive `STOR`/`RETR`.
- **Test TLS session reuse on the data channel** *(added 04142026)*: verify whether the target server requires the passive data connection to reuse the control channel's TLS session. If it does, confirm that the chosen transport can propagate the session.
- Lock validation defaults in code:
  - `validateServerCert = true`
  - `allowInsecureTls = false`
- Lock fingerprint normalization and comparison rules.
- Lock imported-cert parsing limits and atomic-replace behavior.
- Lock clock/hostname behavior for fingerprint and imported-cert modes.
- Record actual server interoperability results and any compatibility constraints.

## Phase 1 — Library types and API surface

- Define `FtpsServerConfig`, `FtpsTrustMode`, `FtpsError` types.
- Implement `FtpsClient` public API.
- Implement transport abstraction interface.

## Phase 2 — FTPS transport integration

- Add the TLS client wrapper behind the transport abstraction.
- Implement explicit TLS only.
- Wire control-channel and passive data-channel protection into the existing FTP helpers.

## Phase 3 — Full regression across all FTP consumers

- Manual backup/restore
- client config manifest flows
- month archive upload/download
- archived client export
- browser archive download endpoint

## Phase 4 — Optional hardening

- revisit implicit TLS only if a later compatibility requirement emerges
- stronger certificate validation model
- dedicated “Test FTPS Connection” button/API
- clearer UI warnings when certificate trust is missing

---

## Testing Checklist

## Functional

- Explicit TLS upload succeeds to target server.
- Explicit TLS download succeeds from target server.
- Passive-mode data channel works for both `STOR` and `RETR`.

## Negative cases

- Wrong port
- Wrong username/password
- Wrong fingerprint
- Certificate changed on server
- FTPS selected with missing trust data when validation is enabled

## Resource/stability

- Watchdog remains healthy during repeated handshake + transfer cycles.
- Memory usage remains stable across multiple transfers.
- Timeouts are long enough for the target server but still fail cleanly.

---

## Bottom-Line Recommendation

Implement **Explicit TLS FTPS** only for the current plan, and do it behind a small transport abstraction rather than patching TLS logic directly into every FTP helper.

Explore the transport options in this order:

1. Mbed `TLSSocketWrapper` / secure-socket path
2. Mbed `TLSSocket` if the first option is unavailable or if a TLS-from-connect fallback prototype is needed
3. `SSLClient` as the fallback Arduino-style prototype path
4. Custom TLS wrapper only if the higher-level options prove unworkable

That gives the library the best balance of:

- server compatibility
- contained code churn
- practical security improvement
- manageable testing scope

The largest technical risk is not the FTP command sequence itself. The largest risk is selecting and validating the TLS-capable Ethernet client layer for Opta in a way that works reliably for both the control and passive data channels.

---

## Appendix: Review Findings (April 14, 2026)

The following issues were identified during a cross-reference review of this document,
the FTPS implementation checklist, the FTPS repository review doc, and the live server firmware.

### Scaffolding code bugs fixed in-place above

| # | Severity | Location | Issue | Fix applied |
|---|----------|----------|-------|-------------|
| 1 | **High** | Option A `ctrlWrite`/`ctrlRead`/`dataWrite`/`dataRead` | `connected()` routing is logically inverted for Explicit FTPS — the underlying plain socket stays connected after TLS wraps it, so all I/O would route to the wrong client | Replaced `connected()` predicate with explicit `ctrlUpgraded_`/`dataUpgraded_` flags |
| 2 | **Medium** | Option C `ctrlConnected()` | Hardcoded `return true` regardless of actual socket state | Replaced with state-tracking check and review note |
| 3 | **Medium** | Option C `wrapControlSocket()` and `openProtectedDataChannel()` | `TRANSPORT_CLOSE` causes the `TLSSocketWrapper` to close the underlying `TCPSocket`, but `closeAll()` also explicitly closes it — double-close risk | Changed to `TRANSPORT_KEEP` with review notes |
| 4 | **Low** | Sample `ftpConnectAndLogin()` | `static_cast<FtpSecurityMode>` from `uint8_t` without range check | Added review note referencing the schema clamping rules |

### Risk ranking updated above

| # | Severity | Issue |
|---|----------|-------|
| 5 | **Medium** | TLS session reuse on the passive data channel was mentioned only in passing. This is one of the most common real-world FTPS interoperability failures. Added as risk #2 in the ranking and as an explicit Phase 0 spike test requirement. |

### Cross-document inconsistencies (fixed 04142026)

| # | Severity | Issue | Where | Status |
|---|----------|-------|-------|--------|
| 6 | **Low** | Phase numbering mismatch: this doc uses 5 phases (0–4), the checklist uses 12 phases (0–11). Phase numbers are not traceable between the two docs. | `FTPS_IMPLEMENTATION_CHECKLIST_04132026.md` | **Fixed**: Phase-mapping table added to checklist. |
| 7 | **Low** | Interface renamed from `IFtpTransport` (this doc) to `IFtpsTransport` (repo review) without noting the change. | `FTPS_REPOSITORY_REVIEW_04132026.md` | **Fixed**: Repository docs and scaffold now standardize on `IFtpsTransport`; this document keeps `IFtpTransport` only in legacy option-comparison snippets. |
| 8 | **Medium** | `FtpsTrustMode` enum starts at `1` in the repo review, but `FTP_TLS_TRUST_FINGERPRINT = 0` in this doc. A zero-initialized value would mean "fingerprint" in one doc and "invalid/undefined" in the other. | `FTPS_REPOSITORY_REVIEW_04132026.md` | **Fixed**: Repo review enum changed to start at `0`. |

### Items verified correct against reference firmware

- FTP session uses `EthernetClient ctrl` as the sole transport.
- Networking includes limited to `PortentaEthernet.h` and `Ethernet.h`.
- FTP credential obfuscation at rest confirmed (XOR + hex encoding pattern).
- All FTP helper functions present with expected signatures.
- `FTP_PORT_DEFAULT` = `21`.

### Remaining observation

The `FtpSecurityMode` enum includes `ImplicitTls = 2` and Option C implements full `ImplicitTls` code branches, despite the doc repeatedly stating Implicit TLS is out of scope. This is intentional background material per the doc text, but anyone implementing from this scaffold should be aware that the out-of-scope branches are present and should not be built into the first release.

### Follow-up findings from plan-update review (April 14, 2026)

These were found while reviewing the April 14 updates to this plan.

| # | Severity | Location | Issue | Status |
|---|----------|----------|-------|--------|
| 9 | **High** | Option A (`SslClientFtpTransport`) | `ctrlUpgraded_` / `dataUpgraded_` flags were added for routing, but no code in the scaffold sets or clears them. | **Fixed 04142026**: Added commented-out state transitions in `upgradeControlToTls()`, `openProtectedDataChannel()`, and `closeAll()` reset. |
| 10 | **High** | Option C (`MbedSecureSocketFtpTransport`) | `ctrlConnected()` referenced `ctrlOpen_` but it was not declared or maintained. | **Fixed 04142026**: Added `bool ctrlOpen_` and `bool ctrlReady_` members. `ctrlOpen_` set on TCP connect, `ctrlReady_` set after TLS handshake succeeds. Both cleared in `closeAll()`. |
| 11 | **Medium** | Option C (`dataConnected()`) | Connection status inferred from object existence instead of real socket state. | **Noted 04142026**: Review note added in scaffold; explicit `dataReady_` flag recommended for real implementation. |
| 12 | **Medium** | Checklist/repo wording around passive data TLS | "TLS session resumption" used as a hard requirement; server-dependent in practice. | Wording adjustment deferred — addressed in checklist Phase 0 context. |
| 13 | **Medium** | Option C (`MbedSecureSocketFtpTransport`) | `ctrlConnected()` short-circuited to `true` whenever `ctrlTls_` wrapper existed, even on failed handshake. | **Fixed 04142026**: Replaced with `ctrlReady_` flag that is set only after successful handshake and cleared on every failure/close path. |
| 14 | **Medium** | `IFtpTransport` boundary vs diagnostics | Transport abstraction has no peer-certificate or verification-result exposure for later trust/diagnostics. | **Fixed 04152026**: Scaffold `IFtpsTransport` now includes `getPeerCertFingerprint()` and `getLastTlsError()`. Remaining work is wiring the values in concrete transport implementations. |
| 15 | **Medium** | Sample `ftpConnectAndLogin()` | Hardcoded `USER` → `331`, `PASS` → `230`. Live firmware already accepts `230`\|`331` after `USER`, and RFC 4217 allows `232`. | **Fixed 04142026**: Sample now accepts `230` and `232` after `USER` and sends `PASS` only when server replies `331`. |

### New findings from second deep review (April 14, 2026)

| # | Severity | Location | Issue | Recommendation |
|---|----------|----------|-------|----------------|
| 16 | **Medium** | `IFtpTransport` interface | The original draft interface had no way to expose the peer certificate fingerprint or TLS error details. The UI plan (Section 3) calls for fingerprint preview and trust enrollment, and the test plan requires verifying fingerprint validation failures. | **Fixed 04152026**: Optional `getPeerCertFingerprint()` and `getLastTlsError()` methods were added to scaffold `IFtpsTransport` with default no-op behavior. |
| 17 | **Medium** | Live firmware `ftpReadResponse()` signature | The live code passes `EthernetClient &` to `ftpReadResponse()`. The transport abstraction replaces `EthernetClient` with `IFtpsTransport` which exposes `ctrlRead()`/`ctrlWrite()`. The doc does not show how `ftpReadResponse()` and `ftpSendCommand()` adapt to the new interface. | Add a sample `ftpReadResponse(IFtpsTransport &transport, ...)` signature showing how the byte-level read loop would use `transport.ctrlRead()` instead of `client.read()`. This is the most surgery-heavy refactor in the plan and needs an explicit migration note. |
| 18 | **Medium** | Live firmware data-channel lifetime | In the live code, `ftpStoreBuffer()` and `ftpRetrieveBuffer()` create and destroy a local `EthernetClient dataClient` inside the function body. The transport abstraction expects data-channel lifetime to be managed by the transport object. The plan does not describe how to migrate from function-local data sockets to transport-managed data channels. | Add a migration note: each `ftpStoreBuffer()`/`ftpRetrieveBuffer()` call should (a) call `transport.openProtectedDataChannel()` where it currently calls `dataClient.connect()`, (b) use `transport.dataWrite()`/`transport.dataRead()` for I/O, and (c) call `transport.closeData()` where it currently calls `dataClient.stop()`. |
| 19 | **Low** | `FTP_TIMEOUT_MS` = 8000ms | The current timeout is tuned for plain FTP. TLS handshake on constrained hardware like Opta can take 2–5 seconds. With both control and data handshakes, the default 8-second timeout may fire during a normal connection setup. | Recommend a separate `FTP_TLS_HANDSHAKE_TIMEOUT_MS` (e.g. 15000ms) for TLS-related operations, or increase `FTP_TIMEOUT_MS` globally with a note explaining the TLS budget. |
| 20 | **Low** | Option A `closeAll()` | `closeAll()` resets `dataUpgraded_` and `ctrlUpgraded_` flags, but `closeData()` only resets `dataUpgraded_`. If `upgradeControlToTls()` fails partway, `ctrlUpgraded_` stays `false` (correct), but nothing calls `closeAll()` on the failure path—the caller would need to do it. The scaffold doesn't show a failure-path cleanup pattern. | Add a note that the caller (or a RAII guard) must call `closeAll()` on any failure after `connectControl()` succeeds. |

### Spike-plan review findings (April 14, 2026)

| # | Severity | Location | Issue | Recommendation |
|---|----------|----------|-------|----------------|
| 21 | **High** | `FTPS_SPIKE_PLAN_04142026.md` sample code | The spike sketch uses `TLSSocketWrapper::TRANSPORT_KEEP_OPEN`, but the installed Opta core header exposes `TRANSPORT_KEEP`. As written, the sample will not compile against the verified 4.5.0 header. | Use `TLSSocketWrapper::TRANSPORT_KEEP` in both the spike sketch and the implementation-plan scaffolding. |
| 22 | **High** | `FTPS_SPIKE_PLAN_04142026.md` first-run trust guidance | `VALIDATE_CERT = false` only changes logging in the sample sketch. It does not modify the Mbed TLS auth mode, so `ROOT_CA_PEM = nullptr` does not actually prove “validation disabled”. A self-signed PR4100 certificate can still cause a false-negative control-handshake failure. | Treat the first meaningful spike run as “transport plus real trust material”: provide the PR4100 PEM cert up front, or explicitly customize the TLS auth mode in a dedicated follow-up variant before using the spike to judge Option C. |
| 23 | **Medium** | `FTPS_SPIKE_PLAN_04142026.md` session-reuse branch | The public `TLSSocketWrapper` header exposes `get_ssl_config()` and `get_ssl_context()`, but no obvious public session export/import helper. If the PR4100 requires data-channel TLS session reuse, Option C is more likely to need deeper Mbed TLS integration than a simple wrapper-only fix. | Treat session reuse as a conditional blocker: if the NAS enforces it, stop calling Option C “viable” until either PR4100 policy changes or a lower-level Mbed TLS session-sharing path is proven. |
| 24 | **Medium** | `FTPS_SPIKE_PLAN_04142026.md` fallback guidance after control-handshake failure | The spike plan currently frames Option B (`TLSSocket`) / Option A (`SSLClient`) as immediate fallbacks even though both are TLS-from-connect style transports unless separately proven to adopt an already-open control socket. Reconnecting over TLS after `AUTH TLS` is not a protocol-equivalent Explicit FTPS fallback. | Describe Option B/A as separate research spikes only. The next realistic direct fallback for Explicit FTPS control upgrade is lower-level custom Mbed TLS wrapping around the existing TCP socket, or an explicit product-scope change such as allowing Implicit FTPS. |

---

## Implementation Plan Readiness Assessment

**Assessment date:** April 14, 2026
**Reviewer:** Cross-reference review against reference firmware and all three FTPS design documents.

### Overall verdict: CONDITIONALLY READY — one blocking spike required

The plan is detailed enough to implement with confidence **after** the Phase 0 TLS transport spike succeeds. The single blocking gate is:

> **Blocker:** `TLSSocketWrapper` has not been compiled or tested on the Arduino Opta device. Until a minimal `AUTH TLS` + `PBSZ 0` + `PROT P` + `PASV` data transfer succeeds against the PR4100, the preferred transport (Option C) is unproven.

### Spike-plan assessment

The spike plan is the right gate and the right next step, but it needed a few corrections before it could serve as a reliable go/no-go test.

- The sample sketch and implementation scaffolding were using `TRANSPORT_KEEP_OPEN`, while the verified Opta header exposes `TRANSPORT_KEEP`.
- The first-run guidance was assuming `VALIDATE_CERT = false` disables certificate verification, but the sample code was not actually changing Mbed TLS auth mode.
- The fallback section was overstating Option B / Option A as direct continuations of the Explicit FTPS plan when they are really separate feasibility experiments unless they can prove existing-socket adoption.

After those documentation fixes, the spike plan is valid as the Phase 0 gate.

### What is ready

| Area | Status | Notes |
|------|--------|-------|
| **Scope definition** | Ready | Explicit TLS only, plain FTP retained, Implicit TLS deferred. Clear and consistently stated across all three docs. |
| **Config schema** | Ready | Core library config types are defined in `FtpsTypes.h` with trust-mode enum ordinals and safe defaults. Host-application persistence rules are documented but intentionally left outside the v1 library implementation. |
| **Trust model** | Ready | Two v1 modes (fingerprint pinning + imported PEM cert) fully specified. Enum values, canonical paths, and validation behavior locked. |
| **Transport abstraction** | Ready | `IFtpsTransport` is defined in the repository scaffold, including diagnostic hooks (`getPeerCertFingerprint()`, `getLastTlsError()`). Candidate transport options remain ranked in priority order. |
| **FTP command flow** | Ready | Sample `ftpConnectAndLogin()` now matches live firmware patterns. `AUTH TLS` → `PBSZ 0` → `PROT P` → login sequence is correct per RFC 4217. |
| **UI plan** | Ready | Settings fields, trust enrollment workflow, and UX decisions all specified. Conditional warnings for missing trust data defined. |
| **Testing plan** | Ready | Functional, negative-case, and stability test checklists cover all FTP consumer call sites. PR4100 interop checklist is explicit. |
| **Extraction plan** | Ready | Repository review doc has a clean extraction map, staged timeline, and release gates. Cross-doc naming/enum inconsistencies resolved. |
| **Phasing** | Ready | Both the 5-phase (implementation doc) and 12-phase (checklist) plans are now cross-referenced with a mapping table. Recommended order of work is clear. |
| **Risk ranking** | Ready | 5 risks ranked with mitigations. TLS session reuse now included as risk #2. |

### What needs work before coding starts

| Item | Priority | Action required |
|------|----------|-----------------|
| **TLSSocketWrapper spike** | **Blocking** | Compile and run a minimal FTPS handshake on the Opta against the PR4100. Confirm `AUTH TLS`, `PBSZ 0`, `PROT P`, and at least one `STOR`/`RETR` over a protected passive data channel. If this fails, stop Phase 1 work and choose a separately proven fallback path rather than assuming Option B / Option A are direct drop-in continuations. |
| **TLS session reuse verification** | **High** | During the spike, verify whether the PR4100 requires TLS session reuse on the data channel. If it does, confirm `TLSSocketWrapper` supports session ticket or session ID resumption. This is the #1 interop risk. |
| **Spike trust bootstrap** | **High** | Do not rely on `VALIDATE_CERT = false` in the current sample sketch; it is not a real verification-bypass path. For the first meaningful run, provide `ROOT_CA_PEM` or build a separate variant that explicitly customizes the Mbed TLS auth mode. |
| **Conditional session-reuse blocker** | **High** | The public `TLSSocketWrapper` API does not expose an obvious session handoff helper. If the PR4100 enforces data-channel TLS session reuse, treat that as a likely blocker for Option C until a lower-level Mbed TLS sharing path or NAS policy change is proven. |
| **`ftpReadResponse` / `ftpSendCommand` migration** | **High** | The plan shows the transport interface but does not show how the existing `ftpReadResponse(EthernetClient &, ...)` adapts to `IFtpsTransport &`. Add a migration sketch (finding #17). |
| **Data-channel lifetime migration** | **High** | The live code creates/destroys data sockets inside `ftpStoreBuffer()`/`ftpRetrieveBuffer()`. The transport owns the data socket. Document the function-by-function migration (finding #18). |
| **Transport diagnostics interface** | **Medium** | Partially complete: scaffold `IFtpsTransport` already includes `getPeerCertFingerprint()` and `getLastTlsError()`. Next step is wiring real values in the concrete transport and exposing them through client-level diagnostics. |
| **Fallback path wording** | **Medium** | Keep Option B / Option A described as separate follow-up experiments, not protocol-equivalent fallback steps for Explicit FTPS control upgrade. |
| **TLS timeout budget** | **Low** | Define a separate handshake timeout or increase `FTP_TIMEOUT_MS` beyond 8000ms for TLS operations (finding #19). |
| **arduino-cli environment** | **Low** | Install `arduino-cli` with the Opta board package to enable local compile verification before device testing. |

### Confidence by phase

| Phase | Confidence | Risk |
|-------|------------|------|
| Phase 0 — Spike | Medium | Depends entirely on `TLSSocketWrapper` behavior on real hardware. Cannot be desk-checked. |
| Phase 1 — Config & UI | High | Schema, defaults, API, and UI are fully specified. Straightforward implementation work. |
| Phase 2 — Transport & TLS | Medium | Scaffolding is reviewed and corrected. Main risk is real-world interop with PR4100 cipher/cert behavior. |
| Phase 3 — Integration test | High | All FTP consumer call sites are identified and listed. Test matrix is explicit. |
| Phase 4 — Docs & cleanup | High | Minimal risk. |

### Summary of review findings

Two full review passes plus a spike-plan review have been completed across the FTPS documents and the reference firmware, and the first implementation pass has now been landed in the repository. Remaining open items are hardware-facing: session-reuse behavior validation, trust-mode validation against real servers, timeout tuning, and compile/release hardening.

No remaining finding invalidates the planned architecture. The unresolved items are concrete implementation checkpoints that should be addressed during Phase 0 and early Phase 2.

### Go/No-Go recommendation

**Go** — proceed to the Phase 0 spike, but do it with the corrected spike-plan assumptions. The plan is architecturally sound, the scaffolding code issues have been corrected, cross-document inconsistencies have been resolved, and the remaining open items are addressable during or immediately before implementation. The single hard gate is still on-device TLS transport validation, but the spike should be run with real trust material or an explicit auth-mode override, and Option B / Option A should be treated as research paths rather than automatic fallbacks for Explicit FTPS.
