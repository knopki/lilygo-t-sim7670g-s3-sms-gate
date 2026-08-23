# ADR-0002: Deliver email over SMTP STARTTLS with a minimal in-tree client

- **Status:** Accepted
- **Date:** 2026-08-23

## Context

The product goal is forwarding received SMS as email. Every practical
submission provider (Gmail, SendGrid, Brevo, SMTP2GO) requires TLS on port 587
(STARTTLS) or 465 (implicit). The firmware runs on arduino-esp32 3.3.11 with
about 2 MB of free app flash, so flash size is not the constraint; free
internal heap at handshake time (roughly 50-60 KB) and credential safety are.

Viable options considered:

- **mobizt/ESP-Mail-Client 3.4.24** — MIT, STARTTLS with certificate checks,
  but deprecated by its author in favor of ReadyMail.
- **mobizt/ReadyMail** — maintained successor, but beta and CC BY-NC licensed
  with a paid commercial option.
- **xreef/EMailSender 4.1.1** — MIT and small, but its ESP32 STARTTLS path
  calls `setInsecure()` unconditionally and verifies no certificate (checked
  in source); SMTP credentials would cross a trivially interceptable
  connection.
- **mobizt/ESP_SSLClient** — BearSSL-based generic TLS with a STARTTLS
  upgrade, but adds roughly 1 MB of flash and another dependency to replace a
  capability the core already ships.
- **Own minimal SMTP dialog over the bundled `NetworkClientSecure`** — the
  arduino-esp32 core 3.x has native STARTTLS (`setPlainStart()` plus
  `startTLS()` on the same socket) and an embedded Mozilla root bundle
  (`esp_crt_bundle`, about 67 KB of flash, no per-connection RAM cost), with
  an official Gmail port 587 example.

For the trust anchor specifically, an earlier draft of this ADR required the
operator to paste a pinned root CA PEM. That moved a certificate-derivation
chore onto the operator, and every derivation path (provider PKI sites,
system stores, helper scripts) proved environment-dependent. The anchor
therefore had to come from the device itself.

## Decision

Implement a minimal in-tree SMTP client (`sms_gate/smtp_client.*` pure dialog
plus `sms_gate/smtp_transport.h` device channel) on the bundled
`NetworkClientSecure`: connect plaintext on 587, EHLO, STARTTLS, upgrade,
AUTH LOGIN, one message per connection; implicit TLS on 465 as a
configuration-selected mode. No third-party dependency; EMailSender is used
only as a reference for dialog details such as multi-line replies and AUTH
encoding.

TLS trust comes from the Mozilla CA bundle embedded in the firmware image:
the channel calls `setCACertBundle()` on the linker symbols
`_binary_x509_crt_bundle_start/_end`, giving browser-grade chain, expiry, and
hostname validation on every connection with zero operator involvement. There
is no insecure mode. Root rotations are handled by firmware updates, which the
device already depends on for everything else; Mozilla roots live 10-25 years
and SMTP providers' roots (DigiCert Global Root G2 valid to 2038, GTS Root R4
to 2036) comfortably outlive a firmware generation. The SMTP password follows
the existing invariant: never logged and never returned over HTTP.

SMTP settings live in their own checksummed record (`smtp_record.h`) under a
separate NVS key (`smtp` namespace, `record` key) in the existing `appcfg`
partition rather than as an extension of the Wi-Fi record, so the existing
provisioning and USB-recovery contracts stay untouched. The record stores
host, port, security mode, username, password, from, and recipient.

The message body is always base64-encoded (`charset=utf-8`) so arbitrary SMS
text never depends on 8BITMIME; Date and Message-ID headers are deferred until
GNSS time is available.

## Alternatives Considered

### Adopt ESP-Mail-Client or ReadyMail

Full-featured (attachments, IMAP) and battle-tested, but one is unmaintained
and the other is beta with a non-open license for roughly 200 lines of dialog
logic this project needs.

### Use EMailSender as the transport

Smallest integration effort, but an on-path attacker could harvest the SMTP
credentials, which is unacceptable for a credential-handling device.

### Operator-pinned root CA PEM

Strongest anchor against a compromised certificate authority, but it requires
the operator to derive a root certificate with external tooling before the
device works, and that derivation cannot be made device-local (see the TOFU
alternative). Dropped as an operator burden the product does not justify.

### On-device TOFU (trust on first use, SSH-style)

Have the device capture the served chain over an unvalidated connection and
pin it after the operator confirms a fingerprint. Rejected: servers do not
send their root, so the pin would land on an intermediate certificate that
providers rotate every one to six months, breaking delivery regularly; and
the operator has no independent source against which to check the displayed
fingerprint, so the confirmation step would be ceremony without security.

## Consequences

- **Positive:** no new dependency; standard chain validation on every
  submission; zero certificate-related operator setup; the dialog is
  host-testable through a channel interface; heap cost is paid only while a
  message is being sent.
- **Negative / trade-offs:** trust spans the whole Mozilla root set rather
  than one pinned root, so a compromised CA could in principle forge the SMTP
  server's identity; the web UI already runs on plain HTTP with Digest auth,
  so TLS-with-standard-roots remains far stronger than the weakest link in
  the device's overall threat model.
- **Accepted risks:** the core 3.x plain-phase socket path was confirmed
  broken on hardware: `available()` after `setPlainStart()` does a
  zero-timeout lwIP `select()` that sporadically returns `-1/EINTR` while
  other tasks run their own selects, killing the dialog within a second
  (implicit TLS on 465 is unaffected because TLS-phase reads bypass lwIP
  select). The transport works around it by reading plain-phase lines with
  one bounded `recv()` per byte (`SO_RCVTIMEO`, no select at all); if a
  future core release fixes the select path, this workaround can be
  dropped. mbedTLS keeps its buffers in internal DRAM, so PSRAM does not
  relieve the free-heap requirement at handshake time. Root-set changes
  require a firmware update rather than a configuration change. The device
  channel (TLS wiring) is verified on hardware for implicit TLS and
  STARTTLS via the workaround.
