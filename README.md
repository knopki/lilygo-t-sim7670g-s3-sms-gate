# LilyGO T-SIM7670G-S3 SMS Gate

A gateway based on the **LilyGO T-SIM7670G-S3** for delivering SMS messages by email and using GNSS as an accurate time source.

## Status

The firmware now implements the device's Wi-Fi and local web-configuration foundation:

- first-time captive-portal setup;
- WPA2/WPA3-Personal station connection with one saved profile;
- fallback access point after a failed connection;
- Digest-authenticated configuration over HTTP;
- a dedicated persistent `appcfg` partition that supports USB recovery without erasing future SMS/GNSS/email settings.

SMS processing, GNSS time, email delivery, and the ZTE MF79RU adapter remain unimplemented.

## Wi-Fi and web configuration

### First-time setup

With no valid configuration, the device starts an open access point named `SMS-Gate-<MAC>`. Connect to it and open `http://192.168.4.1` if the captive portal does not open automatically.

The initial setup page accepts:

- one WPA2/WPA3-Personal SSID and its 8–63 character password;
- an 8–63 character printable-ASCII administrator password, entered twice.

The page shows the device's Wi-Fi station MAC address for an access-point allowlist. Select **Scan nearby networks** to display a visible list of compatible SSIDs; scans are not run while the ordinary page loads. A manually entered SSID remains available for hidden networks. The candidate Wi-Fi profile is tested for up to 30 seconds and is saved only after a successful connection. Nothing is saved after a failed test.

After success, the open AP closes and the web interface is available on the configured network at `http://sms-gate-<MAC>.local`. The assigned IP address is also printed through USB CDC Serial.

### Normal and fallback operation

At boot, a saved profile is tried for 30 seconds. If it cannot connect, the device starts `SMS-Gate-<MAC>` as a WPA2 access point and retries the saved station profile every 60 seconds. Its WPA2 password is the administrator password. The fallback AP closes after station connectivity returns.

The normal web interface uses HTTP Digest authentication with username `admin`. HTTP is intentionally not encrypted; deploy a reverse proxy or other network controls if access extends beyond a trusted local network.

The protected page shows the mode, configured SSID, station IP, MAC, RSSI, mDNS name, and the last connection error. It never displays or writes passwords to Serial. It allows changing the Wi-Fi profile and administrator password. Changing the administrator password requires the current password and confirmation of the new password.

Only WPA2/WPA3-Personal SSID/password networks are supported. Open, WEP, and Enterprise Wi-Fi networks are deliberately unsupported.

## Hardware

- LilyGO T-SIM7670G-S3: ESP32-S3, SIM7670G LTE modem, and GNSS antenna connection.
- A SIM card with SMS service for the SIM7670G.
- LTE and active GNSS antennas.
- ZTE MF79RU as a separate SMS source.

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
arduino-cli compile sms_gate
arduino-cli upload sms_gate
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

Board and port settings are versioned in `sms_gate/sketch.yaml`. The sketch-local `sms_gate/partitions.csv` is part of the firmware contract: it adds the dedicated `appcfg` partition and must be built and flashed with the sketch.

If the bootloader is not detected, hold `BOOT`, press and release `RST`, release `BOOT`, then retry the upload.

### Arduino IDE

1. Open `sms_gate/sms_gate.ino`.
2. Select **ESP32S3 Dev Module** and `/dev/ttyACM0`.
3. Under `Tools`, set: `USB CDC On Boot: Enabled`, `Flash Mode: QIO 80MHz`, `Flash Size: 16MB (128Mb)`, `PSRAM: QSPI PSRAM`, and `Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)`.
4. Upload the sketch and open Serial Monitor at `115200` baud.

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
├── sms_gate.ino      # Wi-Fi lifecycle and HTTP route controller
├── config_record.h   # Portable checksummed configuration record
├── config_store.*    # Isolated appcfg NVS persistence and validation
├── web_ui.*          # HTML rendering separated from control flow
├── partitions.csv    # Dedicated appcfg NVS partition and FFat layout
└── sketch.yaml       # Arduino CLI FQBN, board options, and port
tests/
└── config_record_test.cpp  # Host test for record integrity and limits
```

## Tests

The portable configuration-record test has no framework dependency. Run it on a host with a C++17 compiler:

```bash
c++ -std=c++17 -Wall -Wextra -Werror tests/config_record_test.cpp \
  -o /tmp/config_record_test
/tmp/config_record_test
```

## Next steps

1. Enable power and AT-command communication with the SIM7670G.
2. Receive and process SMS messages from the board's SIM card.
3. Add GNSS time acquisition and system time synchronization.
4. Implement email delivery without committing secrets to the repository.
5. Select and implement an adapter for SMS messages from the ZTE MF79RU.

## Board documentation

- [LilyGO T-SIM7670G-S3 Standard](https://github.com/Xinyuan-LILYGO/LilyGo-Modem-Series/blob/main/docs/en/esp32s3/sim7670g-s3-standard/README.MD)
