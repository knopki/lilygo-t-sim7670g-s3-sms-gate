# ZTE MF79RU — Technical Reference and Integration Guide

Hardware-proven reference for the ZTE MF79RU HiLink modem as used by this
project. All endpoints, quirks, and limits below are verified on firmware
`BD_MF79RUV1.0.0B02` (old goform API) against live hardware. See
`../zte-mf79-ru-sms-forwarder` for the Python reference forwarder,
`CHECKLIST.md` for reproducible `curl` probes, `zte-probe/` for raw responses,
and `docs/adr/0003-zte-mf79ru-polled-sms-source.md` for design rationale.

## 1. Device overview

* **Product:** ZTE MF79RU — USB stick / portable 4G router. HiLink mode
  (no AT over USB to the host; the only host interface is an HTTP web UI).
* **Function:** LTE Cat-4 data (USB RNDIS + Wi-Fi AP), plus SMS send/receive
  via the modem's own web API. No voice; no external AT port.
* **Verified firmware in this project:** `wa_inner_version=BD_MF79RUV1.0.0B02`,
  `cr_version=""` (empty). Old goform endpoint set
  (`/goform/goform_*_cmd_process`). Build date from probe clock: Aug 2021.
* **Default LAN address:** `192.168.0.1` (also reachable as `192.168.0.1`
  over USB RNDIS when the stick is plugged in; over Wi-Fi when connected to
  its SSID). Port `80`, plain HTTP. No TLS on the modem side.
* **Storage:** Device NV (flash) inbox **100** messages, SIM inbox **15**
  (`sms_nv_total=100`, `sms_sim_total=15`). Probe `zte-probe/04-capacity.json`
  shows `sms_nvused_total=100`, `sms_sim_rev_total=0` on a full NV box.
* **Polling only:** The modem has no webhook, WebSocket, or push. New SMS
  must be discovered by polling `sms_data_total`.

## 2. Connectivity modes

| Attachment | Host interface | Modem IP | Notes |
|---|---|---|---|
| USB | RNDIS / ECM (appears as `usb0` / `enx...`) | `192.168.0.1` | Most reliable for a gateway co-located with the stick. Powers the stick. |
| Wi-Fi | Connect to the MF79RU SSID (printed on label) | `192.168.0.1` | Same API; adds Wi-Fi contention. |

Both paths expose the identical `http://192.168.0.1/` web UI and goform API.
The gateway in this project (`sms_gate/`) reaches the modem over the **LAN**
(STA interface) as a plain `NetworkClient` TCP connection.

## 3. Firmware variants — goform vs. reqproc

ZTE MF79-family ships one of two web stacks. They are mutually exclusive
(detect by inspecting browser DevTools after logging into `http://192.168.0.1/`):

| Stack | Endpoints | How to identify | This project |
|---|---|---|---|
| **goform (old)** — MF79RU `BD_MF79RUV1.0.0B02` | `/goform/goform_get_cmd_process` (GET), `/goform/goform_set_cmd_process` (POST) | Network tab shows `goform_get_cmd_process` | **Used here** |
| **reqproc (new)** — cheap ZTE-IC boards | `/reqproc/proc_get`, `/reqproc/proc_post` | Network tab shows `proc_get` | Not implemented |

Sources: SmsForwardCenter/ZxicSmsFwd (`zxic_utils.py`), research.
All curl/REST examples below are goform.
The reqproc stack is structurally similar but uses different JSON keys and
is out of scope for the tested MF79RU.

## 4. HTTP transport contract

```
Base:  http://<host>/          (default <host> = 192.168.0.1)
GET:   /goform/goform_get_cmd_process?isTest=false&cmd=<cmd>[&...]
POST:  /goform/goform_set_cmd_process   (Content-Type: application/x-www-form-urlencoded; charset=UTF-8)
```

**Mandatory headers on every request** (both GET and POST):

```
Referer: http://<host>/index.html
Cookie: stok=<value>            (after LOGIN; see §5)
Connection: close               (one request per TCP connection)
Host: <host>
```

Without `Referer` or without a valid `stok` the modem returns `HTTP 200`
with a **valid JSON shape but empty values** instead of an HTTP error:

```json
{"loginfo":""}
{"sms_data_total":""}
{"sms_capacity_info":""}
```

This is the "stale session" signature — not a transport error. Every client
must detect it and re-login once (see §5.4).

Other transport rules proven in `zte_client.cpp`:

* One `Connection: close` request per TCP connection; channel is stopped
  before every return.
* `Content-Length` is authoritative; an oversized body is a protocol error.
* `Transfer-Encoding: chunked` is never emitted by B02 and is rejected as
  `kProtocolError` if seen.
* Bodies may arrive without `Content-Length` — then read until EOF/close.
* Timeout: `10 s` (reference forwarder and `NetworkZteChannel` default).

## 5. Authentication and session

### 5.1 LOGIN

```http
POST /goform/goform_set_cmd_process HTTP/1.1
Host: 192.168.0.1
Referer: http://192.168.0.1/index.html
Content-Type: application/x-www-form-urlencoded; charset=UTF-8

isTest=false&goformId=LOGIN&password=<base64(password)>
```

