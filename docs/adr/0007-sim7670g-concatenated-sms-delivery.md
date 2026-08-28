# ADR-0007: Preserve concatenated SIM7670G SMS delivery

- **Status:** Accepted
- **Date:** 2026-08-28
- **Supersedes:** ADR-0004

## Context

ADR-0004 selected the onboard SIM7670G as the second SMS source and sender, but
its text-mode send and single-message receive policy does not preserve multipart
messages. Device test 4 found that text-mode `AT+CMGS` silently truncated a
300-character Latin message to 44 delivered characters and rejected long
Cyrillic text with `+CMS ERROR`.

A receive scan must also distinguish an empty inbox from an incomplete `CMGL`
reply, retrieve bodies that `CMGL` truncates, and assemble incoming SMS-DELIVER
parts before SMTP forwarding. The implementation must preserve the existing
at-least-once inbox and SMTP-delete contract, bounded RAM, and `ME` as the
incoming-message store.

Options considered:

- **Keep text mode and reject all multi-segment messages.** Avoids PDU packing,
  but loses valid messages despite the shared 335-UTF-16-unit input limit.
- **Use PDU mode for every outbound message.** Uniform, but adds unnecessary
  PDU complexity to reliable single-segment sends.
- **Use text mode for one segment and PDU only for multipart sends.** Preserves
  the proven simple path while preventing silent multipart truncation.

## Decision

The SIM7670G remains the onboard SMS source and sender from ADR-0004. Its
receive and send protocols are replaced by the following bounded multipart
policy:

- **Receive:** scan `AT+CMGL="ALL"` to its terminal `OK` before issuing any
  `AT+CMGR`; a missing terminal response fails the poll cycle and is never an
  empty inbox. Consider only `REC UNREAD` and `REC READ` records, retrieve each
  selected body with `AT+CMGR`, and ignore outgoing `STO` records. Probe a
  selected message in PDU mode (`CMGF=0 → CMGR → CMGF=1`) to parse SMS-DELIVER
  UDH, restoring text mode on every outcome.
- **Inbound concatenation:** parse UDH as bounded `IEI + IEDL + data` entries
  and hold parts in a volatile cache keyed by the complete reference and its
  width (8 or 16 bits), sender, and total, bounded to two sets of five parts.
  Send a complete set as one SMTP email and record its SMTP `250` before
  any `CMGD`; each successful deletion is recorded immediately, so later
  polls retry only pending cleanup and never re-submit that set during the
  same boot. After 20 poll cycles, send each available part marked
  `[INCOMPLETE n/m]`, with equivalent per-fragment SMTP/delete progress.
  Sets above five parts or rejected because both cache sets are occupied are
  forwarded as one explicit incomplete part and deleted only after SMTP
  `250`, preventing a CMGL-head fragment from starving the inbox. Send at
  most one single message, complete set, or expired set per poll cycle.
- **Storage:** use `ME` as the primary store. A bounded `SM` fallback changes
  only CPMS mem1 (`AT+CPMS="SM","ME","ME"`); mem2 and mem3 remain `ME` so new
  messages continue to land there. Restore `AT+CPMS="ME","ME","ME"` after every
  poll outcome. A restore failure is logged separately and does not mask the
  original result.
- **Single-segment send:** use text mode. Send only the ASCII subset with GSM
  03.38-identical byte values raw, using `CSCS="GSM"` and DCS 0. Encode all
  Unicode and mismatched ASCII punctuation as UCS2 with DCS 8. Do not implement
  a GSM extension-table encoder.
- **Multipart send:** messages beyond 160 GSM-safe ASCII characters or 70
  UTF-16 units use UCS2 SMS-SUBMIT PDUs in `CMGF=0`. Each part has TP-UDHI and
  UDH `05 00 03 <ref> <total> <seq>`, DCS `0x08`, at most 67 UTF-16 units, and
  no surrogate pair split. The 335-unit input cap may require six outbound
  parts when pair-safe boundaries reduce five parts to 330 units; this does not
  change the five-part inbound cache bound. `AT+CMGS=<n>` receives the TPDU
  octet count excluding SCA; restore `CMGF=1` after success or failure.

The modem dialog remains host-testable through `ModemChannel`; SMS delivery
continues to use the inbox as its only durable state. ZTE behaviour, NVS, and
partition layout do not change.

## Alternatives Considered

### Text mode only

Rejected after device test 4 proved that the modem can accept a multipart text
submission yet silently truncate it in delivery.

### PDU mode for all outbound messages

Rejected because simple one-segment sends are already reliable in text mode and
need no UDH or TPDU construction.

### GSM extension-table encoder

Rejected: UCS2 for mismatched punctuation is simpler and less likely to change
the recipient-visible text.

## Consequences

- **Positive:** inbound concatenated messages are forwarded as one email when
  complete; outbound messages within the existing cap reach the peer without
  text-mode truncation; incomplete modem replies cannot be treated as an empty
  inbox.
- **Negative / trade-offs:** multipart send now requires bounded PDU/UDH
  construction; long Latin text uses UCS2 and therefore 67 units per part;
  volatile inbound assembly can be lost on restart.
- **Accepted risks:** the inbox remains at-least-once, so a restart between SMTP
  acceptance and `CMGD` can resend a message; the volatile same-boot progress
  only prevents repeated SMTP after a cleanup failure until reboot. Incomplete
  sets eventually reach SMTP as separately marked fragments; GSM extension-table
  punctuation uses UCS2 rather than GSM-7.
