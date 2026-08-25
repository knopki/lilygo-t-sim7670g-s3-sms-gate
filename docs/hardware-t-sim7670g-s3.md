# Hardware: LilyGO T-SIM7670G-S3 — ESP32-S3 + SIM7670G + GNSS

Detailed reference for the single board used in this project. The goal is to
remove the frequent confusion between `SIM7670G` vs `A7670G`, between Classic
vs Standard revisions, and between "GPS antenna" vs "GPS module".

> Verified on the project's unit with `tools/modem_probe` and
> `tools/gps_probe` — see §9.

## 1. Board at a glance

* **Product:** LilyGO T-SIM7670G-S3, ESP32-S3-WROOM-1 + SIMCom SIM7670G-MNGV
* **This unit:** **Classic** revision (`H707`, `LILYGO_T_SIM7670G_S3`), not
  Standard (`H802`). Determined by live probing — Standard pins are silent,
  Classic `RX=GPIO10 TX=GPIO11` answers `AT -> OK`.
* **SoC:** ESP32-S3 dual-core LX7 @ 240 MHz, 16 MB Flash (Quad-SPI), 8 MB PSRAM
  (OPI on Classic, Quad-SPI on Standard), Wi-Fi 802.11 b/g/n, Bluetooth 5 LE.
* **Cellular:** SIMCom SIM7670G-MNGV LTE Cat-1, 10 Mbps DL / 5 Mbps UL,
  global bands, LCC+LGA 24×24 mm, supply 3.4–4.2 V, Nano SIM (1.8/3.0 V).
* **GNSS:** **integrated inside SIM7670G** — GPS L1 + GLONASS + BeiDou
  (+ Galileo/QZSS on newer firmware). No external GPS chip is needed.
  See §4.
* **Antennas:** LTE main IPEX, GNSS active-antenna IPEX + pogo-pin pad.
  AUX LTE not required. LTE and GNSS antennas are different parts.
* **USB:** `ESP-USB` (Type-C) for flashing/monitoring/power, `Modem-USB`
  for modem firmware upgrade (SBOOT). `ESP-USB` must be used to activate the
  battery BMS.
* **Power:** USB-C/VBUS must provide **≥2 A peak** or RF bursts brown out.
  3.7 V Li-Po via JST + BMS, optional 5–6 V solar via JST 2.0 (charge only).
* **Partition used by firmware:** `appcfg` NVS at `0x610000/0x6000` — survives
  normal flash, erasable via bootloader without `erase-flash`.

## 2. Classic vs Standard — why it matters

Both revisions share ESP32-S3 but the modem wiring differs:

| Signal | Classic (`LILYGO_T_SIM7670G_S3`, H707) | Standard (`_STAN`, H802) |
|--------|----------------------------------------|--------------------------|
| Modem UART (Serial1, 115200 8N1) | RX GPIO10, TX GPIO11 | RX GPIO5, TX GPIO4 |
| PWRKEY | GPIO18 | GPIO46 |
| DTR (LOW = awake) | GPIO9 | GPIO7 |
| RING | GPIO3 | GPIO6 |
| RESET (active LOW) | GPIO17 | — |
| Power Save Mode (HIGH = max PSU) | — | GPIO42 |
| GPS ant bias GPIO | **GPIO4** `AT+CGDRT=4,1; AT+CGSETV=4,1` | **GPIO1** `AT+CGDRT=1,1; AT+CGSETV=1,1` |
| Camera, QWIIC, eSIM pad, PPS routing | no | yes |

This project's board is Classic — confirmed by `tools/modem_probe` trying
Standard first (20 s timeout) then Classic (reply in 123 ms) and by
`tools/gps_probe` where `AT+CGDRT=4,1 OK` succeeded. Code must not assume
Standard pins. The probe sketches auto-detect; production firmware fixes the
Classic map per `docs/research/modem-sim7670g.md §1`.

**SIM7670G vs A7670G confusion.** The table that marks "A7670G = no GPS" is
for the **T-A7670E/G/SA R2** family (`A7670E` has GPS, `A7670G` does not).
`SIM7670G` is a different SimCom part and **does have GNSS** — LilyGo's
`docs/en/esp32s3/sim7670g-s3/README.MD` and `.../sim7670g-s3-standard/README.MD`
both list `SIM7670G | GPS ✅ | Phone ❌ | SMS ✅`.

