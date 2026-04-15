# ArduinoOPTA-FTPS

ArduinoOPTA-FTPS is an experimental repository for a general-purpose FTPS client library for Arduino Opta devices.

The goal is to provide a reusable, Opta-first FTPS library built on the board's Ethernet and Mbed networking stack, with initial validation targeted against a WD My Cloud PR4100, vsftpd, and FileZilla Server, and designed to support other standards-compliant FTPS servers over time.

> [!IMPORTANT]
> This repository now includes an initial Explicit FTPS implementation built on the Opta Mbed networking stack. Treat it as experimental until it has been validated on real Opta hardware against the target servers in your environment.

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
- SHA-256 fingerprint pinning
- Imported PEM certificate trust
- FTPS-specific error reporting suitable for field diagnostics

## Not in Scope for v1

- Implicit FTPS
- Active mode
- Recursive sync
- Delete, rename, mkdir, or broader file-management helpers
- Product-specific UI, config storage, or backup policy
- Broad multi-board support claims before hardware validation

## Candidate v2 Scope

- Explicit and Implicit FTPS modes under a common client API
- Optional active-mode transfers in addition to passive mode
- Capability discovery and directory helpers such as `FEAT`, `PWD`, `MLSD`, and `NLST`
- File-management helpers such as `DELE`, `RNFR`/`RNTO`, `MKD`, and `RMD`
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

This repository is not yet published to Arduino Library Manager.

For development, it can be used as a local library checkout. The library now contains a first-pass FTPS implementation, but it still needs live Opta validation and interoperability testing before it should be treated as release-ready.

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
	const uint8_t payload[] = "{}";
	client.store(
			"/backup/config.json",
			payload,
			sizeof(payload) - 1,
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

The public API surface is `begin()`, `connect()`, `store()`, `retrieve()`, `quit()`, and `lastError()`. `begin()` initializes the transport layer with the Mbed `NetworkInterface` and must be called once before `connect()`. `lastError()` returns an `FtpsError` enum for programmatic error handling alongside the human-readable `char*` error buffer.

The v1 public config intentionally does not expose `securityMode` or `passiveMode` toggles. Until additional modes are implemented, the library surface is fixed to Explicit FTPS plus protected passive transfers so sketches cannot accidentally rely on unsupported options.

## Trust Model

- SHA-256 leaf-certificate fingerprint pinning
- Imported PEM certificate trust
- Certificate validation required (fail-closed)
- No silent fallback from FTPS to plaintext FTP

## Current and Planned Examples

The repository currently includes:

- `examples/BasicUpload/BasicUpload.ino`
- `examples/BasicDownload/BasicDownload.ino`
- `examples/FileZillaLiveTest/FileZillaLiveTest.ino`
- `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino`
- `FtpsSpikeTest/FtpsSpikeTest.ino`

The upload/download examples and the FileZilla live-test sketch emit structured serial diagnostics so Opta-side runs are easy to inspect in the serial monitor. The web harness sketch hosts a lightweight LAN page for updating FTPS settings and running small connect/upload/download/quit tests without reflashing, uses token-gated actions behind a small passcode unlock step, and supports downloadable plain-text test reports.

Planned follow-up examples after broader transport/client validation lands:

- Trust-mode-focused fingerprint validation example
- Trust-mode-focused imported PEM validation example
- PR4100 reference setup
- vsftpd reference setup
- Additional FileZilla Server reference variants

## Current Status

This repository currently contains planning docs plus a first-pass implementation:

- Architecture and implementation notes
- An execution checklist for the FTPS migration
- A repository bootstrap and extraction plan
- A Phase 0 transport spike plan for Arduino Opta hardware
- Library source files in `src/` (`FtpsClient`, `FtpsTypes`, `FtpsErrors`, trust helpers, transport implementation)
- Upload/download examples, a FileZilla live-test sketch, a web harness live-test sketch, and a full spike sketch

Before a first experimental release, the project still needs:

1. Real Opta hardware validation against FileZilla Server and the other reference servers
2. Protected passive data-channel interoperability checks, especially around TLS session reuse behavior
3. On-device fingerprint and imported-certificate validation runs
4. Example compile validation and release hardening

## Limitations

- Experimental repository; no released library yet
- Arduino Opta is the only planned supported board for v1
- Explicit FTPS only
- Passive mode only
- Compatibility beyond the three reference servers is not yet claimed
- Servers that enforce unsupported TLS session reuse behavior may require additional transport work

## Project Documentation

- [Implementation note](CODE%20REVIEW/FTPS_IMPLEMENTATION_04132026.md)
- [Implementation checklist](CODE%20REVIEW/FTPS_IMPLEMENTATION_CHECKLIST_04132026.md)
- [Repository creation plan](CODE%20REVIEW/FTPS_REPOSITORY_REVIEW_04132026.md)
- [Phase 0 spike plan](CODE%20REVIEW/FTPS_SPIKE_PLAN_04142026.md)
- [Serial monitor output guide](CODE%20REVIEW/SERIAL_MONITOR_OUTPUT_04152026.md)
- [Web harness API reference](CODE%20REVIEW/WEB_HARNESS_API_REFERENCE_04152026.md)

## License

This repository uses the CC0 1.0 license. See [LICENSE](LICENSE).
