# ADR-0004: Onboard SIM7670G modem as SMS source and sender (AT over Serial1)

- **Status:** Superseded
- **Date:** 2026-08-25
- **Superseded by**: ADR-0007

## Context

The gateway must forward SMS and send SMS without a host PC. ADR-0003 reuses an
external ZTE MF79RU HiLink modem over LAN (poll 15 s, inbox is the only state,
at-least-once). That source is proven but depends on a LAN device; the board
itself carries a SIMCom SIM7670G Cat-1 modem on `Serial1` that is currently
unused.

`docs/research/modem-sim7670g.md` plus `tools/modem_probe` verified on this unit (Classic revision, `LILYGO_T_SIM7670G_S3`):

- Pin map Classic: `RX=GPIO10 TX=GPIO11 PWRKEY=GPIO18 DTR=GPIO9 RESET=GPIO17 active LOW`; Standard pins silent — auto-detect was only for research.
- Module SIM7670G-MNGV Rev V1.9.05 / `AT+CGMR=2374B03`; SMS in text mode confirmed on Tele2 RU LTE (`CEREG 0,1`, `CSQ 22 / -69 dBm`, `RSRP -97`), so 2374B04 upgrade is not required for the decision.
- Status AT surface present: `AT+CPIN? +CSQ +CESQ +CREG? +CEREG? +CGATT? +COPS? +CCLK? +CPMS=? +CMGF=? +CNMI=?`, plus `ATI/CGMM/CGMR/GSN`.
- SMS surface: `AT+CMGF=(0-1) +CNMI=(0,1,2) +CPMS=("ME","SM")`, storage `ME` (flash, large) / `SM` (SIM, tens), `+CMTI` store-notify vs `+CMT` direct-deliver.
- Boot URCs `QCRDY`, `+CPIN`, `$QCSTKURC`, `SMS DONE/PB DONE`; healthy gate `CPIN READY + CEREG 1/5 + CSQ>12 + CGATT 1`.
- Power: ≥2 A peaks, battery BMS only via ESP-USB; `DTR LOW = awake`.

Options considered:

- **Keep only ZTE source, ignore onboard modem.** Simplest, but leaves the
  product dependent on a second box and its LAN reachability; credentials split
  across two systems.
- **Use onboard SIM7670G as second SMS source/sender over AT (`Serial1`),
  alongside ZTE.** One device owns all SMS, no host process; ZTE remains as
  proven fallback. Requires AT bring-up, polling, and send sequencing.
- **Generic multi-source abstraction now (pluggable SMS provider interface).**
  Second source does not yet forward, so the abstraction would be speculative;
  YAGNI until a third consumer needs it.
- **PDU-mode only / add ArduinoJson for AT parsing.** PDU adds UDH complexity
  without benefit for 160/70-char text mode; ArduinoJson is not bundled and is
  strict while the modem emits plain `+CMxx` lines — a hand-rolled line parser
  suffices.

## Decision

Use the onboard SIM7670G as a second SMS source and sender over `Serial1` (Classic pin map), host-testable and parallel to the ZTE source:

- **Bring-up:** `PWRKEY LOW 100 ms → HIGH 100 ms → LOW`, poll `AT` to `OK`
  (~10×200 ms, up to 3 s `MODEM_START_WAIT`), `ATE0 ATV1 AT+CMEE=2`, await `SMS
  DONE/PB DONE` (timeout 10 s for status, 100 s reference for SMS work) before
  SIM-dependent commands.
- **Status (step 1):** `AT+CPIN? → AT+CSQ → AT+CESQ → AT+CEREG? → AT+CREG? →
  AT+CGATT? → AT+COPS? → AT+CPMS? → AT+CCLK?` (+ `ATI/CGMR/GSN` once). `RSSI dBm
  = -113+2*rssi` (0-31, 99 unknown), `RSRP dBm = -140+rsrp` (0-97, 255 unknown).
  Published as `ModemStatus` via `portMUX` cache, `GET /api/modem/status`
  (Digest-protected after setup), `event=modem_status` Serial.
