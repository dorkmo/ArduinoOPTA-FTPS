# FTPSclientOPTA

FTPSclientOPTA is a general-purpose FTPS client library for Arduino Opta devices.

The goal is to provide a reusable, Opta-first FTPS library built on the board's Ethernet and Mbed networking stack, with initial validation targeted against a WD My Cloud PR4100, vsftpd, and FileZilla Server, and designed to support other standards-compliant FTPS servers over time.

> [!NOTE]
> This library has been validated on real Arduino Opta hardware with all core operations (connect, mkdir, upload, size, download, quit) passing against a pyftpdlib FTPS server. Treat it as early-release — broader server interoperability testing is still in progress.

## Project Goals

- Provide a small Arduino-style FTPS client for Arduino Opta.
- Ship Explicit FTPS (`AUTH TLS`) over Ethernet first.
- Support protected passive-mode transfers (`PBSZ 0`, `PROT P`) for upload and download.
- Keep certificate handling practical for field deployments.
- Keep the session and transport model flexible enough to add Implicit FTPS and optional active mode later.
- Stay general enough to support servers beyond the initial reference NAS.

## Planned v1 Scope

- Explicit FTPS only
- Passive mode only
- Binary transfers only
- Upload and download primitives
- Remote directory creation for nested application paths
- Remote size preflight for variable-size downloads
- SHA-256 fingerprint pinning
- Imported PEM certificate trust
- FTPS-specific error reporting suitable for field diagnostics

## Not in Scope for v1

- Implicit FTPS
- Active mode
- Recursive sync
- Delete, rename, or broader file-management helpers beyond the current integration set
- Product-specific UI, config storage, or backup policy
- Broad multi-board support claims before hardware validation

## Candidate v2 Scope

- Explicit and Implicit FTPS modes under a common client API
- Optional active-mode transfers in addition to passive mode
- Capability discovery and directory helpers such as `FEAT`, `PWD`, `MLSD`, and `NLST`
- File-management helpers such as `DELE`, `RNFR`/`RNTO`, and `RMD`
- Stream-oriented transfer APIs in addition to buffer-based helpers
- Broader certificate and trust-management options
- Expanded interoperability testing across more FTPS servers
- Possible support for additional Mbed-based Arduino targets after Opta support is stable

Active mode is worth keeping on the v2 backlog, but it should stay behind Implicit FTPS and passive-mode hardening. It adds firewall and NAT complexity, and it is less likely to matter for the initial Opta-to-NAS deployment patterns this library is targeting.

## Compatible Hardware

- Arduino Opta only for v1
- Intended minimum core: `arduino:mbed_opta` 4.5.0 or later
- Initial network path: Ethernet via `PortentaEthernet` and `Ethernet`

## Reference Servers

The v1 validation targets are:

- **WD My Cloud PR4100** — NAS with built-in FTPS support
- **vsftpd** — widely deployed Linux FTPS server
- **FileZilla Server** — cross-platform FTPS server with GUI management

These are reference servers, not a guarantee of universal FTPS compatibility. Broader compatibility claims should only be made after real interop testing.

## Expected Dependencies

When the first implementation lands, the library is expected to rely on:

- Arduino Opta board support package
- `PortentaEthernet` and `Ethernet`
- Mbed networking interfaces exposed by the Opta core, including `NetworkInterface`, `TCPSocket`, and `TLSSocketWrapper`

## Installation

FTPSclientOPTA is structured for Arduino Library Manager distribution.

Until the indexer has picked up the latest tagged release, you can also install it as a local library checkout.

## Current API Direction

The public API is intentionally small and Arduino-style. The current implementation direction looks like this:

```cpp
// Illustrative usage.

FtpsServerConfig config;
config.host = "192.168.1.100";
config.port = 21;
config.user = "user";
config.password = "pass";
config.tlsServerName = "nas.local";
config.trustMode = FtpsTrustMode::Fingerprint;
config.fingerprint = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
// Current implementation is fail-closed and requires certificate validation.
config.validateServerCert = true;

FtpsClient client;
char error[96];

if (!client.begin(Ethernet.getNetwork(), error, sizeof(error))) {
	Serial.println(error);
	return;
}

if (client.connect(config, error, sizeof(error))) {
	client.mkd("/backup", error, sizeof(error));

	const uint8_t payload[] = "{}";
	client.store(
			"/backup/config.json",
			payload,
			sizeof(payload) - 1,
			error,
			sizeof(error));

	size_t remoteBytes = 0;
	client.size(
			"/backup/config.json",
			remoteBytes,
			error,
			sizeof(error));

	uint8_t buffer[64] = {};
	size_t bytesRead = 0;
	client.retrieve(
			"/backup/config.json",
			buffer,
			sizeof(buffer),
			bytesRead,
			error,
			sizeof(error));

	client.quit();
}
```