* `password`: raw modem web password (label under the battery / `admin` by
  default), **base64-encoded** (standard alphabet, `=` padding).
  Example: `admin` → `YWRtaW4=`.
* Success reply:

  ```json
  {"result":"0"}
  ```

  plus `Set-Cookie: stok=XXXX; Path=/` (e.g. `stok=ABC123` in tests).
  The cookie **is the session**. It must be sent as `Cookie: stok=XXXX` on
  every subsequent request. No `stok` = not authenticated.
* Failure: `{"result":"1"}` / `"2"` / `"3"` or missing `Set-Cookie` → all are
  `kLoginRejected` (`failedStage=login` or `login_cookie`).

On the wire (`tests/zte_client_test.cpp: testLoginSuccess`):

```
POST /goform/goform_set_cmd_process HTTP/1.1
Host: 192.168.0.1
Referer: http://192.168.0.1/index.html
Content-Type: application/x-www-form-urlencoded; charset=UTF-8
Content-Length: 42
Connection: close

isTest=false&goformId=LOGIN&password=YWRtaW4=
```

### 5.2 Session check

```http
GET /goform/goform_get_cmd_process?isTest=false&cmd=loginfo HTTP/1.1
Referer: http://192.168.0.1/index.html
Cookie: stok=XXXX
```

* Authenticated: `{"loginfo":"ok"}`
* Stale / unauthenticated: `{"loginfo":""}` — trigger a re-login.

`zte-probe/01-loginfo-pre.json` (`{"loginfo":""}` before login) vs
`03-loginfo-post.json` (after) confirms this transition.

### 5.3 Firmware version (needed for AD)

Immediately after LOGIN, read the version string that seeds the AD token:

```http
GET /goform/goform_get_cmd_process?isTest=false&cmd=cr_version,wa_inner_version&multi_data=1
```

Reply on MF79RU B02:

```json
{"cr_version":"","wa_inner_version":"BD_MF79RUV1.0.0B02"}
```

Concatenation `wa_version = cr_version + wa_inner_version` → on B02 this is
just `"BD_MF79RUV1.0.0B02"`. Empty `wa_version` = stale session.

This project caches `wa_version` per session; a stale/empty value forces a
fresh `openSession()`.

### 5.4 Session lifecycle and stale handling

* A session expires when: the modem reboots, another client logs in via the
  web UI, or the modem evicts the `stok`.
* Stale signature: `200 OK` with `{"sms_data_total":""}` or
  `{"loginfo":""}` / missing `RD` / empty `wa_inner_version`.
* Contract: **one silent re-login per command**. If the first GET returns the
  stale shape, `LOGIN` + re-read-versions once, then retry the original
  command once. A second stale result is a terminal `kStaleSession`.

Implemented in `ZteModem::fetchSmsPage`, `fetchRd`, `openSession`, and
mirrored in the Python `connect_and_list()` two-attempt loop.

## 6. Anti-CSRF token AD

Every `goform_set_cmd_process` command except `LOGIN` requires `AD`:

```
AD = hex_md5( hex_md5(wa_version) + RD )
```

* `wa_version` — the cached concatenation above.
* `RD` — **fresh per command**, fetched in the current session:

  ```http
  GET /goform/goform_get_cmd_process?isTest=false&cmd=RD
  → {"RD":"9bf31c7ff062936a96d3c8bd1f8f2ff3"}
  ```

* `hex_md5` — lowercase hex MD5. Inner hash over `wa_version` ASCII bytes,
  outer hash over `inner_hex + RD` ASCII bytes.

In-tree implementation: `codec.h:md5Hex` (no external dependency; MD5 here is
an anti-CSRF token, not a security primitive). Verified vector in the test
suite:

```
wa_version = "BD_MF79RUV1.0.0B02"
RD         = "AB12CD34"
AD         = "02bb862c133c79826efbe952c8a57c34"
```

Without a correct `AD` the modem replies `{"result":"failure"}`.

The web UI's service layer (`zte-probe/webui/service.js`) computes the same
value as `hex_md5(hex_md5(rd0+rd1) + RD)` where `rd0`/`rd1` are the version
strings.

## 7. SMS storage model

### 7.1 Stores

| `mem_store` | Location | Capacity | `sms_*_total` key |
|---|---|---|---|
| `1` | Device NV (flash) | 100 | `sms_nv_total` |
| `0` | SIM | 15 | `sms_sim_total` |

The reference forwarder and this firmware both use **`mem_store=1`** (device).
SIM is empty on the probed unit (`sms_sim_rev_total=0`). Storage selection
is explicit in every `sms_data_total` call.

### 7.2 Tags

| `tag` | Meaning | Source |
|---|---|---|
| `0` | Incoming, **read** | Inbox — forwardable |
| `1` | Incoming, **unread** | Inbox — forwardable |
| `2` | Outgoing, **sent** | Device outbox — cleaned after confirmed send |
| `3` | Outgoing, **failed** | Device outbox — cleaned after failed send |
| `4` | Draft / group draft | Never cleaned by the poll/send lifecycle |