## 3. ESP32-S3 — relevant pins and peripherals

* Classic `utilities.h` map: `Modem TX 11 / RX 10 / PWRKEY 18 / DTR 9 / RESET 17 / RING 3`.
* SD: `SCK 21 / MISO 47 / MOSI 14 / CS 13`.
* Battery/Solar ADC: `GPIO4 / GPIO5` (Classic).
* Board LED: `GPIO12`.
* Unlisted GPIOs are free for I/O; ADC only on 1–21; strapping pins
  0/3/45/46 need care.
* Flash/PSRAM as above; no PSRAM beyond 8 MB.

## 4. SIM7670G modem — cellular + SMS + GNSS in one chip

### 4.1 Cellular

* Bands: LTE-FDD B1/2/3/4/5/7/8/12/13/18/19/20/25/26/28/66/71,
  LTE-TDD B34/38/39/40/41, **no GSM** fallback.
* This unit: firmware `2374B03SIM767XM5A_M` (`Revision V1.9.05`, `AT+CGMR`),
  IMEI `864643060158781` (from `ATI` in probe log). Per LilyGo,
  `2374B04` is the SMS-confirmed build; `2374B03` also sends SMS on Tele2 RU
  LTE (`AT+CMGS -> +CMGS: <mr> OK` verified with `tools/modem_probe/send_sms.py`,
  `CSQ 22/-69 dBm, RSRP -97, CEREG 0,1`). SMS requires **SMS over SGs/NAS**
  on LTE — carrier must support it; fallback diagnostic is `AT+CNMP=13` (GSM-only)
  which does not apply here because SIM7670G has no GSM.
* Startup per LilyGo examples: `PWRKEY LOW 100 ms -> HIGH 100 ms -> LOW`,
  poll `AT` until `OK` (10×200 ms, up to 20 s; LilyGo waits 3 s then retries),
  optional auto-baud, `ATE0 ATV1 AT+CMEE=2`, wait for `SMS DONE / PB DONE` URCs
  before SIM work. Boot URCs: `QCRDY, +CPIN: NOT INSERTED, $QCSTKURC`.
* Power: ≥2 A peak on VBUS/ESP-USB, battery BMS wakes only via `ESP-USB`
  plug (even with power switch off). SBOOT (near modem) held at power-on for
  modem DFU via `Modem-USB`.

### 4.2 SMS

Covered in `docs/research/modem-sim7670g.md` §§2–5. Key AT surface verified
on this board: `AT+CMGF=? (0-1), AT+CNMI, AT+CPMS=("ME","SM")`,
`AT+CSQ/AT+CESQ/AT+CREG?/AT+CEREG?/AT+CGATT?/AT+COPS?/AT+CCLK?`. Healthy gate:
`CPIN READY, CEREG 1 or 5, CSQ >12 (~-89 dBm), CGATT 1`. Text-mode send
`AT+CMGS` works with `CSCS GSM/UCS2`; `+CMS ERROR` with `CMEE=2` is the
terminal failure (mirrors the ZTE `sendRejected` contract in ADR-0003).

### 4.3 GNSS — integrated, not a separate module

* SimCom spec: `GNSS Support: GPS (L1) / BDS (B1) / GLONASS / QZSS`,
  4-sat fix minimum, SBAS capable. LilyGo wiki (`wiki.lilygo.cc/.../t-sim7670g-s3`):
  `Integrated GNSS: GPS, GLONASS, BeiDou`.
* The only external part needed is an **active antenna** (§5). The square
  `etecl25t6a-n3-v1` on the pogo pad is that antenna, not a module.
* AT control (Waveshare/LilyGo examples, TinyGSM `modem.enableGPS()`):

  ```
  AT+CGDRT=<gpio>,1 ; AT+CGSETV=<gpio>,1   # bias on — gpio 4 on Classic, 1 on Standard
  AT+CGNSSPWR=1        # engine on  (AT+CGNSSPWR? -> 1)
  AT+CGNSSMODE=3       # GPS+BD+GLONASS (read-back should be 3)
  AT+CGPSINFO          # NMEA-like: lat,N,lon,E,date,time,alt,speed
  AT+CGNSSINFO         # sat count / fix status (,,,,,,, = no fix)
  AT+CGNSSPORTSWITCH=0,1 / AT+CGNSSTST=1  # optional NMEA routing
  AT+CGPSCOLD          # cold start (forces TTFF)
  AT+CGNSSPWR=0        # engine off
  ```

  Before `CGNSSPWR=1`, `AT+CGNSSMODE?` and `AT+CGNSSINFO` correctly return
  `ERROR`; after power-on they return `+CGNSSMODE: 3` and `+CGNSSINFO: ,,,,,,,,`
  until a fix appears. This transition is the live test that the RF chain is
  alive (see log in §9).