The public API surface is `begin()`, `connect()`, `setTraceCallback()`, `lastPhase()`, `mkd()`, `size()`, `list()`, `dele()`, `rmd()`, `rename()`, `store()`, `retrieve()`, `quit()`, `lastError()`, and `lastNsapiError()`. `begin()` initializes the transport layer with the Mbed `NetworkInterface` and must be called once before `connect()`. `lastError()` returns an `FtpsError` enum for programmatic error handling alongside the human-readable `char*` error buffer. `lastNsapiError()` exposes the most recent socket-layer code (for example `-3005` when LWIP socket-pool pressure causes a transient data-channel open failure). `setTraceCallback()` registers an optional callback invoked at each protocol phase for diagnostics or watchdog integration.

`FtpsErrors.h` also provides two sketch-level classifier helpers used by multi-file retry loops:

- `ftpsIsSessionDead(err)`
- `ftpsIsTransferRetriable(err, nsapiCode)`

These helpers standardize error classification across projects while keeping retry/backoff policy in the application layer.

The v1 public config intentionally does not expose `securityMode` or `passiveMode` toggles. Until additional modes are implemented, the library surface is fixed to Explicit FTPS plus protected passive transfers so sketches cannot accidentally rely on unsupported options.

## Hardware Notes

**Static IP vs DHCP:** On some Opta boards and network configurations, `Ethernet.begin(mac)` (DHCP) can hang or take a very long time. If your sketch appears stuck at Ethernet init, use a static IP instead:

```cpp
IPAddress ip(192, 168, 1, 50);
IPAddress dns(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
Ethernet.begin(mac, ip, dns, gateway, subnet);
```

**Watchdog and long operations:** TLS handshakes on the Opta can take several seconds each. If your application uses a hardware watchdog, kick it from the trace callback to prevent resets during connect/transfer:

```cpp
#include <mbed.h>

static void ftpsTraceCallback(const char *phase) {
  Watchdog::get_instance().kick();
  Serial.print("[FTPS] ");
  Serial.println(phase);
}

ftps.setTraceCallback(ftpsTraceCallback);
```

## Near-Term Integration Requirements

For general Arduino applications that need multi-file backup, restore, manifest, or archive workflows on top of this library, the next items are:

- broader server interoperability testing (FileZilla Server, vsftpd, WD My Cloud PR4100)
- documentation for buffer-owned transfer semantics, variable-size download guidance, and the provisional one-client-at-a-time assumption
- streaming transfer APIs only if measured payload sizes exceed safe RAM ceilings for the supported Opta target

These requirements are tracked as integration follow-up work, not as product-specific behavior inside the library.

## Trust Model

- SHA-256 leaf-certificate fingerprint pinning
- Imported PEM certificate trust
- Certificate validation required (fail-closed)
- No silent fallback from FTPS to plaintext FTP

## Current and Planned Examples

The repository currently includes:

- `examples/BasicUpload/BasicUpload.ino`
- `examples/BasicDownload/BasicDownload.ino`
- `examples/RetryUpload/RetryUpload.ino`
- `examples/FileZillaLiveTest/FileZillaLiveTest.ino`
- `examples/PyftpdlibLiveTest/PyftpdlibLiveTest.ino`
- `examples/WDMyCloudLiveTest/WDMyCloudLiveTest.ino`
- `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino`
- `examples/WebFileManagerLiveTest/WebFileManagerLiveTest.ino`
- `FtpsSpikeTest/FtpsSpikeTest.ino`

The upload/download examples and the FileZilla live-test sketch emit structured serial diagnostics so Opta-side runs are easy to inspect in the serial monitor. The **PyftpdlibLiveTest** example bundles a self-contained Python FTPS server (`ftps_server.py`) and certificate generator (`gen_cert.py`) so you can test the library without installing any third-party server software — just `pip install pyftpdlib pyOpenSSL` and run the scripts. The **WDMyCloudLiveTest** example targets WD My Cloud NAS devices running My Cloud OS 5 and includes a [setup guide](examples/WDMyCloudLiveTest/README.md) for enabling FTPS on the NAS and extracting the certificate fingerprint. The web harness sketch hosts a lightweight LAN page for updating FTPS settings and running small connect/upload/download/quit tests without reflashing, uses token-gated actions behind a small passcode unlock step, and supports downloadable plain-text test reports. The web file manager sketch builds on that harness and adds authenticated `/fs` routes for remote list, mkdir, delete, move, and demo copy workflows (copy remains buffer-limited in the example). On `arduino:mbed_opta` 4.5.0, local directory listing is currently disabled in this example build because `<dirent.h>` is not available in the shipped toolchain.

Planned follow-up examples after broader transport/client validation lands:

- Trust-mode-focused fingerprint validation example
- Trust-mode-focused imported PEM validation example
- PR4100 reference setup
- vsftpd reference setup
- Additional FileZilla Server reference variants

## Current Status

The library has been validated on real Arduino Opta hardware:

