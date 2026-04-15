# Review Fix Tracker — 2026-04-15

This file maps findings from both review documents to current implementation status.

Sources:
- CODE REVIEW/CODE_REVIEW_04152026.md
- CODE REVIEW/REPOSITORY_CODE_REVIEW_04152026.md

Status legend:
- Fixed: Implemented and visible in current code/docs.
- Partial: Improved, but not fully guaranteed across all backends/servers.
- Open: Not yet implemented.

---

## A) CODE_REVIEW_04152026.md Mapping

| Finding | Status | Evidence |
|---|---|---|
| BUG-1 overlap snprintf in failWith | Fixed | src/FtpsClient.cpp:64 |
| BUG-2 NSAPI_ERROR_WOULD_BLOCK include fragility | Fixed | src/FtpsClient.cpp:13 |
| BUG-3 PASV tuple bounds validation | Fixed | src/FtpsClient.cpp:210, src/FtpsClient.cpp:226, src/FtpsClient.cpp:268 |
| PITFALL-1 retrieve would-block handling | Fixed | src/FtpsClient.cpp:918 |
| PITFALL-2 writeAll would-block handling | Fixed | src/FtpsClient.cpp:90 |
| PITFALL-3 caller-owned config pointer lifetime | Fixed | src/FtpsClient.h:50, src/FtpsClient.h:58, src/FtpsClient.h:62, src/FtpsClient.cpp:433, src/FtpsClient.cpp:497 |
| PITFALL-4 no TLS session reuse for data channel | Partial | src/transport/MbedSecureSocketFtpsTransport.cpp:82, src/transport/MbedSecureSocketFtpsTransport.cpp:93, src/transport/MbedSecureSocketFtpsTransport.cpp:98, src/transport/MbedSecureSocketFtpsTransport.cpp:363 |
| PITFALL-5 quit outcome not checked in examples | Fixed | examples/BasicUpload/BasicUpload.ino:89, examples/BasicDownload/BasicDownload.ino:94, examples/FileZillaLiveTest/FileZillaLiveTest.ino:61 |
| OBS-1 dead dataPort variable in FtpsClient transfer paths | Fixed | src/FtpsClient.cpp:210, src/FtpsClient.cpp:705, src/FtpsClient.cpp:840 |
| OBS-2 store/retrieve duplication | Open | src/FtpsClient.cpp (store/retrieve flow remains duplicated) |
| OBS-3 tabs vs spaces style drift | Open | src/transport/MbedSecureSocketFtpsTransport.cpp (tab-indented) |
| OBS-4 redundant hostname set twice | Fixed | src/transport/MbedSecureSocketFtpsTransport.cpp:165 (configureTlsSocket has no set_hostname call; hostname is passed in wrapper constructors) |
| OBS-5 spike cert-validation flag naming ambiguity | Fixed | FtpsSpikeTest/FtpsSpikeTest.ino:36, FtpsSpikeTest/FtpsSpikeTest.ino:297 |
| OBS-6 peer-cert extraction future-compatibility risk | Partial | src/transport/MbedSecureSocketFtpsTransport.cpp:197 (Mbed TLS 3.x guard added) |
| OBS-7 README linked docs missing from listing | Fixed | README.md:193 and CODE REVIEW directory now includes SERIAL_MONITOR_OUTPUT_04152026.md and NAMING_CONVENTIONS_04152026.md |

Notes:
- PITFALL-4 remains partial because strict server-side session reuse behavior is backend-dependent and may still fail depending on TLSSocketWrapper capabilities.
- OBS-6 remains partial because the compatibility guard prevents compile-time breakage but can disable fingerprint extraction under unsupported configurations.

---

## B) REPOSITORY_CODE_REVIEW_04152026.md Mapping

| Finding | Status | Evidence |
|---|---|---|
| 1) Control-channel desync after transfer failures | Fixed | src/FtpsClient.cpp:202, src/FtpsClient.cpp:771, src/FtpsClient.cpp:908, src/FtpsClient.cpp:921, src/FtpsClient.cpp:945 |
| 2) PASV host trusted directly from server | Fixed | src/FtpsClient.cpp:715, src/FtpsClient.cpp:850 |
| 3) PASV tuple bounds not validated | Fixed | src/FtpsClient.cpp:226, src/FtpsClient.cpp:268 |
| 4) STOR/RETR truncation not detected | Fixed | src/FtpsClient.cpp:743, src/FtpsClient.cpp:878 |
| 5) isIpLiteral misclassification risk | Fixed | src/FtpsClient.cpp:75, src/FtpsClient.cpp:76 |
| 6) validateServerCert semantics inconsistent | Fixed | src/FtpsClient.cpp:374, README.md:138 |
| 7) PEM validation only structural | Fixed | src/FtpsTrust.cpp:9, src/FtpsTrust.cpp:99 |
| 8) repeated dynamic allocation in transport hot paths | Partial | src/transport/MbedSecureSocketFtpsTransport.cpp:111, src/transport/MbedSecureSocketFtpsTransport.cpp:138, src/transport/MbedSecureSocketFtpsTransport.cpp:405, src/transport/MbedSecureSocketFtpsTransport.cpp:419 |

Notes:
- Item 8 is partial because TCPSocket reuse is implemented, but TLSSocketWrapper objects are still allocated per TLS channel setup.

---

## C) Additional Transport-Focused Follow-Up (Post-Review)

These are additional hardening updates completed during the follow-up pass:
- Best-effort control->data TLS session reuse hinting via cached mbedtls_ssl_session.
- Socket object reuse to reduce repeated heap churn.
- Mbed TLS 3.x compatibility guard around peer-certificate fingerprint extraction path.

Change log references:
- CHANGELOG.md:34
- CHANGELOG.md:35
- CHANGELOG.md:36
