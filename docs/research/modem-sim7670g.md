# Research: Onboard SIM7670G Modem Usage

Research notes for future work on the onboard SIMCom SIM7670G Cat-1 modem.
Scope: modem status, receiving SMS, sending SMS, managing stored SMS. Not
implemented yet in `sms_gate/`; the current SMS source is the LAN ZTE device
(ADR-0003).

## 0. Confirmed hardware facts (probe run)

Verified with `tools/modem_probe/modem_probe.ino` (temporary probe sketch,
auto-detects both board revisions):

- Board revision: **T-SIM7670G-S3 Classic** (`LILYGO_T_SIM7670G_S3` pin map),
  not the Standard revision. The Standard UART pins are silent on this unit.
- Module: SIM7670G-MNGV, `ATI` reports Revision V1.9.05, `AT+CGMR` =
  **2374B03**SIM767XM5A_M. Per LilyGo, SMS on SIM7670G-MNGV is confirmed for
  firmware **2374B04** (and still requires carrier SMS over SGs/NAS); whether
  2374B03 sends SMS must be tested with a SIM inserted, or the module firmware
  upgraded to 2374B04 via the Modem-USB port (SBOOT button).
- SMS AT surface present: `AT+CMGF=? -> (0-1)` (text+PDU),
  `AT+CNMI=(0,1,2),(0,1,2,3),(0,1,2),(0,1,2),(0,1)`,
  `AT+CPMS=("ME","SM"),("ME","SM"),("ME","SM")`.
- With SIM inserted and registered on Tele2 RU LTE (`COPS: 0,2,"25020",7`,
  `CEREG: 0,1`, `CREG: 0,6` = SMS-only/NAS registration): outbound SMS
  **confirmed working** on firmware 2374B03 — text-mode `AT+CMGS` returned
  `+CMGS: <mr>` + `OK` (`tools/modem_probe/send_sms.py`). Signal sample:
  `CSQ 22` (-69 dBm), RSRP -97 dBm. NITZ clock syncs (`CCLK`).
- With no SIM: `+CPIN: NOT INSERTED`, `+CEREG/+CREG: 0,0`, `+CGATT: 0`,
  `+CCLK` epoch-zero. Boot URCs observed: `QCRDY`, CPIN URC, `$QCSTKURC`.

Sources:

- LilyGO board docs and examples
  (`Xinyuan-LILYGO/LilyGo-Modem-Series`: `docs/en/esp32s3/sim7670g-s3-standard/README.MD`,
  `examples/ATdebug/{utilities.h,ATdebug.ino}`, `examples/SendSMS/SendSMS.ino`,
  `examples/SendSMS/utilities.h`)
- SIMCom LTE AT command reference mirror (`momoiot.co.kr/lte-at/cmd/sms-control/`)
- 3GPP TS 27.007 / 27.005 semantics for the standard commands.

## 1. Hardware bring-up

Two LilyGo revisions exist; pin maps from `LilyGo-Modem-Series/utilities.h`:

| Signal | Classic (`LILYGO_T_SIM7670G_S3`) | Standard (`_STAN`) |
| --- | --- | --- |
| UART (Serial1, 115200 8N1) | RX=GPIO10, TX=GPIO11 | RX=GPIO5, TX=GPIO4 |
| PWRKEY | GPIO18 | GPIO46 |
| DTR (LOW = awake) | GPIO9 | GPIO7 |
| RING | GPIO3 | GPIO6 |
| RESET | GPIO17, active LOW | — |
| Power Save Mode (HIGH = max PSU) | — | GPIO42 |

This project's board is the **Classic** revision (see section 0).

Startup sequence (per LilyGo examples):

1. `pinMode(PWRKEY, OUTPUT)`; LOW 100 ms -> HIGH 100 ms -> LOW.
2. Poll `AT\r\n` until `OK` (examples retry ~10x200 ms per rate; allow several
   seconds total; boot completes asynchronously). LilyGo waits up to
   `MODEM_START_WAIT_MS` (3 s default for SIM7670G) then re-polls.
