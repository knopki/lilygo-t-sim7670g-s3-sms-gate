# Golden snapshots — web_api JSON (phase A baseline)

Baseline of `web_api.cpp` output on 2026-08-26, before refactor.

Each `*.json` file holds the direct output of a `render*Json` function for
representative input data. Any change to format or field set is intentional
and requires a golden-file update plus `git diff` review.

Source:
- `sms_gate/web_api.h` — structs WebStatus, WebSmtpConfig, WebZteConfig,
  WebModemSourceConfig, WebModemStatus, WebAsyncOp
- `sms_gate/web_api.cpp` — render functions
  (escapeJson -> appendJsonString -> render*Json)

Generation: manual, from source code, without Arduino dependencies.
Verified line-by-line against `web_api.cpp`. For host verification, add
`tests/web_api_test.cpp` (see PLAN §8.1) with
`EXPECT_EQ(render*Json(...), golden)`.

Note: `python3 tools/gen_assets.py` output (`web_assets.h`) is deterministic
(gzip mtime=0). No golden file is necessary for it.
