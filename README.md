# LilyGO T-SIM7670G-S3 SMS Gate

A gateway based on the **LilyGO T-SIM7670G-S3** for delivering SMS messages by email and using GNSS as an accurate time source.

## Status

The firmware now implements the device's Wi-Fi and local web-configuration foundation:

- first-time captive-portal setup;
- WPA2/WPA3-Personal station connection with one saved profile;
- fallback access point after a failed connection;
- Digest-authenticated configuration over HTTP;
- a dedicated persistent `appcfg` partition that supports USB recovery without erasing future SMS/GNSS/email settings.

SMS forwarding from the ZTE MF79RU source, outgoing SMS through the same
modem, and email delivery are implemented; SMS from the board's own
SIM7670G modem and GNSS time remain unimplemented.

## SMS source: ZTE MF79RU (ADR-0003)

The device polls a ZTE MF79RU HiLink modem reachable on the LAN and forwards
its incoming SMS as email through the configured SMTP profile. This is the
same protocol the proven Python forwarder uses: LOGIN with the modem's web
password, the mandatory `Referer` header and `stok` session cookie,
UCS-2-hex message decoding, and single-message deletion with an `AD` token.
The modem inbox is the only delivery state: one oldest incoming SMS per
15-second poll cycle is emailed, deleted, and the deletion is verified. A
crash between email acceptance and the verified delete can resend one
message; nothing is lost.