3. Optional auto-baud scan (115200 first) — the modem may boot at a different
   stored rate; fixed 115200 is what the boards ship with.
4. Before SMS operations, wait for the `SMS DONE` / `PB DONE` URCs (SIM and
   phonebook ready). LilyGo's `SendSMS` waits up to 100 s for `SMS DONE`.
5. Recommended session setup: `ATE0` (echo off), `ATV1`, `AT+CMEE=2`
   (verbose `+CMS ERROR` strings), then SMS setup below.

Power notes: USB-C/VBUS must supply >=2 A peak; RF bursts brown out otherwise.
The battery BMS activates only when powered through `ESP-USB`.

## 2. Status

Standard 27.007 commands; all verified against the SIMCom LTE AT set.

| Purpose | Command | Response |
| --- | --- | --- |
| Alive / echo control | `AT`, `ATE0` | `OK` |
| Module identity | `ATI`, `AT+CGMM`, `AT+CGMR` (firmware), `AT+GSN` (IMEI) | text / `+CGMR: ...` |
| SIM readiness | `AT+CPIN?` | `+CPIN: READY` |
| Signal (RSSI) | `AT+CSQ` | `+CSQ: <rssi>,<ber>`; RSSI 0–31, 99=unknown; dBm ≈ `-113 + 2*rssi` |
| Signal detail (LTE) | `AT+CESQ` | `+CESQ: ...,<rsrq>,<rsrp>`; RSRP dBm = `-140 + rsrp` (0–97), RSRQ dB = `-20 + 0.5*rsrq` (0–34), 255=unknown |
| CS registration | `AT+CREG?` | `+CREG: <n>,<stat>[,...]`; stat 1=home, 5=roaming, 2=searching, 3=denied |
| LTE/EPS registration | `AT+CEREG?` | same shape as `+CREG`; prefer this on LTE |
| PS attach | `AT+CGATT?` | `+CGATT: 1` |
| Operator | `AT+COPS?` | `+COPS: <mode>,<format>,"<name>",<act>` |
| RAT preference | `AT+CNMP?` | 2=auto, 13=GSM only, 38=LTE only (module-specific) |
| Network clock | `AT+CCLK?` | wall clock from network (useful for NTP-less time) |
| Battery/board voltage | `AT+CBC` | ADC is routed to the modem on this board |

Practical "healthy" gate before SMS work: `+CPIN: READY`, `+CEREG: 0,1` (or
`,5`), `AT+CSQ` rssi > 12 (≈ -89 dBm), `AT+CGATT?: 1` for PS services.

## 3. Receiving SMS

Setup:

```text
AT+CMGF=1              # text mode (PDU mode 0 avoids unless needed)
AT+CSCS="UCS2"         # or "GSM"; UCS2 required for Cyrillic payloads
AT+CPMS="ME","ME","ME" # storage: ME (flash, large) or SM (SIM, small)
AT+CNMI=2,1,0,0,0      # mode 2 + mt 1: store incoming SMS in <mem3>,
                       # notify with +CMTI
```

Two notification styles via `AT+CNMI`:

- `<mt>=1` (recommended): message is stored in `<mem3>` (third `AT+CPMS`
  argument), URC arrives as `+CMTI: "ME",<index>`. Read with `AT+CMGR=<index>`
  or list/delete later. This matches the project's poll-forward-delete cycle.
- `<mt>=2`: message delivered directly as `+CMT: "<oa>","",<timestamp>\r\n<text>`;
  nothing is stored. Requires immediate parsing and risks loss if UART stalls;
  combined with `<bfr>=1` buffered URCs after reconnect.

Reading one message:

```text
AT+CMGR=4
+CMGR: "REC UNREAD","+380XXXXXXXXX","","yy/MM/dd,hh:mm:ss+zz",145,...
<message text>
OK
```

Statuses seen in storage listings/reads: `REC UNREAD`, `REC READ`,
`STO UNSENT`, `STO SENT`, plus `ALL`. With `AT+CSDH=1` extra header parameters
are shown; concatenated parts expose part metadata in the header fields
(SIMCom reports sequence info; treat each part as its own stored record until
reassembly, mirroring the existing ZTE partial-message handling).