* TinyGSM example (from `wiki.lilygo.cc/.../quick-start.html`):

  ```cpp
  modem.enableGPS();
  float lat, lon, speed, alt; int vsat, usat;
  modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat);
  ```

## 5. GNSS antenna etecl25t6a-n3-v1 and the pogo pin

* **What it is:** 25×25 mm **active ceramic patch** (the "25" in the part number),
  1575.42 MHz L1, ~28 dBi, two-stage LNA + SAW filter inside the square
  housing, 3.3 V bias (tolerant 2.5–5.5 V, e.g. Sanav MK-76). Passive copper
  alone is insufficient.
* **How it connects:** either to the `GNSS` IPEX1 on the board edge or to the
  `GPS` pogo-pin pad — same net, same modem RF input. The pogo is just a
  spring contact for patch antennas with a pad; it is not a module socket.
  LilyGo tables label it `GNSS | GPS active antenna interface`.
* **Bias supply:** the modem drives antenna voltage via `AT+CGDRT/AT+CGSETV`.
  `Standard: GPIO1, Classic: GPIO4`. Without `...=1,1` the LNA is unpowered
  and no satellite is seen, even outdoors — the classic pitfall in
  `Xinyuan-LilyGO/LilyGo-Modem-Series#394` where `CGNSSINFO` stayed `,,,,,,,`
  for 30 min on a T-SIM7670G-S3 with correct antenna indoors.
* **Placement for a fix:** adhesive side down, ceramic up, outdoors with full
  sky view. Indoors/through window film or inside a metal case: `no_fix` is
  expected. Cold TTFF 30–60 s after `CGNSSPWR=1`, up to several minutes on
  first lock.
* **LED:** `GPS PPS` (red near modem) — `ON = starting, OFF = ready, 1 Hz flash = fix`.
  Mirrors `AT+CGNSSINFO` fix status.

## 6. Connectors, jumpers, and indicators

* `ESP-USB` Type-C: ESP32-S3 flashing + CDC `Serial`, power, BMS wake.
* `Modem-USB` Type-C + `SBOOT` button: modem firmware upgrade (`2374B04/05`
  via SBOOT DFU — see LilyGo `T-A76XX Upgrade docs`).
* `GNSS` IPEX + `GPS` pogo + `LTE` main IPEX (AUX not needed).
* `QWIIC` (Standard only), `JST Solar` (5–6 V charge only, not board power),
  `TF` (FFat at `0x616000`), `CAM 24-pin` (Standard).
* LEDs: `MODEM STATE` (startup ON), `MODEM NET` (flashing after attach),
  `GPS PPS` (§5), `CHARGE` / `CHARGE DONE` near battery switch (DONE stays on
  with no battery).
* VBUS = USB-C 5 V, VBAT = 4.2 V header; VBUS is the only cabled power input
  besides battery.

## 7. AT quick reference (validated on this board)

| Purpose | Command | Good reply |
|---------|---------|------------|
| Probe alive | `AT` / `ATE0` | `OK` |
| Identity | `ATI` / `AT+CGMM` / `AT+CGMR` / `AT+GSN` | `SIM7670G-MNGV / 2374B03 / 86464306...` |
| SIM | `AT+CPIN?` | `READY / NOT INSERTED` |
| Signal | `AT+CSQ` / `AT+CESQ` | `23,0 ~ -67 dBm / RSRP -97` |
| Registration | `AT+CREG?` / `AT+CEREG?` | `0,1 home / 0,6 SMS-only NAS` |
| Attach/Operator | `AT+CGATT?` / `AT+COPS?` | `1 / 25020` |
| SMS storage | `AT+CPMS? / =?` | `ME/SM` |
| Antenna bias | `AT+CGDRT=4,1; AT+CGSETV=4,1` (Classic) | `OK` |
| GNSS power | `AT+CGNSSPWR=1` / `AT+CGNSSPWR?` | `OK / 1` |
| GNSS status | `AT+CGNSSMODE?` / `AT+CGNSSINFO` / `AT+CGPSINFO` | `3 / ,,,,,,,, (no fix) / lat,lon` |

