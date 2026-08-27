# LilyGO T-SIM7670G-S3 SMS Gate

A gateway based on the **LilyGO T-SIM7670G-S3** (LilyGo SIM7670G ESP32S3) for
delivering SMS messages by email and using GNSS as an accurate time source.
Product page: <https://lilygo.cc/products/t-sim-7670g-s3>.

## Status

The firmware now implements:

- first-time captive-portal setup; WPA2/WPA3-Personal STA with fallback AP;
  Digest-authenticated HTTP;
- dedicated persistent `appcfg` partition with USB recovery;
- email delivery over SMTP STARTTLS/Implicit TLS (Mozilla bundle);
- SMS forwarding and sending via the onboard SIM7670G modem and via ZTE MF79RU
  (additional) — both poll `REC UNREAD`, forward as email, delete after SMTP
  `250`;
- GNSS polling (`AT+CGPSINFO`/`CGNSSINFO`) and unified time sync: `GNSS > SNTP >
  NITZ` arbitration with forward-only discipline, quorum quarantine,
  configurable SNTP servers, and a minimal NTP server (stratum 1 when GNSS, `GET
  /api/time`).

## SMS sources

### Primary: onboard SIM7670G (ADR-0004)

The primary SMS source is the board's own SIM7670G modem on `Serial1` (Classic
pin map `RX=10/TX=11`, `PWRKEY=18`). The firmware polls it with AT commands
(`AT+CMGF=1`, `AT+CSCS="UCS2"`, `AT+CPMS="ME","ME","ME"`, `AT+CNMI=2,1`) and
forwards one oldest `REC UNREAD` SMS per poll cycle as email through the
configured SMTP profile. Storage `ME` (flash, large) is the inbox; `AT+CMGL="REC
UNREAD"` pages it, UCS-2-hex is decoded to UTF-8, and `AT+CMGD` deletes the
message only after SMTP `250` — crash between `250` and `CMGD` can resend once,
never lose. Enable flag, poll interval (5–300 s, default 15 s) and alias
(`Received on:`) are configured on the protected page; `nitzTimeSyncEnabled`
optionally feeds `AT+CCLK` (NITZ) into TimeSync. Polling runs only with station
connectivity, an enabled SIM7670G profile (`CPIN READY`), and a working SMTP
profile. Status (`CPIN/CSQ/CEREG/COPS/CCLK/CPMS`, `event=modem_status`, `GET
/api/modem/status`) and the last poll outcome are visible in the UI/Serial.

Sending uses `AT+CMGS` (UCS-2-hex number+body, `>` prompt, `0x1A`, `+CMGS/+CMS
ERROR`): up to 335 UTF-16 units, same limit as ZTE. The shared **Send SMS** form
(`via=modem`) validates `To`/`Message` once and is mutual-excluded with
poll/test/send.

### Additional: ZTE MF79RU (ADR-0003)

The additional source is a ZTE MF79RU HiLink modem reachable on the LAN, polled
with the proven Python forwarder protocol: LOGIN with the modem's web password,
mandatory `Referer` header and `stok` cookie, UCS-2-hex decoding, and
single-message deletion with `AD` token. Inbox is the only state: one oldest
incoming SMS per 15-second cycle, verified delete. An additional `via=zte` path
is kept for redundancy or when the onboard modem has no coverage.

The protected page configures the source: enable flag, modem host (for example
`192.168.0.1`), the modem's own web password, and an optional phone number or
alias shown as the `Received on:` line in every forwarded email so you can tell
which source a message came from. **Test connection** performs a non-destructive
login plus capacity read and reports the firmware version and inbox occupancy;
use it before enabling polling. Polling runs only while all three are true:
station connectivity, an enabled and configured ZTE profile, and a working SMTP
profile. The last poll outcome is shown next to the form; the Serial log carries
the detailed events (`zte_poll_begin`, `zte_sms_found`, `zte_forward_result`,
`zte_delete_complete`, …).

Notes:

- The modem must be reachable from the device's network (bridged or routed
  into the LAN). The goform API is plain HTTP with base64-encoded login —
  exactly as exposed by the modem's own web UI — so do not expose the modem
  beyond the trusted LAN.