Tags `0` and `1` together are the **incoming** set (`tags=10` in the global
list, `tags=10` or filter `0`/`1` in paged reads). `sms_data_total` with
`tags=10` returns all tags; the firmware filters to `tag ∈ {0,1}` for
forwarding. Cleanup after `SEND_SMS` touches only `2`/`3`.

### 7.3 Capacity counters

```http
GET /goform/goform_get_cmd_process?isTest=false&cmd=sms_capacity_info
```

Reply (from `zte-probe/04-capacity.json`):

```json
{
  "sms_nv_total":"100","sms_sim_total":"15",
  "sms_nvused_total":"100","sms_nv_rev_total":"100",
  "sms_nv_send_total":"0","sms_nv_draftbox_total":"0",
  "sms_sim_rev_total":"0","sms_sim_send_total":"0",
  "sms_sim_draftbox_total":"0"
}
```

* `sms_nv_total` / `sms_sim_total` — total slots.
* `sms_nv_rev_total` / `sms_sim_rev_total` — **incoming** count (the reliable
  occupancy counter).
* `sms_nvused_total` — documented as total used but **stale on B02**: after
  deleting 5 oldest IDs (105–109) the list shrank to 95 and `sms_nv_rev_total`
  went to 95, but `sms_nvused_total` stayed at `100`. Do not use it as a
  fill-level trigger (see `CHECKLIST.md` Block A result).
* `sms_received_flag` — not implemented on B02 (always `""`); do not poll it.
  `sms_unread_num` only works inside `multi_data=1` together with other keys;
  as a standalone trigger it is unreliable. The only reliable poll trigger is
  a separate `cmd=sms_capacity_info` + comparison of `sms_nv_rev_total`.

The non-destructive `/api/zte/test` route in `sms_gate.ino` surfaces
`sms_nvused_total`/`sms_nv_total` as `ZteInboxStatus{used,total}` for the
operator UI.

### 7.4 Message record fields

One `messages[]` element in `sms_data_total`:

```json
{
  "id":"105",
  "number":"+70001234567",
  "content":"041F04400438043204350442",
  "tag":"1",
  "date":"26,01,02,03,04,05,+12",
  "received_all_concat_sms":"1",
  "concat_sms_total":"1",
  "concat_sms_received":"1",
  "sms_class":"1",
  "draft_group_id":""
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | decimal string | Monotone increasing; primary key. Stable `Message-ID` source. |
| `number` | string | Sender MSISDN. **May contain raw control characters** (B02 bug) — parse with `strict=False` / lenient scanner. |
| `content` | UCS-2 hex string | 4 hex chars per UTF-16 code unit (see §9.1). Non-hex fallback to raw string. |
| `tag` | `"0"`–`"4"` | See §7.2. |
| `date` | `yy,mm,dd,HH,MM,SS,+tz` | Modem clock; quarter-hour `tz` offset (see §9.2). Often wrong — never use for dedup/order. |
| `received_all_concat_sms` | `"0"` / `"1"` | `"1"` = complete. B02 exposes only the first segment of long concats. |
| `concat_sms_total` | decimal string | Total segments expected. |
| `concat_sms_received` | decimal string | Segments actually stored. |

`zte-probe/08-list-nv.json` (100 messages, all `content` UCS-2 hex) is the
canonical sample.

## 8. Endpoint reference

All paths relative to `http://<host>`.

### 8.1 GET — `goform_get_cmd_process`

| `cmd` | Params | Reply shape | Notes |
|---|---|---|---|
| `loginfo` | — | `{"loginfo":"ok"}` or `{"loginfo":""}` | Session check |
| `cr_version,wa_inner_version` | `multi_data=1` | `{"cr_version":"","wa_inner_version":"BD_MF79RUV1.0.0B02"}` | AD seed |
| `RD` | — | `{"RD":"<hex>"}` | Fresh AD nonce |
| `sms_capacity_info` | — | `{"sms_nv_total":"100", ...}` (see §7.3) | Capacity |
| `sms_data_total` | `page`, `data_per_page`, `mem_store`, `tags`, `order_by` | `{"messages":[{...}]}` or `{"sms_data_total":""}` (stale) | List SMS |
| `sms_cmd_status_info` | `sms_cmd=1` or `4` | `{"sms_cmd_status_result":"1"/"2"/"3"}` or `{}` | Send/status polling (see §11.2) |
| `sms_parameter_info` | — | `{"MessageCenter":"+7912..."}` | SMSC (probe: `smscfg` command) |
| `sntp_*`, `network_type`, `rssi`, `opms_wan_mode` etc. | various | Misc modem state | Not needed for SMS |

**`sms_data_total` query** (the only paginated endpoint):

```
isTest=false
&cmd=sms_data_total
&page=<uint>                 # 0-based
&data_per_page=<uint>        # reference forwarder uses 500; firmware uses 5
&mem_store=1                 # 1=device, 0=SIM
&tags=10                     # 10=all, or a single tag "0"/"1"/"2"/"3"/"4"
&order_by=order+by+id+asc    # requested; B02 ignores it (see §12)
```