- All core operations pass: connect, mkd, store, size, retrieve, quit
- SHA-256 fingerprint pinning verified on-device
- TLS session reuse hinting implemented for data-channel handshakes
- Socket lifecycle hardened: ordered tear-down (TCP close → TLS delete
  → TCP delete) prevents Mbed OS hard faults and the historical
  `delete tls` hang
- TLS close timeouts and `SO_LINGER` (where supported) prevent
  indefinite hangs during shutdown
- **Multi-file backup verified end-to-end against a pyftpdlib FTPS
  server: 8 uploads, 0 failed in a single session (2026-04-17)**
- Web file manager live validation passed on real Opta against local
  pyftpdlib FTPS (`2026-04-18`): connect, remote list, upload,
  download, copy, move, delete, auth-gate enforcement, bad-fingerprint
  rejection, bad-password rejection, and reconnect-after-server-restart
- Trace callback support for diagnostics and watchdog integration,
  including new `xport:cleanup:*`, `xport:linger-*`, and
  `xport:open-failed:*` / `xport:connect-failed:*` traces for
  fine-grained socket-lifecycle observability

Remaining work:

1. Broader server interoperability testing (FileZilla Server, vsftpd, WD My Cloud PR4100)
2. Imported PEM certificate trust validation on hardware
3. Broader example compile validation across the included sketches

## Limitations

- Arduino Opta is the only supported board for v1
- Explicit FTPS only
- Passive mode only
- Buffer-based transfers only (no streaming API yet)
- Compatibility beyond pyftpdlib is not yet validated on hardware
- Servers that enforce strict TLS session reuse may require additional transport work
- DHCP may hang on some Opta/network configurations; static IP is recommended
- `examples/WebFileManagerLiveTest` uses safer placeholder defaults and
  requires explicit runtime configuration for host credentials and
  fingerprint before live FTPS actions

### Arduino Opta networking constraints (mbed_opta 4.5.0)

The FTPS library itself is platform-neutral, but Arduino Opta integrators
should be aware of two hard limits in the LWIP build shipped inside the
precompiled `libmbed.a` archive:

- **Socket pool size is fixed at 4.** `MBED_CONF_LWIP_SOCKET_MAX` and
  `MBED_CONF_LWIP_TCP_SOCKET_MAX` are baked into the precompiled archive,
  so editing `variants/OPTA/mbed_config.h` has no effect at link time.
  An Opta application can have at most ~4 simultaneous TCP PCBs across
  the entire device.
- **`SO_LINGER` is not implemented.** `setsockopt(SO_LINGER, {l_onoff=1,
  l_linger=0})` returns `NSAPI_ERROR_UNSUPPORTED` (`-3002`). Every closed
  TCP socket therefore sits in `TIME_WAIT` for ~60 s instead of being
  hard-reset. The transport now traces `xport:linger-unsupported:-3002`
  so this is visible at runtime.

For a single FTPS transfer (control + data) these limits are not visible.
For multi-file backup workflows on Opta, the practical consequences are:

- An idle `EthernetServer` consumes 1 of the 4 PCB slots for the LISTEN
  socket. If the application also keeps an accepted browser session
  active, only 2 slots remain for FTPS — not enough for a control
  channel plus a fresh PASV data channel while the previous data
  socket is still in `TIME_WAIT`.
- Pacing matters. Opening a new PASV data socket within ~60 s of
  closing the previous one will fail with `NSAPI_ERROR_NO_SOCKET`
  (`-3005`). The transport surfaces this via
  `xport:open-failed:-3005` so integrators can distinguish pool
  exhaustion from network errors.

A recipe for working around both constraints in an application that also
hosts a web UI is documented in the integrating Tank Alarm Server
project (see
`ArduinoSMSTankAlarm/CODE REVIEW/OPTA_LWIP_BACKUP_RECIPE_04172026.md`
and `MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md`).

## Project Documentation

- [Implementation note](CODE%20REVIEW/FTPS_IMPLEMENTATION_04132026.md)
- [Implementation checklist](CODE%20REVIEW/FTPS_IMPLEMENTATION_CHECKLIST_04132026.md)
- [Application integration requirements](CODE%20REVIEW/APPLICATION_INTEGRATION_REQUIREMENTS_04152026.md)
- [Hardware and follow-up checklist](CODE%20REVIEW/HARDWARE_AND_FOLLOWUP_CHECKLIST_04152026.md)
- [Repository creation plan](CODE%20REVIEW/FTPS_REPOSITORY_REVIEW_04132026.md)
- [Phase 0 spike plan](CODE%20REVIEW/FTPS_SPIKE_PLAN_04142026.md)
- [Serial monitor output guide](CODE%20REVIEW/SERIAL_MONITOR_OUTPUT_04152026.md)
- [Web harness API reference](CODE%20REVIEW/WEB_HARNESS_API_REFERENCE_04152026.md)

## License

This repository uses the CC0 1.0 license. See [LICENSE](LICENSE).