- Enabling polling deletes SMS from the modem after successful email
  delivery; keep a copy if the inbox matters to you.
- Incomplete concatenated SMS are forwarded immediately with an
  `[INCOMPLETE received/total]` subject prefix instead of being dropped.
- The poller pages through the inbox in bounded chunks, so a full
  100-message inbox cannot exhaust memory; forwarding order is oldest
  first, one SMS per cycle.

### Sending SMS through the ZTE modem (additional source)

The shared **Send SMS** form (`via=zte|modem`) uses the same two fields for both
sources: **To** (3\u201320 digits, optional `+`) and **Message** (up to 335
UTF-16 units). For the ZTE path, the device logs in with the stored ZTE profile,
submits `SEND_SMS` with a fresh `AD` token and the text encoded as UCS-2-hex,
then samples the modem's send status once per second for at most 20 seconds and
reports the outcome. For the onboard modem the same form uses `AT+CMGS` as
described in the Primary section above.

Notes:

- The form needs a saved ZTE profile (host and password); polling does not need
  to be enabled.
- The device synchronizes its clock via unified TimeSync (ADR-0005): `GNSS (ms,
  stratum 1) > SNTP (configurable servers) > NITZ (AT+CCLK)` with freshness
  windows `2×poll+10s / 2h / 5min` and forward-only `settimeofday`/`adjtime`;
  quorum quarantine protects against bad NITZ / GNSS spoofing (SNTP+NITZ agree
  within `10s` while GNSS differs by `300s` → 15 min quarantine). SNTP is
  started/stopped by TimeSync, not by Wi-Fi directly; sends are refused until
  synchronized.
- The timestamp is sent as UTC with an unpadded "+0" offset, mirroring the exact
  shape the modem's own web UI sends ("+3").
- Mirroring the modem's own web UI, plain ASCII text is transmitted with GSM7
  encoding and everything else (Cyrillic, emoji) with UNICODE; the
  device-verified browser request used the same UTF-16-hex body under both
  labels.
- A send is excluded from the poll cycle and the connection test, so the modem
  never serves two dialogs at once.
- After a terminal modem result (delivered or failed), the gateway deletes and
  verifies every final outgoing record in ZTE device storage: tags `2` (sent)
  and `3` (failed). Incoming SMS (`0`/`1`) and drafts (`4`) are not touched. The
  B02 firmware can return a stale list immediately after a DELETE, so the
  gateway retries that verification after a short delay. A cleanup failure is
  reported in the UI and Serial log.
- If the modem accepts the message but its status stays in progress past the
  bound, the UI says so honestly: the message may still be delivered; its record
  is intentionally not removed while the outcome is unknown.
- Delivery also requires a valid SMS center (SMSC) configured in the modem; if
  the modem accepts sends that never complete, check the SMSC in its web UI
  (Settings → SMS).

## Wi-Fi and web configuration

### First-time setup

The web interface is a small JavaScript application served by the device itself;
a JavaScript-capable browser is required. It is split into topic pages with a
shared top navigation:

- **Wi-Fi** — connection status and network change (also the initial-setup
  page);
- **Admin** — administrator password and watchdog status;
- **E-mail** — SMTP settings and a test message;
- **Time Sync** — SNTP servers and clock source status;
- **Modem** — onboard SIM7670G status and SMS source settings;
- **ZTE MF79RU** — additional modem settings, last poll status and test;
- **GPS** — GNSS status and polling settings;
- **SMS** — compose and send a message through any enabled modem.

Each page polls only its own status endpoints. Opening the device root
(`/` or `http://192.168.4.1`) redirects to the Wi-Fi page. With no valid
configuration, the device starts an open access point named `SMS-Gate-<MAC>`.
Connect to it and open `http://192.168.4.1` if the captive portal does not open
automatically.

The initial setup page accepts:

- one WPA2/WPA3-Personal SSID and its 8–63 character password;
- an 8–63 character printable-ASCII administrator password, entered twice.

