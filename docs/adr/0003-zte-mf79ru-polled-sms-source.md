# ADR-0003: Poll the ZTE MF79RU goform API as an in-firmware SMS source

- **Status:** Accepted
- **Date:** 2026-08-24

## Context

The gateway must forward SMS from two sources: the board's own SIM7670G
(unimplemented) and an existing ZTE MF79RU HiLink modem reachable over the
LAN. A proven Python forwarder (`zte-mf79-ru-sms-forwarder`) already talks to
that modem: it polls the modem's own web API (old goform endpoints,
`BD_MF79RUV1.0.0B02`), forwards one oldest incoming SMS per poll over SMTP,
then deletes exactly that message and verifies the deletion. HiLink modems
have no push or webhook channel, so polling is the only option.

Options considered:

- **Keep the external Python forwarder and only build the onboard-modem
  path.** Leaves the device incomplete: it depends on an always-on host PC,
  duplicates SMTP credentials in two places, and the host keeps the modem's
  web password.
- **Reimplement the forwarder inside the firmware as a polled source.** One
  device owns credentials, delivery, and both SMS sources; no host process.
- **Generic multi-source abstraction now.** The second source (SIM7670G over
  AT commands) does not exist yet, so the abstraction would be speculative.

Two sub-deisions shape the implementation:

- **JSON parsing.** ArduinoJson is not part of the bundled Arduino-ESP32
  libraries, and it is a strict parser. The modem's B02 firmware emits raw
  control characters inside the `number` field (the Python forwarder parses
  with `strict=False` for exactly this reason), so a strict parser can fail
  on real traffic.
- **Delivery state.** The proven design keeps no host-side state: the modem
  inbox is the only state, giving at-least-once delivery (a crash between
  SMTP success and a verified delete can resend one message) with no
  persistent queue, NVS wear, or dedup tables.

## Decision

Implement the ZTE MF79RU as an in-firmware SMS source, mirroring the proven
Python dialog exactly: LOGIN with a base64 password, mandatory `Referer`
header and `stok` session cookie, `sms_data_total` listing, UCS-2-hex content
decoding, `AD = md5(md5(cr_version+wa_inner_version) + RD)` token for
`DELETE_SMS`, one oldest incoming SMS (tag 0 or 1) per poll cycle, SMTP
delivery first, then a single-ID delete verified by re-reading the inbox, and
one stale-session relogin per request. The modem inbox remains the only
delivery state (at-least-once).

The client follows the ADR-0002 module split: `zte_client.*` is a
host-testable dialog over an abstract `ZteChannel` (plus the lenient
fixed-shape JSON scanner and the UCS-2/UTF-8 decoder), `zte_transport.h`
binds the bundled `NetworkClient` on the device, and `codec.h` shares base64
and an in-tree MD5 (the AD token is an anti-CSRF token, not a secret; the MD5
is never used for anything security-relevant). No third-party dependency is
added.

Memory stays bounded: the Python forwarder fetches up to 500 messages in one
response, which on the ESP32 could exceed 100 KB. The firmware instead pages
through the inbox with `data_per_page=5` (a ~20 KB heap scratch buffer, at
most 21 pages for the 100-message device inbox) and auto-detects the actual
`order_by` direction from the returned IDs, so a firmware that ignores the
requested `order by id asc` still yields the oldest incoming message.

The ZTE profile (enabled flag, host, modem web password) lives in its own
checksummed record (`zte_record.h`, NVS namespace `zte` in `appcfg`), so
enabling the source or erasing recovery never touches the Wi-Fi or SMTP
records. A `/api/zte/test` route performs a non-destructive login and
capacity read before the operator enables polling, following the project's
verify-before-save philosophy; polling deletes modem SMS only after SMTP
acceptance.

The poll loop runs on its own pinned task (15 s interval, one SMS per cycle)
gated on station connectivity, an enabled ZTE record, and a configured SMTP
profile. A generic SMS-source abstraction is deferred until the SIM7670G
source exists.

## Alternatives Considered

### Keep the external Python forwarder

Proven and already running, but the product is a self-contained gateway; a
required host PC defeats it and splits credentials across two systems.

### Add ArduinoJson for response parsing

Rejected: violates the bundled-libraries rule without an approved need, and
its strict parser rejects the control characters this modem's firmware emits
inside string values.

### Host-side dedup/queue state (last-seen ID, pending set)

Rejected: the modem inbox is a working single state; host state adds NVS
wear, boot-time reconciliation, and new failure modes (state desync drops or
duplicates messages) for exactly-once semantics SMTP cannot provide anyway.

### One large `sms_data_total` fetch like the Python forwarder

Rejected: unbounded worst-case response size (~100 stored messages) against
roughly 300 KB of free heap shared with TLS; paging keeps the footprint
fixed and costs a few extra LAN requests only while a large backlog exists.

## Consequences

- **Positive:** the device forwards ZTE SMS with no host process; the
  protocol logic is host-testable against a scripted fake modem, including
  the AD token, stale-session relogin, paging, and decode edge cases; memory
  use is constant; credentials stay in the isolated `appcfg` partition and
  are never logged.
- **Negative / trade-offs:** the goform protocol is plain HTTP on the modem's
  LAN segment — the base64 login password is as readable there as in the
  modem's own web UI, so the modem must not be exposed beyond the trusted
  LAN; polling adds a periodic 1–2 request burst to the modem; forwarding
  order is oldest-first per poll, so a burst of SMS arrives one email per
  poll cycle.
- **Accepted risks:** at-least-once delivery means one duplicate email is
  possible after a crash between SMTP acceptance and the verified delete;
  the AD token algorithm and `order by` handling are verified against this
  modem's B02 firmware and may need adjustment for other ZTE firmware
  variants; paging behavior (`page`/`data_per_page` with ascending order) is
  robust by construction but only the page-0 shape is hardware-proven.
