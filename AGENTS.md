# AGENTS.md

<!-- #region SECTION_Dev_Rules -->

## Development Rules

- Follow YAGNI, DRY, KISS, and SOLID. Prefer the smallest change that preserves
  current configuration and recovery contracts.
- Work top-down: establish requirements and module boundaries; write a
  `MODULE_CONTRACT`; contract public/non-trivial entities; create stubs; then
  implement inside paired GRACE regions.
- Keep hardware control, persistent configuration, and HTML rendering in their
  respective modules.
- Use only bundled Arduino-ESP32 libraries unless a new dependency is required
  by an approved design decision.
- Any change to a persisted record schema (e.g. `config_record.h`,
  `smtp_record.h`) requires a version bump and a load-time migration of
  previously stored data.
- Do not add compatibility layers without a concrete need.
- Do not commit Wi-Fi passwords, administrator passwords, email credentials,
  modem credentials, or generated build artifacts.
- Never run `git commit` (or any other state-changing git action) unless the
  user explicitly asks for it in the current request; leaving verified,
  uncommitted changes in the worktree is the expected end state.
- If a commit is requested, format its message according to Conventional
  Commits.
- Project is managed by mise-en-place. Run tools via mise exec like `mise exec -- arduino-cli`.
- Don't enumerate ports. Target device, if present, connected to `/dev/ttyACM0`.

<!-- #endregion SECTION_Dev_Rules -->

<!-- #region SECTION_ADR_Rule -->

## Architectural Decisions Rule

Before changing module boundaries, persistent-data ownership, the partition
layout, network/security protocols, reset/recovery behavior, or runtime
lifecycle, inspect `docs/adr/`.

If a change introduces a new architectural trade-off with viable alternatives,
create `docs/adr/` and record an ADR there before implementation. Number ADRs
sequentially from the next free number. Bug fixes, file relocations, test
additions, documentation maintenance, and feature work that follows an existing
decision do not require an ADR.

<!-- #endregion SECTION_ADR_Rule -->

<!-- #region SECTION_Verification -->

## Verification

- Add focused tests for pure core logic. Cover the happy path and meaningful
  corrupt/invalid input cases first.
- Compile the firmware after changing files under `sms_gate/`:

  ```bash
  python3 tools/gen_assets.py  # or: mise run assets
  mise exec -- arduino-cli compile sms_gate
  ```

- Run all host tests (preferred):

  ```bash
  mise run test
  ```

- Run the available static checks before completion:

  ```bash
  mise run fmt:check
  mise run lint
  git diff --cached --check
  git diff --check
  ```

- `mise run lint` runs Cppcheck. Treat a successful Arduino compile, `mise run test`, clang-format check, Cppcheck run, and whitespace checks as the minimum local verification.

<!-- #region SECTION_Logging -->

### Logging & Verification

Use USB CDC `Serial` structured events for operational boundaries:

```cpp
Serial.printf("event=sta_connected ip=%s\n", WiFi.localIP().toString().c_str());
Serial.println("event=config_save_begin");
```

- Use `event=<stable-name>` followed by flat `key=value` fields.
- Log startup, persistence, AP/STA transitions, mDNS, HTTP route registration,
  configuration tests, and error branches.
- Never log passwords, HTTP form values, authentication headers, or other
  secrets. SSID, MAC, IP, RSSI, and event outcome are allowed only when useful
  for the operator.
- Preserve boot stages through the in-memory boot trace; it is replayed after
  USB CDC becomes ready. Keep markers deterministic when tests or operator
  procedures depend on them.
- Update tests when a changed pure behavior invalidates their assertions.

<!-- #endregion SECTION_Logging -->

<!-- #endregion SECTION_Verification -->

<!-- #region SECTION_Project -->

## Project

The project targets the LilyGO T-SIM7670G-S3 / ESP32-S3. Its implemented MVP is
local network provisioning: a device joins one WPA2/WPA3-Personal network when a
verified profile exists, otherwise exposes a captive portal. Configuration is
stored in the dedicated `appcfg` NVS partition so USB bootloader recovery can
clear it without removing future SMS, GNSS, or email settings.

### Core Flow

1. Build the stable station MAC, AP SSID, and mDNS hostname from eFuse data.
2. Load the checksummed configuration record from `appcfg`.
3. With no valid record, start the initial open AP and captive portal.
4. With a record, try STA for 30 seconds; on failure start the password-protected
   fallback AP and retry STA every 60 seconds.
5. Serve configuration through HTTP Digest authentication after initial setup.
6. Save replacement credentials only after they connect successfully.
7. Recover forgotten/broken configuration through the documented USB bootloader
   erase of `appcfg`; do not use `erase-flash`.

### Structure

```text
sms_gate/
├── sms_gate.ino      # thin setup/loop shell, boot trace, Serial events
├── codec/            # codec headers (codec_base64/md5/ucs2.h), codec.h re-export at root
├── persistence/      # headers: config_record.h, config_store*.h (appcfg NVS, validated)
├── smtp/             # headers: smtp_record/client/service/transport.h
├── zte/              # headers: zte_record/client/service/transport/json/form_codec.h
├── modem/            # headers: modem_record/client/service/transport.h
├── system/           # headers: wifi_manager/http_server/web_api/email_builder/task_control/sms_validate/plain_socket_reader.h
├── *.cpp             # impl at sketch root (Arduino flat compile) — logically in same feature groups
├── web_assets.h      # generated gzip assets from www/ (not committed)
├── partitions.csv    # appcfg NVS partition and FFat layout
└── sketch.yaml       # Arduino CLI board, USB CDC, flash, and port settings
# headers grouped by feature (records stay in their feature); .cpp stay at root for Arduino build.
# codec/ already header-only; smtp/zte/modem/system/persistence hold public contracts.
www/                  # client-rendered UI sources (index.html, app.js, style.css)
tools/gen_assets.py   # generates sms_gate/web_assets.h from www/
tests/
├── config_record_test.cpp    # Host test for record integrity and field limits
├── smtp_client_test.cpp      # Host test for SMTP record and dialog sequencing
├── zte_client_test.cpp       # Host test for the ZTE goform dialog and record
├── modem_client_test.cpp     # Host test for SIM7670G AT dialog and parsers
├── zte_form_codec_test.cpp   # Host test for form-urlencoding (ZTE goform)
├── sms_validate_test.cpp     # Host test for shared SMS validation (recipient/335 units)
├── email_builder_test.cpp    # Host test for subject/body rendering (INCOMPLETE, alias)
├── web_api_test.cpp          # Host test for JSON escaping and envelope rendering
└── host_stub/                # Minimal Arduino String/WebServer stubs for host tests
README.md             # Build, provisioning, and USB recovery procedure
```

<!-- #endregion SECTION_Project -->

<!-- #region RULES_REPEATED -->

<critical_rules>
<rule>Follow YAGNI, DRY, KISS, and SOLID; preserve configuration/recovery contracts.</rule>
<rule>Work top-down and use paired GRACE regions with meaningful purposes.</rule>
<rule>Keep lifecycle, persistence, and UI in separate modules.</rule>
<rule>Never commit or log credentials; use structured Serial events without secrets.</rule>
<rule>Consult or create docs/adr/ before a new architectural trade-off.</rule>
<rule>Run Arduino compile, mise run test, mise fmt:check, mise lint, and both git diff whitespace checks.</rule>
</critical_rules>

<!-- #endregion RULES_REPEATED -->