The page shows the device's Wi-Fi station MAC address for an access-point
allowlist. Select **Scan nearby networks** to display a visible list of
compatible SSIDs; scans are not run while the ordinary page loads. A manually
entered SSID remains available for hidden networks. The candidate Wi-Fi profile
is tested for up to 30 seconds and is saved only after a successful connection.
Nothing is saved after a failed test.

After success, the open AP closes and the web interface is available on the
configured network at `http://sms-gate-<MAC>.local`. The assigned IP address is
also printed through USB CDC Serial.

### Normal and fallback operation

At boot, a saved profile is tried for 30 seconds. If it cannot connect, the
device starts `SMS-Gate-<MAC>` as a WPA2 access point and retries the saved
station profile every 60 seconds. Its WPA2 password is the administrator
password. The fallback AP closes after station connectivity returns.

Once initial setup is done, every page and API request uses HTTP Digest
authentication with username `admin`; the browser shows its native login
prompt as soon as a page is opened. Page scripts and styles are static,
secret-free assets served without a challenge. Assets carry an ETag with one
hour of freshness (the same for every asset, so a page and its scripts expire
together), and the page scripts prefetch every page and script into the
browser cache once per session, so menu navigation renders from cache without
waiting on the device. After flashing new firmware, force one reload (F5) to
pick up changed pages immediately. HTTP is intentionally not
encrypted;
deploy a reverse proxy or other network controls if access extends beyond a
trusted local network.

After success, the open AP closes and the web interface is available on the
configured network at `http://sms-gate-<MAC>.local`. The assigned IP address is
also printed through USB CDC Serial.

### Pages

The Wi-Fi page shows the mode, configured SSID, station IP, MAC, RSSI, mDNS
name, and the last connection error. The interface never displays or writes
passwords to Serial. Changing the administrator password (Admin page) requires
the current password and confirmation of the new password.

Only WPA2/WPA3-Personal SSID/password networks are supported. Open, WEP, and
Enterprise Wi-Fi networks are deliberately unsupported.

## Email delivery (SMTP)

The protected page configures the SMTP profile used to forward SMS as email:
host, port, security (STARTTLS 587 or implicit TLS 465), username, password, and
from and recipient addresses. The password is never returned to the browser;
leaving the field empty keeps the stored one. **Send test email** performs a
real delivery with the values currently in the form and reports the outcome.

TLS trust needs no operator setup: every connection validates the server
certificate chain, expiry, and hostname against the Mozilla root bundle embedded
in the firmware, the same root set browsers use. If a delivery fails with
`tls_failed`, the provider presented a certificate that publicly trusted roots
do not validate — most often a wrong host name in the settings.

## Hardware

- LilyGO T-SIM7670G-S3: ESP32-S3, SIM7670G LTE modem, and GNSS antenna
  connection.
- A SIM card with SMS service for the SIM7670G.
- LTE and active GNSS antennas.
- ZTE MF79RU as a separate SMS source, reachable from the device's network.

Use the board's **ESP-USB** USB-C connector to flash the ESP32-S3. The
`Modem-USB` connector is for modem servicing, not sketch uploads.

## Build and upload

`arduino-cli` must be installed. Install the ESP32 core once:

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

The board normally appears as `/dev/ttyACM0`. Check the actual port before uploading:

```bash
arduino-cli board list
```

From the repository root:

