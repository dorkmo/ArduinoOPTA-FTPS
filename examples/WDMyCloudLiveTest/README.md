# WD My Cloud OS 5 Live Test

End-to-end FTPS test for Arduino Opta against a **WD My Cloud NAS** running
My Cloud OS 5. Exercises connect, mkdir, upload, size, download, content
verification, and quit over Explicit FTPS with SHA-256 fingerprint pinning.

## Compatible NAS Models

Any WD My Cloud NAS running **My Cloud OS 5** firmware:

- WD My Cloud (single-bay, OS 5-capable models)
- WD My Cloud Mirror Gen2
- WD My Cloud EX2 Ultra
- WD My Cloud EX2100 / EX4100
- WD My Cloud DL2100 / DL4100
- WD My Cloud PR2100 / PR4100

## NAS Setup

### 1. Enable FTP Access

1. Open the My Cloud dashboard in a browser (e.g. `http://192.168.1.200` or
   the device URL like `http://mycloudpr4100`).
2. Navigate to **Settings > Network > FTP Access**.
3. Toggle **FTP Access** to **ON**.
4. Toggle **FTP SSL** to **ON** (this enables Explicit FTPS / AUTH TLS).
5. Click **Save** or **Apply**.

### 2. Create or Identify an FTP User

My Cloud OS 5 FTP access uses the same user accounts as SMB/CIFS shares.

1. Go to the dashboard home page.
2. Click **Users** (or the user icon).
3. Either use an existing user or create a new one with a strong password.
4. Make sure the user has read/write access to at least one share.

The FTP root for a user is typically the NAS share root (e.g. `/`), so
paths like `/Public/ftps_test` map to the Public share.

### 3. Get the NAS Certificate Fingerprint

The NAS uses a self-signed TLS certificate. You need its SHA-256 fingerprint
for the Arduino sketch.

From a PC on the same LAN, run:

```bash
openssl s_client -connect 192.168.1.200:21 -starttls ftp < /dev/null 2>/dev/null \
  | openssl x509 -fingerprint -sha256 -noout
```

This prints something like:

```
sha256 Fingerprint=AB:CD:EF:12:34:...
```

Remove the colons and paste the 64-character hex string into `FTP_FINGERPRINT`
in the sketch. For example:

```
AB:CD:EF:12:34:56:...  -->  ABCDEF123456...
```

**Windows (PowerShell) alternative:**

```powershell
# Requires OpenSSL installed (e.g. via Git for Windows, Chocolatey, or standalone)
echo "" | openssl s_client -connect 192.168.1.200:21 -starttls ftp 2>$null | openssl x509 -fingerprint -sha256 -noout
```

**If openssl is not available**, you can also use FileZilla or WinSCP to
connect to the NAS over FTPS and inspect the certificate details in the
connection dialog. Look for the SHA-256 fingerprint.

### 4. Note the NAS IP Address

Find the NAS IP in the dashboard under **Settings > Network** or on your
router's DHCP client list. Use this IP for `FTP_HOST` in the sketch.

For reliable operation, consider assigning the NAS a static IP or a DHCP
reservation on your router.

## Sketch Configuration

Edit the configuration block at the top of `WDMyCloudLiveTest.ino`:

```cpp
static const char *FTP_HOST       = "192.168.1.200";    // NAS IP
static const uint16_t FTP_PORT    = 21;                  // default
static const char *FTP_USER       = "ftpuser";           // My Cloud user
static const char *FTP_PASS       = "changeme";          // user password
static const char *FTP_FINGERPRINT = "ABCDEF123456...";  // from step 3
```

If DHCP hangs on your Opta, set `USE_STATIC_IP = true` and configure the
static IP block.

## Expected Serial Output

```
==============================================
  WD My Cloud OS 5 Live FTPS Test
==============================================
[INFO] Target NAS: 192.168.1.200:21
[INFO] Trust mode: Fingerprint
[INFO] Mode: Explicit FTPS with protected passive transfers.
[STEP] Ethernet initialization
[PASS] Ethernet.begin(): 192.168.1.50
[STEP] FtpsClient.begin()
[PASS] FtpsClient.begin()
[STEP] FtpsClient.connect()
[FTPS] connect:tcp-open
[FTPS] connect:banner
[FTPS] connect:auth-tls
[FTPS] connect:control-tls
...
[PASS] FtpsClient.connect()
[STEP] FtpsClient.mkd() parent
[PASS] FtpsClient.mkd(): /ftps_test
...
[PASS] FtpsClient.store(): uploaded /ftps_test/opta_live/wd_mycloud_test.txt
[PASS] FtpsClient.size(): 107 bytes
[PASS] FtpsClient.retrieve(): downloaded 107 bytes
[PASS] Content verification: download matches upload.
[PASS] FtpsClient.quit()

==============================================
  WD MY CLOUD LIVE TEST COMPLETE
==============================================
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Connect fails at `connect:auth-tls` | FTP SSL not enabled on NAS | Enable **FTP SSL** in dashboard Settings > Network |
| Connect fails at `connect:control-tls` | Fingerprint mismatch | Re-extract fingerprint with `openssl s_client` |
| Connect fails at `connect:user` | Wrong username | Check user exists in My Cloud dashboard |
| Connect fails at `connect:pass` | Wrong password | Reset password in My Cloud dashboard |
| `mkd()` fails | User lacks write permission on target share | Grant read/write access to the share |
| Ethernet hangs | DHCP timeout | Set `USE_STATIC_IP = true` |
| Connection timeout | NAS and Opta on different subnets/VLANs | Ensure both are on the same LAN segment |

## Notes

- My Cloud OS 5 uses **Explicit FTPS** (AUTH TLS on port 21), which matches
  this library's default mode.
- The NAS generates a self-signed certificate at setup time. The fingerprint
  may change after a firmware update or factory reset.
- The NAS FTP server enforces passive mode, which is the only mode this
  library supports.
- Tested NAS models use port 21 for FTP. The passive data port range is
  managed by the NAS and is not user-configurable.