Full SMS flow is in `docs/research/modem-sim7670g.md` §§3–5 and ADR-0003.

## 8. Firmware variants and upgrade

* `2374B03` (this board): SMS and GNSS AT surface both present, SMS send
  actually works on LTE via SGs/NAS on Tele2 RU.
* `2374B04` (LilyGo-confirmed SMS): requires SGs/NAS, needs base-station
  support; some `A7670E -LNXY-UBL / -LNMV` builds have no SMS/voice at all.
* `2374B05` (issue #394): same GNSS behavior, still `CGNSSINFO ,,,,,,,,` indoors.
* Upgrade via `Modem-USB` holding `SBOOT` at power-on — documented in
  `LilyGo-Modem-Series/docs/update_fw.md`. Not required for GNSS on 2374B03.

## 9. Verified state on this unit

`tools/modem_probe` then `tools/gps_probe` (Classic, pogo antenna
`etecl25t6a-n3-v1` indoors, no sky):

```
event=variant_start variant=standard rx=5 ... -> variant_failed
event=variant_start variant=classic rx=10 tx=11 pwrkey=18 dtr=9 ant_gpio=4
event=uart_noise variant=classic bytes_hex=41540D0D0A4552524F520D0A
event=modem_ready variant=classic
ATI -> Manufacturer: SIMCOM INCORPORATED | Model: SIM7670G-MNGV | Revision: V1.9.05 | IMEI: 864643060158781
AT+CGMR -> 2374B03SIM767XM5A_M
AT+CPIN? -> READY | AT+CSQ -> 23,0 | AT+CGNSSPWR? -> 0 | AT+CGNSSMODE? -> ERROR | AT+CGNSSINFO -> ERROR
AT+CGDRT=4,1 -> OK | AT+CGSETV=4,1 -> OK
AT+CGNSSPWR=1 -> OK | AT+CGNSSPWR? -> 1 | AT+CGNSSMODE? -> 3
gnss_poll count=1..10 every 3s: AT+CGPSINFO -> ,,,,,,,, | AT+CGNSSINFO -> ,,,,,,,, | fix_hint=no_fix
```

Transition `ERROR -> ",,,,,,,,"` and stable `PWR:1 MODE:3` for 27 s proves:
antenna bias path, Classic GPIO4 mapping, and integrated GNSS engine are all
healthy. `no_fix` indoors is expected; a fix appears only outdoors (PPS 1 Hz,
`CGPSINFO` fills lat/lon).

Reproduce: `mise exec -- arduino-cli upload tools/gps_probe -p /dev/ttyACM0`
and monitor at 115200. Passthrough stays active for manual `AT` entry.

## 10. Pitfalls

* **SIM7670G ≠ A7670G.** Tables marking `A7670G ❌ GPS` are for the T-A7670
  family; your T-SIM7670G-S3 `✅ GPS` is the correct row.
* **Active antenna bias must be on before `CGNSSPWR=1`.** `AT+CGDRT/CGSETV`
  per revision (1 vs 4). Most "GNSS not working" reports are just this or
  indoor use.
* **Only active antennas (2.5–5.5 V) on `GNSS`.** Passive or 1.8 V-only patches
  will not get a fix.
* **Power:** brown-outs with <2 A on USB-C/VBUS look like modem silence.
  Always power via `ESP-USB` to wake the BMS after battery insert.
* **SMS on LTE:** needs carrier SGs/NAS; if `+CMGS: <mr> OK` never completes,
  check with `AT+CNMP` or another carrier, and consider `2374B04`+ on
  `Modem-USB`.

---
*Sources: LilyGo T-SIM7670G-S3 wikis (`wiki.lilygo.cc/.../t-sim7670g-s3`,
`.../quick-start.html`), `LilyGo-Modem-Series/docs/en/esp32s3/sim7670g-s3{,-standard}/README.MD`,
`LilyGo-Modem-Series#394`, SimCom SIM7670X datasheets, and the project's
own probes `tools/modem_probe/modem_probe.ino` + `tools/gps_probe/gps_probe.ino`
and research notes `docs/research/modem-sim7670g.md`.*