Character sets matter: `GSM` (default 7-bit alphabet) has no Cyrillic. For
Russian texts set `AT+CSCS="UCS2"` and hex-encode both the sender field in
reads and the payload; a UCS2 SMS carries max 70 characters per part (vs 160
for GSM-7).

## 4. Sending SMS

Text-mode send:

```text
AT+CMGF=1
AT+CSCS="UCS2"                 # match payload encoding; "GSM" for Latin
AT+CMGS="+380XXXXXXXXX"        # in UCS2 mode the address is hex-encoded too
> message text<Ctrl+Z>         # 0x1A submits; ESC cancels
+CMGS: <mr>
OK
```

Notes:

- The modem replies with the `>` prompt after `<CR>`; submit ends with
  `<Ctrl-Z>` (0x1A). Timeout waiting for `+CMGS:` must exceed network latency
  (seconds, not milliseconds).
- Failure surfaces as `+CMS ERROR: <err>` (verbose with `AT+CMEE=2`). Treat
  `+CMGS:` + `OK` as the terminal success signature; only clean up state after
  a terminal outcome (same contract as the ZTE outgoing path).
- Length limits per single submission: 160 chars GSM-7 / 70 chars UCS2.
  Multi-part concatenation needs explicit UDH/PDU handling or repeated short
  sends; text mode does not segment automatically.
- Related storage commands: `AT+CMGW` (write draft, returns index),
  `AT+CMSS=<index>[,"<da>"]` (send from storage), `AT+CMGSEX` (send with
  extra options).

## 5. Stored SMS management

```text
AT+CPMS?                    # usage/capacity: +CPMS: "ME",<used>,<total>,...
AT+CPMS="SM","SM","SM"      # switch storage (ME/MT/SM/SR supported sets)
AT+CMGL="ALL"               # list: +CMGL: <index>,<stat>,"<addr>",...,<text>
AT+CMGL="REC UNREAD"        # filter by status
AT+CMGRD=<index>            # read AND delete in one step (no status change)
AT+CMGD=<index>             # delete one
AT+CMGD=,4                  # delflag bulk forms: 1=read, 2=read+sent,
                            # 3=read+sent+unsent, 4=all (index omitted)
```

Storage sizing: `SM` is SIM-dependent (typically tens of slots); `ME` lives in
modem flash and is larger. Polling pattern that fits the existing design:
select storage -> `AT+CMGL="ALL"` (or `REC UNREAD`) -> process oldest ->
forward -> `AT+CMGD=<index>` only after confirmed forwarding (the ZTE cycle's
delete-after-SMTP-acceptance contract maps directly).

## 6. Firmware caveats specific to SIM7670G

- From LilyGo `SendSMS.ino` and maintainer replies (issues #422, #441):
  SIM7670G-MNGV firmware **2374B04** supports SMS, but on LTE it relies on
  **SMS over SGs/NAS**; the carrier must support it, otherwise sends silently
  fail. This board runs **2374B03** — SMS support is unconfirmed for that
  build; test with a SIM or upgrade via Modem-USB (issue #358 documents a
  firmware-upgrade procedure). Some A7670E variants (`-LNXY-UBL`, `-LNMV`)
  have no voice/SMS at all.
- If SMS never arrives while attached to LTE, test `AT+CNMP=13` (GSM-only) as
  a diagnostic — classic circuit-switched SMS works there without SGs support.
- Incoming-URC behavior depends on factory `AT&F` defaults; always set
  `CMGF/CSCS/CPMS/CNMI` explicitly at startup instead of trusting NVS state.

## 7. Fit with the project

- Dialog logic (command sequencing, response parsing, timeouts) can follow the
  existing host-testable client pattern (`zte_client.*` / `smtp_client.*`):
  pure dialog core over an injected transport, tests in `tests/`.
- Transport would be a thin `HardwareSerial` (Serial1, pins above) binding,
  analogous to `zte_transport.h`.
- Switching the SMS source from the ZTE device to the onboard modem changes
  the polled-source architecture (ADR-0003 territory): consult/create a new
  ADR before implementing.