```bash
python3 tools/gen_assets.py   # or: mise run assets / mise run compile
arduino-cli compile sms_gate
arduino-cli upload sms_gate
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

`sms_gate/web_assets.h` is generated from `www/`; the compile step requires it.
Re-run the generator after editing anything under `www/`.

Board and port settings are versioned in `sms_gate/sketch.yaml`. The sketch-local `sms_gate/partitions.csv` is part of the firmware contract: it adds the dedicated `appcfg` partition and must be built and flashed with the sketch.

Uploads use the hardware USB-Serial/JTAG peripheral, so `arduino-cli
upload` resets the board into download mode automatically; no `BOOT`/`RST`
key sequence is required in normal operation. If the bootloader is still not
detected, hold `BOOT`, press and release `RST`, release `BOOT`, then retry the
upload.

### Arduino IDE

1. Generate the UI header once (and after every `www/` change): `python3
   tools/gen_assets.py`.
2. Open `sms_gate/sms_gate.ino`. 3. Select **ESP32S3 Dev Module** and
   `/dev/ttyACM0`.
4. Under `Tools`, set: `USB Mode: Hardware CDC and JTAG`, `USB CDC On Boot:
   Enabled`, `Flash Mode: QIO 80MHz`, `Flash Size: 16MB (128Mb)`, `PSRAM: QSPI
   PSRAM`, and `Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)`.
5. Upload the sketch and open Serial Monitor at `115200` baud.

The sketch-local `partitions.csv` overrides the selected partition CSV during
the Arduino-ESP32 build. It keeps both 3 MB OTA application slots, creates
`appcfg` at `0x610000` with size `0x6000`, and reduces FFat by 24 KiB.

## Recover forgotten configuration by USB

This is the supported recovery path when the administrator password is forgotten
or the firmware cannot provide its web interface. It does **not** use `BOOT` as
an application button: GPIO0 is a boot strapping pin and must only be used to
enter the ESP32 ROM bootloader.

> [!WARNING]
> These instructions apply only after flashing firmware built from this
  repository with `sms_gate/partitions.csv`. Do not use them for an older
  firmware with a different partition table. Never run `erase-flash`: it erases
  the bootloader, partition table, and firmware.

### What recovery erases

The command erases exactly the `appcfg` NVS partition:

| Partition | Offset | Size | Effect |
| --- | ---: | ---: | --- |
| `appcfg` | `0x610000` | `0x6000` | Wi-Fi SSID/password and web administrator password are removed |

It does not erase the firmware, OTA slots, FFat, coredump partition, or the
default `nvs` partition intended for future independent subsystems.

### Procedure

1. Connect the board's **ESP-USB** connector, not `Modem-USB`.
2. Confirm the actual device path, normally `/dev/ttyACM0`:

   ```bash
   arduino-cli board list
   ```

3. Install `esptool` if it is not already available:

   ```bash
   python3 -m pip install --user esptool
   esptool version
   ```

4. Enter ROM download mode manually: hold **BOOT**, press and release **RST**,
   then release **BOOT**. The board must remain in this mode for the next
   command.
5. Erase only the isolated configuration partition. Replace the port if it
   differs:

   ```bash
   esptool --chip esp32s3 --port /dev/ttyACM0 --before no-reset \
     erase-region 0x610000 0x6000
   ```

   `--before no-reset` is intentional: the board was placed in download mode manually, so the command does not rely on unverified automatic reset behavior of native USB CDC.
6. Allow the default hard reset after `esptool` finishes, or press **RST** once.
The firmware sees no valid `appcfg` record, opens `SMS-Gate-<MAC>`, and returns
to first-time setup at `http://192.168.4.1`.

If `esptool` cannot connect, repeat step 4 and check that the ESP-USB port—not
the modem port—was selected. Do not substitute a whole-flash erase for this
procedure.

## Tests

All host tests are part of the minimum local verification. Run the full suite:

```bash
mise run test
```

### NTP server conformance probe

With the device on the LAN, verify the UDP/123 reply form against the
acceptance rules chrony and ntpd apply (normal reply, VN clamp, mode
filter, KoD RATE above 20 req/s):

```bash
python3 tools/ntp_probe.py sms-gate-<MAC>.local
```


## Next steps

- Watchdog and brown-out resilience.
- UI polish for TimeSync (`GET /api/time` already exposed) and NTP status.

## Board documentation

- [LilyGO T-SIM7670G-S3 product page](https://lilygo.cc/products/t-sim-7670g-s3)
- [LilyGo-Modem-Series: SIM7670G-S3](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/docs/en/esp32s3/sim7670g-s3/README.MD)
- [LilyGO T-SIM7670G-S3 Standard](https://github.com/Xinyuan-LILYGO/LilyGo-Modem-Series/blob/main/docs/en/esp32s3/sim7670g-s3-standard/README.MD)