The protected page configures the source: enable flag, modem host (for
example `192.168.0.1`), the modem's own web password, and an optional
phone number or alias shown as the `Received on:` line in every forwarded
email so you can tell which source a message came from. **Test
connection** performs a non-destructive login plus capacity read and
reports the firmware version and inbox occupancy; use it before enabling
polling. Polling runs only while all three are true: station connectivity,
an enabled and configured ZTE profile, and a working SMTP profile. The
last poll outcome is shown next to the form; the Serial log carries the
detailed events (`zte_poll_begin`, `zte_sms_found`, `zte_forward_result`,
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

### Sending SMS through the ZTE modem

The protected page also has a **Send SMS** form with the two fields every
SMS source shares: **To** (a phone number, 3\u201320 digits with an optional
leading `+`) and **Message** (up to 335 characters, counted the way the
modem counts them, so Cyrillic and emoji are valid; the limit matches the
modem web UI's own five-part UNICODE send). The device logs in with the
stored ZTE profile, submits `SEND_SMS` with a fresh `AD` token and the text
encoded as UCS-2-hex, then samples the modem's send status once per second
for at most 20 seconds and reports the outcome.

Notes:

- The form needs a saved ZTE profile (host and password); polling does not
  need to be enabled.
- The device synchronizes its clock over SNTP once the station has
  internet access, because the modem validates the send timestamp against
  its own SNTP clock and rejects far-off times; sends are refused with a
  clear message until the first fix arrives.
- The timestamp is sent as UTC with an unpadded "+0" offset, mirroring
  the exact shape the modem's own web UI sends ("+3").
- Mirroring the modem's own web UI, plain ASCII text is transmitted with
  GSM7 encoding and everything else (Cyrillic, emoji) with UNICODE; the
  device-verified browser request used the same UTF-16-hex body under both
  labels.
- A send is excluded from the poll cycle and the connection test, so the
  modem never serves two dialogs at once.
- After a terminal modem result (delivered or failed), the gateway deletes
  and verifies every final outgoing record in ZTE device storage: tags `2`
  (sent) and `3` (failed). Incoming SMS (`0`/`1`) and drafts (`4`) are not
  touched. The B02 firmware can return a stale list immediately after a
  DELETE, so the gateway retries that verification after a short delay. A
  cleanup failure is reported in the UI and Serial log.
- If the modem accepts the message but its status stays in progress past
  the bound, the UI says so honestly: the message may still be delivered;
  its record is intentionally not removed while the outcome is unknown.
- Delivery also requires a valid SMS center (SMSC) configured in the
  modem; if the modem accepts sends that never complete, check the SMSC
  in its web UI (Settings → SMS).

## Wi-Fi and web configuration

### First-time setup

The web interface is a small JavaScript application served by the device itself; a JavaScript-capable browser is required. With no valid configuration, the device starts an open access point named `SMS-Gate-<MAC>`. Connect to it and open `http://192.168.4.1` if the captive portal does not open automatically.

The initial setup page accepts:

- one WPA2/WPA3-Personal SSID and its 8–63 character password;
- an 8–63 character printable-ASCII administrator password, entered twice.

The page shows the device's Wi-Fi station MAC address for an access-point allowlist. Select **Scan nearby networks** to display a visible list of compatible SSIDs; scans are not run while the ordinary page loads. A manually entered SSID remains available for hidden networks. The candidate Wi-Fi profile is tested for up to 30 seconds and is saved only after a successful connection. Nothing is saved after a failed test.

After success, the open AP closes and the web interface is available on the configured network at `http://sms-gate-<MAC>.local`. The assigned IP address is also printed through USB CDC Serial.

### Normal and fallback operation

At boot, a saved profile is tried for 30 seconds. If it cannot connect, the device starts `SMS-Gate-<MAC>` as a WPA2 access point and retries the saved station profile every 60 seconds. Its WPA2 password is the administrator password. The fallback AP closes after station connectivity returns.

The normal web interface uses HTTP Digest authentication with username `admin`; the browser shows its native login prompt. HTTP is intentionally not encrypted; deploy a reverse proxy or other network controls if access extends beyond a trusted local network.

The protected page shows the mode, configured SSID, station IP, MAC, RSSI, mDNS name, and the last connection error. It never displays or writes passwords to Serial. It allows changing the Wi-Fi profile and administrator password. Changing the administrator password requires the current password and confirmation of the new password.

Only WPA2/WPA3-Personal SSID/password networks are supported. Open, WEP, and Enterprise Wi-Fi networks are deliberately unsupported.

## Email delivery (SMTP)

The protected page configures the SMTP profile used to forward SMS as email:
host, port, security (STARTTLS 587 or implicit TLS 465), username, password,
and from and recipient addresses. The password is never returned to the
browser; leaving the field empty keeps the stored one. **Send test email**
performs a real delivery with the values currently in the form and reports
the outcome.

TLS trust needs no operator setup: every connection validates the server
certificate chain, expiry, and hostname against the Mozilla root bundle
embedded in the firmware (ADR-0002), the same root set browsers use. If a
delivery fails with `tls_failed`, the provider presented a certificate that
publicly trusted roots do not validate — most often a wrong host name in the
settings.

## Hardware

- LilyGO T-SIM7670G-S3: ESP32-S3, SIM7670G LTE modem, and GNSS antenna connection.
- A SIM card with SMS service for the SIM7670G.
- LTE and active GNSS antennas.
- ZTE MF79RU as a separate SMS source, reachable from the device's network.

Use the board's **ESP-USB** USB-C connector to flash the ESP32-S3. The `Modem-USB` connector is for modem servicing, not sketch uploads.

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

`sms_gate/web_assets.h` is generated from `www/` and is not committed; the
compile step requires it. Re-run the generator after editing anything under
`www/`.

Board and port settings are versioned in `sms_gate/sketch.yaml`. The sketch-local `sms_gate/partitions.csv` is part of the firmware contract: it adds the dedicated `appcfg` partition and must be built and flashed with the sketch.

If the bootloader is not detected, hold `BOOT`, press and release `RST`, release `BOOT`, then retry the upload.

### Arduino IDE

1. Generate the UI header once (and after every `www/` change):
   `python3 tools/gen_assets.py`.
2. Open `sms_gate/sms_gate.ino`.
3. Select **ESP32S3 Dev Module** and `/dev/ttyACM0`.
4. Under `Tools`, set: `USB CDC On Boot: Enabled`, `Flash Mode: QIO 80MHz`, `Flash Size: 16MB (128Mb)`, `PSRAM: QSPI PSRAM`, and `Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)`.
5. Upload the sketch and open Serial Monitor at `115200` baud.

The sketch-local `partitions.csv` overrides the selected partition CSV during the Arduino-ESP32 build. It keeps both 3 MB OTA application slots, creates `appcfg` at `0x610000` with size `0x6000`, and reduces FFat by 24 KiB.

## Recover forgotten configuration by USB

This is the supported recovery path when the administrator password is forgotten or the firmware cannot provide its web interface. It does **not** use `BOOT` as an application button: GPIO0 is a boot strapping pin and must only be used to enter the ESP32 ROM bootloader.

> [!WARNING]
> These instructions apply only after flashing firmware built from this repository with `sms_gate/partitions.csv`. Do not use them for an older firmware with a different partition table. Never run `erase-flash`: it erases the bootloader, partition table, and firmware.

### What recovery erases

The command erases exactly the `appcfg` NVS partition:

| Partition | Offset | Size | Effect |
| --- | ---: | ---: | --- |
| `appcfg` | `0x610000` | `0x6000` | Wi-Fi SSID/password and web administrator password are removed |

It does not erase the firmware, OTA slots, FFat, coredump partition, or the default `nvs` partition intended for future independent subsystems.

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

4. Enter ROM download mode manually: hold **BOOT**, press and release **RST**, then release **BOOT**. The board must remain in this mode for the next command.
5. Erase only the isolated configuration partition. Replace the port if it differs:

   ```bash
   esptool --chip esp32s3 --port /dev/ttyACM0 --before no-reset \
     erase-region 0x610000 0x6000
   ```

   `--before no-reset` is intentional: the board was placed in download mode manually, so the command does not rely on unverified automatic reset behavior of native USB CDC.
6. Allow the default hard reset after `esptool` finishes, or press **RST** once. The firmware sees no valid `appcfg` record, opens `SMS-Gate-<MAC>`, and returns to first-time setup at `http://192.168.4.1`.

If `esptool` cannot connect, repeat step 4 and check that the ESP-USB port—not the modem port—was selected. Do not substitute a whole-flash erase for this procedure.

## Project layout

```text
sms_gate/
├── sms_gate.ino      # Wi-Fi lifecycle, HTTP routes, boot trace, Serial events,
│                     # ZTE poll/forward/delete lifecycle, ZTE send lifecycle
├── config_record.h   # Portable checksummed network configuration record
├── config_store.*    # appcfg NVS persistence for network/SMTP/ZTE profiles
├── smtp_record.h     # Portable checksummed SMTP delivery record
├── smtp_client.*     # Host-testable SMTP dialog (STARTTLS, AUTH LOGIN, DATA)
├── smtp_transport.h  # NetworkClientSecure binding with embedded root bundle
├── zte_record.h      # Portable checksummed ZTE SMS-source record
├── zte_client.*      # Host-testable ZTE goform dialog (LOGIN, paging,
│                     # DELETE_SMS, SEND_SMS, send status)
├── zte_transport.h   # NetworkClient binding for the ZTE LAN channel
├── codec.h           # Shared base64/MD5/field validation helpers
├── web_api.*         # JSON API and gzipped UI asset serving
├── web_assets.h      # Generated from www/ (not committed)
├── partitions.csv    # Dedicated appcfg NVS partition and FFat layout
└── sketch.yaml       # Arduino CLI FQBN, board options, and port
www/                  # Client-rendered UI sources (index.html, app.js, style.css)
tools/
└── gen_assets.py     # Gzips www/ into sms_gate/web_assets.h
tests/
├── config_record_test.cpp  # Host test for record integrity and limits
├── smtp_client_test.cpp    # Host test for SMTP record and dialog sequencing
└── zte_client_test.cpp     # Host test for the ZTE goform dialog and record
```

## Tests

The portable configuration-record test has no framework dependency. Run it on a host with a C++17 compiler:

```bash
c++ -std=c++17 -Wall -Wextra -Werror tests/config_record_test.cpp \
  -o /tmp/config_record_test
/tmp/config_record_test
```

The SMTP dialog test scripts a fake server and asserts the exact command
sequence, including the STARTTLS upgrade ordering and base64 credentials:

```bash
c++ -std=c++17 -Wall -Wextra -Werror tests/smtp_client_test.cpp \
  sms_gate/smtp_client.cpp -o /tmp/smtp_client_test
/tmp/smtp_client_test
```

The ZTE dialog test scripts a fake modem through the `ZteChannel` interface
and asserts the request contract (LOGIN base64 password, Referer, stok
cookie, AD token), stale-session relogin, paging/order detection, UCS-2
decoding, delete verification, and the record validation:

```bash
c++ -std=c++17 -Wall -Wextra -Werror tests/zte_client_test.cpp \
  sms_gate/zte_client.cpp -o /tmp/zte_client_test
/tmp/zte_client_test
```

## Next steps

1. Enable power and AT-command communication with the SIM7670G.
2. Receive and process SMS messages from the board's SIM card as a second
   SMS source alongside the ZTE adapter.
3. Add GNSS time acquisition and system time synchronization.

## Board documentation

- [LilyGO T-SIM7670G-S3 Standard](https://github.com/Xinyuan-LILYGO/LilyGo-Modem-Series/blob/main/docs/en/esp32s3/sim7670g-s3-standard/README.MD)