On stale session: returns `{"sms_data_total":""}` (no `messages` array) →
re-login and retry once.

### 8.2 POST — `goform_set_cmd_process`

All POST bodies are `application/x-www-form-urlencoded`.

| `goformId` | Required fields | Reply | Notes |
|---|---|---|---|
| `LOGIN` | `password` (base64) | `{"result":"0"}` + `Set-Cookie: stok=...` | §5.1 |
| `DELETE_SMS` | `msg_id` (`id;` or `id1;id2;`), `AD`, `notCallback=true` | `{"result":"success"}` or `"failure"` | §10.3 |
| `SET_MSG_READ` | `msg_id` (`id;`), `tag=0`, `AD` | `{"result":"success"}` | Marks `1→0`; requires AD; verified in CHECKLIST.md |
| `SEND_SMS` | `Number`, `sms_time`, `MessageBody` (UCS-2 hex), `ID=-1`, `encode_type`, `AD`, `notCallback=true` | `{"result":"success"}` or empty `200` (malformed) | §11 |
| `REBOOT_DEVICE` | `AD` | `{"result":"success"}` | Resets the modem (probe: `reboot` command) |
| `SET_MESSAGE_CENTER` | `MessageCenter`, `save_time`, `status_save`, `save_location`, `AD` | `{"result":"success"}` | Sets SMSC |

**`msg_id` format:** IDs joined by `;`, **trailing `;` required**:
`msg_id=105;` or `msg_id=105;106;107;`. `AD` is fresh per call.

## 9. Data encoding

### 9.1 UCS-2 hex (`content` / `MessageBody`)

* Every code unit is **4 uppercase hex chars** (UCS-2 / UTF-16BE).
* BMP characters: one unit. Astral (U+10000+): **surrogate pair** — two units
  (`D83D DE00` for U+1F600).
* Decoding: `chr(int(hex[i:i+4],16))` per 4-char chunk, join surrogate pairs,
  replace unpaired surrogates with `U+FFFD`, emit UTF-8. Encoding is the
  reverse. Implemented in `zte_client.cpp: encodeUcs2Hex` / `decodeUcs2HexView`
  and Python `decode_content()`.
