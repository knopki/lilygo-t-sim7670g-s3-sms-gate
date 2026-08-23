# ADR-0001: Client-rendered web UI with gzipped assets in PROGMEM

- **Status:** Accepted
- **Date:** 2026-08-23

## Context

The configuration UI is rendered on the device by concatenating HTML strings in
`web_ui.cpp`. Every layout change means editing C++ string building, escaping is
hand-rolled per page, and each interaction re-renders a full document. The UI
will keep growing (SMS log, GNSS status, email settings), so string-concat
server rendering will not scale, and the device has no CDN to fall back on: it
must serve everything itself on a LAN or through the captive portal.

Options considered:

- **Vanilla JS single-page app, no build toolchain:** the device serves static
  `index.html` / `app.js` / `style.css` plus a small JSON API; the browser
  renders all markup.
- **SPA with a build toolchain (Vite/Preact/Svelte):** better developer
  ergonomics, but requires a Node toolchain the project deliberately avoids, for
  what is two forms and a status table.
- **htmx served from the device:** keeps HTML generation in C++; only swaps the
  templating engine, so the maintenance pain remains.
- **Assets on the FFat partition via `serveStatic`:** `arduino-cli` cannot
  upload a filesystem image for ESP32, so it needs `mklittlefs` plus a separate
  flash step, a mount/format failure branch at boot, and a UI/firmware version
  handshake after OTA. FFat is reserved for future device data.

## Decision

The UI becomes a dependency-free single-page app in `www/` (vanilla JavaScript
with template literals). At build time `tools/gen_assets.py` gzip-compresses
those files into `sms_gate/web_assets.h` as PROGMEM arrays; the firmware serves
them with `Content-Encoding: gzip`. `web_ui.*` is replaced by `web_api.*`, which
owns asset serving and JSON serialization. Dynamic data moves to a JSON API:
`GET /api/status`, `GET /api/scan`, `POST /api/setup`, `POST /api/network`,
`POST /api/password`. Requests stay `application/x-www-form-urlencoded` so the
built-in `WebServer` argument parsing is reused; authentication stays HTTP
Digest on the API routes and relies on the browser-native prompt for `fetch`.
The captive portal redirects to `/`, which now serves the SPA; the SPA picks the
setup or configuration view from `GET /api/status`.

## Alternatives Considered

### SPA with a build toolchain

Rejected: introduces a Node/npm toolchain and a bundled framework dependency for
a two-form UI, against the project's bundled-libraries-only rule.

### htmx

Rejected: HTML still originates in C++ string concatenation; only the transport
of partial HTML changes.

### Assets on FFat

Rejected: separate filesystem image flashing outside `arduino-cli`, an extra
boot-time failure branch, and UI/firmware version skew across OTA updates — for
no benefit while UI and firmware always change together.

## Consequences

- **Positive:** HTML/CSS/JS are edited as real source files; rendering code
  leaves the firmware; the JSON API is reusable for future SMS/GNSS/email
  features; gzip keeps the whole UI at a few kilobytes of flash; OTA keeps UI
  and firmware versions in lockstep; the recovery contract (partition table, USB
  `appcfg` erase) is unchanged.
- **Negative / trade-offs:** a generated header adds a build step that must run
  before `arduino-cli compile`; the UI now requires a JavaScript-capable
  browser; a no-JS fallback page no longer exists.
- **Accepted risks:** the raw bundle consumes flash inside the 3 MB OTA slots
  (comfortably available); Digest-over-`fetch` depends on the browser's native
  authentication prompt behavior.