- **Receive (step 2):** `AT+CMGF=1 AT+CSCS="UCS2" AT+CPMS="ME","ME","ME"
  AT+CNMI=2,1,0,0,0` — store + `+CMTI: "ME",<idx>` notify (not direct `+CMT`).
  Poll `AT+CMGL="REC UNREAD"` (or `AT+CMGR` per idx), one oldest `REC UNREAD`
  per cycle, decode UCS2-hex → UTF-8, forward via existing SMTP (`buildSmsEmail`
  shared with ZTE), delete `AT+CMGD=<idx>` (or `AT+CMGRD`) only after SMTP `250`
  acceptance. Inbox is the only state (at-least-once, like ZTE). `ME` primary;
  `SM` queried optionally via `AT+CPMS?` for the counter. Interval 15 s, one SMS
  per cycle, bounded `kModemScratch 2 KB`.
- **Send (step 3):** `AT+CMGF=1 AT+CSCS="UCS2" → AT+CMGS="<number>" → > prompt →
  UCS2-hex text + 0x1A (Ctrl-Z) → +CMGS: <mr> / +CMS ERROR` as terminal outcome.
  Timeout seconds (network latency), verbose `+CMS ERROR` via `CMEE=2`. Limits
  per submission 160 GSM-7 / 70 UCS2; concatenated parts handled as separate
  records until reassembly (mirroring ZTE partial handling). `AT+CMGW`/`AT+CMSS`
  deferred.
- **Module split (GRACE):** `modem_client.*` pure dialog over abstract
  `ModemChannel` (host-testable, line parser, stable
  `ModemResult`/`failedStage`), `modem_transport.h` thin `HardwareSerial`
  binding (owns `Serial1`, `DTR`, `RESET`, timeouts). `sms_gate.ino` owns
  lifecycle: one pinned modem task + mutex/queue (like `zte_poll`/`zte_send`
  mutual exclusion), HTTP handlers only snapshot the cache or enqueue `to/text`.
  History: `codec.h` helpers reused.
- **Coexistence:** ZTE and SIM7670G run in parallel, sharing SMTP delivery; no
  persistent per-modem queue, NVS unchanged (`partitions.csv` untouched). First
  increment (this PR) implements only status + ADR; receive/send follow without
  a new ADR.

USB recovery contract (`appcfg` erase at `0x610000 0x6000`) unchanged.

## Alternatives Considered

### Keep only ZTE source

Proven but requires a separate powered modem and LAN path; defeats the self-contained gateway goal and splits the SMS trust domain.

### Generic SMS-source abstraction now

Would add an interface (`SmsSource::poll/send/status`) before the second source forwards. Rejected as speculative: only two concrete sources exist, and ZTE/SIM7670G have different transports (HTTP vs AT) and lifecycle needs; defer until a third consumer or UI needs polymorphism.

### PDU-mode or strict JSON/AT parser library

Rejected: PDU requires UDH/hex packing for every send; text+UCS2 covers 335-unit (5×70) user limit with simple hex. No third-party parser needed; AT replies are fixed `+CMD: ...` lines.

### Add ArduinoJson / external AT library

Violates bundled-libraries-only rule without approved need; strict parser would reject modem's line shapes. Hand-rolled `+` line scanner keeps host-testability with zero dependency.

## Consequences

- **Positive:** gateway owns SMS end-to-end without a host; status is visible in UI/Serial alongside ZTE; protocol logic is host-testable via scripted `ModemChannel` (like `zte_client_test.cpp`); memory bounded (page-like `CMGL` + 2 KB scratch, not 100-msg dump); polling mirrors proven ZTE pattern so SMTP-forward-delete contract is reused; no NVS/partition change.
- **Negative / trade-offs:** AT runs over a single `Serial1` shared by all modem operations — mutual exclusion required; power peaks can brown out (`present=false` + `event=modem_error`); SMS over LTE needs carrier SGs/NAS (Tele2 confirmed); `ME` wear vs `SM` size must be balanced per deployment.
- **Accepted risks:** at-least-once (crash between SMTP accept and verified `CMGD` can resend one SMS); `AT+CMTI` vs `+CMT` misconfig can lose messages if direct-deliver is chosen (we use store+notify); `UCS2` 70-char limit and concatenation reassembly are per-part until generic handling lands; firmware `2374B03` vs `2374B04` SMS-over-SGs behavior varies by carrier — mitigation is diagnostic `AT+CNMP=13` (GSM-only) and the existing probe (`tools/modem_probe`).