* Validation: length must be `%4==0` and all hex digits; otherwise treat as
  raw string fallback (the Python forwarder's `decode_content` branch).
* JSON: B02 emits `content` as a JSON string that may contain a **raw control
  byte inside `number`** (e.g. `"\x01"` not `\u0001`). A strict JSON parser
  rejects it. The firmware uses a **lenient scanner** (`skipString` accepts
  raw controls), the Python forwarder uses `json.loads(..., strict=False)`.

Example:

```
"test"    → 0074006500730074
"Привет"  → 041F04400438043204350442
"😀"      → D83DDE00
```

### 9.2 Timestamp (`date` / `sms_time`)

Modem format: `yy,mm,dd,HH,MM,SS,+tz` where `tz` is **quarters of an hour**
from UTC:

```
"26,01,02,03,04,05,+12"  → 2026-01-02 03:04:05 UTC+03:00   (+12 quarters = +3h)
"25,12,31,23,59,59,+5"   → 2025-12-31 23:59:59 UTC+01:15  (+5 quarters = +1h15m)
"26,08,24,13,48,05,-8"   → 2026-08-24 13:48:05 UTC-02:00
```

`formatZteDate()` in `zte_client.cpp` renders this as
`20yy-mm-dd HH:MM:SS UTC±HH:MM`; out-of-range or trailing-garbage shapes
pass through verbatim. The clock itself is often wrong (probe shows 2024
mixed with 2026) — **never use it for ordering or dedup**; use `id`.

For `SEND_SMS` the modem validates `sms_time` against its own SNTP clock.
`sms_time` shape is `yy;mm;dd;HH;MM;SS;+H` with **unpadded hour** (`+0`…`+12`,
not `+00`):

```
sms_time = "26;01;02;03;04;05;+0"     # ';' separators, '+' hour
```

The modem's web UI builds it from local time; the firmware here uses
`time(nullptr)` (NTP-synced via `pool.ntp.org`/`time.nist.gov`) or falls back
to `"00;01;01;00;00;00;+0"` (epoch placeholder) before sync.

### 9.3 JSON quirks

* Lenient: control characters inside strings, as above.
* Empty stale values are strings (`""`), not missing keys.
* Numbers (counters, `id`) are **strings** (`"100"`, not `100`) — parse as
  decimal string.
* `Content-Length` may be absent; fallback is read-until-close.

## 10. Polling and delivery model

The modem is the **only delivery state**. The host keeps no queue, no
`last_seen_id`, no dedup table. This gives **at-least-once** delivery.

### 10.1 One oldest incoming per poll

Each poll cycle:

1. `LOGIN` (if no session) + read `cr_version`/`wa_inner_version`.
2. **Scan** the inbox for the oldest incoming (`tag 0` or `1`) — see §10.2.
3. If none → idle (`"no incoming SMS"`), publish status, sleep.
4. If found → decode `content` (UCS-2 hex → UTF-8), build email:

   ```
   Subject: "SMS from <sender>"  or "[INCOMPLETE r/t] SMS from <sender>"
   Message-ID: <zte-sms-<id>@<modem_host>>    (stable per id → mail dedup)
   Body: Sender / Received on (label) / Modem message ID / Modem date / text
   ```

5. `SMTP send` → if SMTP fails, **keep the SMS** (poll will retry it).
6. `DELETE_SMS` for **that one `id`** with fresh `AD` → then **verify**
   the `id` disappeared by re-reading the inbox.
7. If verification fails → report `delete_unverified`; the next poll will
   re-send the same SMS (at-least-once).

`DELETE_AFTER_FORWARD=true` is mandatory. Without it the same `id` would
resend every poll. If the process crashes between SMTP success and verified
delete, the next poll re-sends one duplicate email — the stable `Message-ID`
gives the mail system a chance to deduplicate.

The Python reference does `fetch 500 at once, sort by id ascending, pick
lowest 0/1`. The firmware cannot allocate 500× messages, so it **pages** (next
section) while preserving the same oldest-first selection.

### 10.2 Paging and order auto-detection

Firmware constants (`zte_client.h`):

```
kZtePageSize = 5
kZteMaxPages = 21          // 21×5 = 105 ≥ 100 (full NV box)
scratch    = 20 KB         // bounded heap footprint
```

Loop `page=0..20` with:

```
GET cmd=sms_data_total&page=<p>&data_per_page=5&mem_store=1&tags=10&order_by=order+by+id+asc
```

*Each page result is scanned for `tag 0`/`1`; the candidate is the oldest
among observed entries.*

**Why auto-detection:** On B02 the `order_by=order by id asc` parameter is
**ignored** — the modem returns pages in **descending** order regardless.
The scanner detects the effective order from the first two IDs:

```
ascending  → first page's lowest id is already the oldest → return early
descending → must scan to the final page to find the oldest → full walk
```

This is implemented in `ZteModem::scanOldest` and proven by
`testFindOldestDescending` / `testFindOldestAscending`. Unknown firmware
variants that honour `asc` benefit from the early-exit; B02 pays the extra
pages only while a large backlog exists (LAN RTT, not SMS latency).

For the outgoing cleanup path (`DELETE_SMS` for `tag 2`/`3`), paging is not
needed: B02 ignores `page` and the tag filter makes `page=0` a bounded work
queue — after each verified delete the next matching record shifts into it
(see `ZteModem::findOutgoing`).

### 10.3 Delete and verification

```
POST goform_set_cmd_process
isTest=false&goformId=DELETE_SMS&msg_id=<id>%3B&notCallback=true&AD=<AD>
```

* `msg_id` percent-encoded `;` as `%3B`; one or batch (`id1;id2;`) with
  trailing `;` even for a single ID. Reference probe confirms batch delete
  of 5 oldest IDs with one call removed them.
* Success: `{"result":"success"}`. Then the client **re-reads**
  `sms_data_total` (`tags=10` for incoming, tag-filtered `2`/`3` for outgoing)
  and asserts the target `id` is absent. If present → `kProtocolError`
  `delete_unverified` (the poll cycle reports it and retries deletion next
  cycle).
* Failure (`{"result":"failure"}` or non-`success`) → `kProtocolError`
  `delete` — the message is retained for the next poll.

### 10.4 Concatenated (long) SMS

* When a sender transmits a long message (> 70 UCS-2 chars or > 160 GSM-7),
  the carrier splits it into segments. The MF79RU **stores only the first
  segment** and marks:

  ```json
  "received_all_concat_sms":"0",
  "concat_sms_total":"5",
  "concat_sms_received":"1"
  ```

* The gateway **forwards the available fragment immediately** with
  `[INCOMPLETE 1/5]` in the subject and a `WARNING: modem received only 1/5
  parts` prefix in the body, then deletes the fragment after SMTP success.
  Later segments, if they arrive as separate modem messages, are forwarded
  independently (same `id` space — each segment is its own record).

This matches the Python `incomplete` branch and the firmware's
`ZteSms.concatComplete` flag.

## 11. Sending SMS

### 11.1 Request shape

Proven by the modem's own web UI capture and ZxicSmsFwd; mirrored exactly in
`ZteModem::sendSms`:

```
POST /goform/goform_set_cmd_process
isTest=false&goformId=SEND_SMS&notCallback=true
&Number=<tel>                         # percent-escaped, plain ASCII
&sms_time=<yy;mm;dd;HH;MM;SS;+H>      # percent-escaped, see §9.2
&MessageBody=<UCS2-hex>               # percent-escaped
&ID=-1
&encode_type=GSM7_default | UNICODE   # ASCII → GSM7_default, else UNICODE
&AD=<AD>                              # percent-escaped
```

* `Number`: `3–20` chars, optional leading `+`, rest digits. Percent-escaped
  (`+` → `%2B`). The body params after `Number` include `sms_time` with
  `+` → `%2B` and `%3B` for `;`.
* `MessageBody`: `UCS-2 hex` encoding of the UTF-8 text (see §9.1), then
  form-escaped (hex chars are unreserved, so no escaping in practice, but
  the code path is the same as `Number`).
* `encode_type`: the web UI sends `GSM7_default` for printable-ASCII-only
  text and `UNICODE` otherwise; **the body is still UCS-2 hex in both
  cases** (browser capture for `"test"` → `0074006500730074`). The firmware
  mirrors this label selection; the transport accepts either.
* `ID=-1` (new message).
* `AD`: fresh `RD`-derived token (see §6) — a missing or stale `AD` → the
  modem replies `{"result":"failure"}` or `200` with empty body.

Size limit enforced before any network call:

```
kMaxZteSmsSendUnits = 335   // 5 × 70 minus segment headers (UNICODE limit)
```

Counted as **UTF-16 code units** (`zteSmsUtf16Units`): BMP = 1, astral = 2.
Empty text, malformed UTF-8, or `units > 335` → `kProtocolError` `send_input`
without touching the modem. Printable-ASCII control bytes are rejected for
`Number`.

Request construction is validated in `tests/zte_client_test.cpp`:

```
Number=%2B79990000000&sms_time=26%3B01%3B02%3B03%3B04%3B05%3B%2B0
&MessageBody=041F...&ID=-1&encode_type=UNICODE&AD=02bb862c...
```

Precise tail check: the form ends exactly with `&AD=<32 hex>` — no trailing
bytes (a past bug leaked stack bytes beyond `AD` into `Content-Length`).

### 11.2 Asynchronous result

`SEND_SMS` success (`{"result":"success"}`) means **accepted**, not delivered.
Delivery is polled via:

```http
GET /goform/goform_get_cmd_process?isTest=false&cmd=sms_cmd_status_info&sms_cmd=4
```

Sample values:

| `sms_cmd_status_result` | Meaning |
|---|---|
| `"1"` / absent / `{}` | In progress (keep polling) |
| `"3"` | **Done** — modem accepted for dispatch |
| `"2"` | **Failed** — radio/SMSC error |

The firmware polls one sample per second up to `20` attempts
(`kZteSendStatusAttempts=20`, `kZteSendStatusDelayMs=1000`). The reference
Python `probe.py` (`send`, `sendraw`, `sendtz`, `sendfix`) documents the
browser's XHR + `sms_cmd_status_info` polling loop as well.

Special cases:

* Some malformed `SEND_SMS` forms are answered with **`HTTP 200 and empty body`**
  (no JSON) — classified as `kSendRejected` `send_reply_empty` so logs name
  the shape explicitly.
* A `{"result":"failure"}` body → `kSendRejected` `send`.

After a terminal status (`Done` or `Failed`), the firmware runs
`cleanupOutgoing()` — verified deletion of all remaining `tag 2`/`3` records
(see §10.3 batch semantics), retrying `delete_unverified` with 500 ms gaps
up to `kZteMaxPages * kZtePageSize` attempts.

## 12. Firmware quirks and limitations

| Quirk | Detail | Impact |
|---|---|---|
| **`Referer` required** | Missing → empty values with 200 | Silent auth failure; must always send `Referer: http://<host>/index.html` |
| **`stok` required** | Missing → empty values with 200 | One-relogin-per-command contract |
| **Raw control in `number`** | B02 emits e.g. `"\x01"` inside a JSON string | Strict parsers (`JSON.parse`, ArduinoJson) reject; lenient scanner required |
| **`order_by` ignored** | `order by id asc` always returns descending | Auto-detect ordering; do not assume asc |
| **`page` ignored (B02)** | Deleting shifts next match into `page=0` | Use page-zero work-queue for cleanup |
| **`sms_received_flag` empty** | Always `""` on B02 | Never use as new-SMS trigger |
| **`sms_nvused_total` stale** | Stays `100` after deletes | Use `sms_nv_rev_total` |
| **`sms_data_total` without `messages`** | `{"sms_data_total":""}` = stale | Re-login |
| **Empty 200 on bad SEND_SMS** | `200` with zero bytes | Treat as `send_reply_empty` |
| **Only first concat segment** | Long concats truncated | Forward incomplete + warning |
| **Clock wrong** | `date` mixes years | Never use for ordering |
| **100-message NV limit** | Oldest messages dropped on overflow | Regular polling prevents loss |
| **Plain HTTP only** | No TLS on modem | LAN must be trusted; see §14 |

## 13. Integration in this project

### 13.1 Module map

| File | Role |
|---|---|
| `sms_gate/zte_client.h/.cpp` | **Host-testable dialog**: LOGIN, paging, AD, UCS-2/UTF-8, date formatting, delete verification, send + status. Depends only on `codec.h` + `zte_record.h`. See `MODULE_CONTRACT` region. |
| `sms_gate/zte_transport.h` | `NetworkClient` binding: TCP connect with deadline, CRLF line reads, bounded `recv()` (not `available()` — avoids lwIP `select` bug on core 3.x), `SO_RCVTIMEO`. |
| `sms_gate/zte_record.h` | Checksummed NVS record: `magic=0x5A544547 ("ZTEG")`, `version=2` (migrates v1), `enabled`, `host[64]`, `password[64]`, `label[32]` (phone/alias for email header). FNV-1a checksum. |
| `sms_gate/config_store.*` | NVS namespace `zte` in `appcfg` partition (`partitions.csv: 0x610000/0x6000`). Isolated from Wi-Fi/SMTP records; erasing `appcfg` via bootloader recovery clears it. |
| `sms_gate/sms_gate.ino` | Wiring: poll task (15 s, one SMS per cycle), `/api/zte/test` (non-destructive login + capacity), `/api/zte/send` (recipient `3–20` digits + `335` units), status polling, `cleanupOutgoing`. Gated on `STA connected && ZTE enabled && SMTP configured`. |
| `tests/zte_client_test.cpp` | 24 host tests with a scripted `FakeZteChannel` (see §13.3). Build: `c++ -std=c++17 tests/zte_client_test.cpp sms_gate/zte_client.cpp -o /tmp/zte_client_test`. |
| `tools/modem_probe`, `tools/gps_probe` | Not ZTE-specific (SIM7670G board probes), but the pattern for hardware verification. |

### 13.2 Runtime flow (`sms_gate.ino`)

```
Boot:
  load ZteConfigRecord (validate magic/version/checksum, migrate v1)
  if enabled && SMTP present && STA connected → spawn ztePollTask (pinned, 16 KB stack)

Poll task (kZtePollIntervalMs = 15 s):
  wait for STA, sleep while zteTestRunning/zteSendRunning
  runZtePollCycle():
    login → findOldestIncoming (paged, auto-detect) → forwardZteSms (SMTP) → deleteSms (AD+verify)
  publishZteStatus() + Serial event=zte_poll_complete

On-demand tasks (excluded from poll):
  zteTestTask:  LOGIN + readInboxStatus  (non-destructive, for UI "Test connection")
  zteSendTask:  LOGIN + sendSms + readSendStatus×20 (1 Hz) + cleanupOutgoing
```

Events logged on `Serial` (structured `event=<name> key=value`):

```
event=zte_poll_begin host=192.168.0.1 heap=...
event=zte_forward_begin id=105 number=+7000... heap=...
event=zte_poll_complete result=forwarded id=105
event=zte_delete_failed id=105 stage=delete_unverified result=protocol_error
event=zte_send_begin to=+7999... units=12 epoch=... heap=...
event=zte_send_form ...          # exact form body for byte-level diagnosis
event=zte_outgoing_cleanup_retry attempt=1 deleted=0 delay_ms=500
```

Credentials never appear in logs or HTTP responses.

### 13.3 Host tests (contract enforcement)

`tests/zte_client_test.cpp` scripts a `FakeZteChannel` byte-by-byte and
asserts the exact wire contract:

* `testLoginSuccess` — base64 password, `Referer`/`Host` headers, `stok`
  capture, version fetch.
* `testFindOldestAscending/Descending/ContinuesPages/Empty` — paging +
  auto-detection.
* `testIncompleteConcatAndDecode`, `testControlCharactersInNumber`,
  `testNonHexContentFallback` — decoding + lenient parsing.
* `testStaleSessionRelogin` — one re-login path.
* `testDeleteFlow/Rejected/Unverified`, `testCleanupOutgoing` — AD +
  verification, tag `2`/`3` only.
* `testSendSmsFlow/AsciiUsesGsm7/EmptyReply/Rejected/InputValidation`,
  `testSendStatusSamples`, `testZteSmsUtf16Units`, `testFormatZteDate` — send
  shape, label selection, time format, `AD` tail, status mapping, length rule.

## 14. Security notes

* **Transport is plain HTTP** on the modem's LAN. `password` (base64) and
  `stok` are readable to anyone on that segment — the same exposure as the
  modem's own web UI. **Do not expose the modem beyond the trusted LAN/VLAN.**
  The project's `NetworkClient` path is intentionally plain; there is no modem
  TLS to negotiate.
* The goform `sms_data_total` family has a history of SQL injection via the
  `order_by` parameter on some ZTE firmware (Reversec, 2023). The modem must
  not be reachable from untrusted networks. This project only issues fixed,
  known-safe values for `order_by`.
* `AD` is an anti-CSRF token, not a secret — its MD5 is not used for any
  integrity/security purpose. It is computed in-tree (`codec.h`) so no crypto
  library is added.
* NVS (`appcfg`) is **not encrypted**; credentials at rest are as exposed as
  the device's flash. Recovery is by erasing `appcfg` via the USB bootloader
  (`erase 0x610000 0x6000`), not `erase-flash`.

## 15. Curl quick reference

All examples from `CHECKLIST.md` Block A/B. Set `M=http://192.168.0.1`,
`JAR=/tmp/zte.jar`, `REF="Referer: $M/index.html"`, `PW='...'` (or empty if
no web password is set).

```bash
# 1. LOGIN (observe Set-Cookie)
curl -si -c "$JAR" -H "$REF" \
  -H 'Content-Type: application/x-www-form-urlencoded; charset=UTF-8' \
  --data "isTest=false&goformId=LOGIN&password=$(echo -n "$PW" | base64)" \
  "$M/goform/goform_set_cmd_process"
# → {"result":"0"}  and  Set-Cookie: stok=...

# 2. Check session
curl -sG -b "$JAR" -H "$REF" "$M/goform/goform_get_cmd_process" \
  --data-urlencode 'isTest=false' --data-urlencode 'cmd=loginfo'
# → {"loginfo":"ok"}

# 3. Firmware versions (AD seed) + fresh RD
curl -sG -b "$JAR" -H "$REF" "$M/goform/goform_get_cmd_process" \
  --data-urlencode 'isTest=false' --data-urlencode 'multi_data=1' \
  --data-urlencode 'cmd=cr_version,wa_inner_version,RD'

# 4. Capacity
curl -sG -b "$JAR" -H "$REF" "$M/goform/goform_get_cmd_process" \
  --data-urlencode 'isTest=false' --data-urlencode 'cmd=sms_capacity_info'

# 5. List inbox (device, all tags, one page)
curl -sG -b "$JAR" -H "$REF" "$M/goform/goform_get_cmd_process" \
  --data-urlencode 'isTest=false' --data-urlencode 'cmd=sms_data_total' \
  --data-urlencode 'page=0' --data-urlencode 'data_per_page=5' \
  --data-urlencode 'mem_store=1' --data-urlencode 'tags=10' \
  --data-urlencode 'order_by=order by id asc'

# 6. Compute AD (outer example — paste wa_inner_version + RD)
python3 -c "from hashlib import md5; w='BD_MF79RUV1.0.0B02'; r='9bf31c7ff062936a96d3c8bd1f8f2ff3'; print(md5((md5(w.encode()).hexdigest()+r).encode()).hexdigest())"

# 7. Delete one SMS (verified)
curl -s -b "$JAR" -H "$REF" \
  -H 'Content-Type: application/x-www-form-urlencoded; charset=UTF-8' \
  --data "isTest=false&goformId=DELETE_SMS&msg_id=105%3B&notCallback=true&AD=<AD>" \
  "$M/goform/goform_set_cmd_process"
# → {"result":"success"}  — then re-read #5 to confirm id absence

# 8. Send SMS (UCS-2 hex body, +3 offset like a browser)
python3 -c "print(''.join(format(ord(c),'04X') for c in 'Hello'))"
# → 00480065006C006C006F
curl -s -b "$JAR" -H "$REF" \
  -H 'Content-Type: application/x-www-form-urlencoded; charset=UTF-8' \
  --data "isTest=false&goformId=SEND_SMS&notCallback=true&Number=%2B79120000000&sms_time=26%3B01%3B02%3B03%3B04%3B05%3B%2B3&MessageBody=00480065006C006C006F&ID=-1&encode_type=UNICODE&AD=<AD>" \
  "$M/goform/goform_set_cmd_process"
# → {"result":"success"} then poll sms_cmd_status_info (sms_cmd=4) until "3" or "2"

# 9. Poll send status
curl -sG -b "$JAR" -H "$REF" "$M/goform/goform_get_cmd_process" \
  --data-urlencode 'isTest=false' --data-urlencode 'cmd=sms_cmd_status_info' \
  --data-urlencode 'sms_cmd=4'
# → {"sms_cmd_status_result":"3"}  done / "2" failed / "1" in-progress
```

## 16. References

* **This repo:** `docs/adr/0003-zte-mf79ru-polled-sms-source.md`,
  `sms_gate/zte_client.*`, `sms_gate/zte_transport.h`, `sms_gate/zte_record.h`,
  `sms_gate/codec.h`, `sms_gate/sms_gate.ino` (poll/send tasks), probe data
  `../zte-mf79-ru-sms-forwarder/zte-probe/`.
* **Reference forwarder:** `../zte-mf79-ru-sms-forwarder/forwarder.py`,
  `RESEARCH.md`, `CHECKLIST.md`, `probe.py` (subcommands `send`, `sendraw`,
  `sendtz`, `sendfix`, `sendmtr` exhaust the SEND_SMS matrix).
* **Upstream clients of the same goform family:**
  SmsForwardCenter/ZxicSmsFwd (`zxic_utils.py`) — canonical dual-stack
  (goform/reqproc) client, MIT;
  sebenik/zte-sms (Node, MF79U), `@zigasebenik/zte-sms` (npm),
  node-red-zte-sms — Referer+AD+`msg_id ;` + `sms_cmd_status_info` syntax;
  hueNET-llc/zte-sms-smtp — Python ZTE→SMTP forwarder;
  wijayamin/zte-modem-api-docs — Beeline MF90 goform docs;
  Kajkac/ZTE-MC-Home-assistant-repo — `cmd` dictionary for MC series.
* **Security disclosure:** Reversec 2023 — SQL injection in ZTE goform
  `sms_data_total` `order_by` — reason to keep the modem LAN-only.

---
*Teams that port this to another ZTE firmware should re-probe at least:
LOGIN → loginfo → versions → RD → capacity → sms_data_total (page/order
behavior) → DELETE_SMS+AD → SEND_SMS+status. Compare every raw response
against `zte-probe/` before changing the `AD` or paging assumptions. The
`tests/zte_client_test.cpp` vectors are the executable contract.*
