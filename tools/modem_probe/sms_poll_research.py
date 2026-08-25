#!/usr/bin/env python3
"""Research probe for SIM7670G SMS poll — CSDH / CMGL / multipart.

Runs over the modem_probe passthrough (tools/modem_probe/modem_probe.ino
already flashed, loop() bridges USB CDC <-> Serial1). No picocom needed —
pure pyserial.

Usage:
    pip install pyserial                          # once
    python3 tools/modem_probe/sms_poll_research.py
    python3 tools/modem_probe/sms_poll_research.py --port /dev/ttyACM0 --log /tmp/sms-poll.log

Flow mirrors docs/research/modem-sim7670g-sms-poll.md §1-7:
  1) basics without SMS (CSDH/CSCS/CPMS/CMGL empty)
  2) 1× cyrillic at CSDH=0 vs CSDH=1 (CMGL/CMGR raw)
  3) 1× latin 160
  4) multipart 2-3 parts
  5) PDU fallback CMGL=4
  6) delete/verify
  7) URC +CMTI

Every AT exchange is logged raw (copy-paste into §8 «Сырые логи»).
Interactive steps pause and ask you to send an SMS from a phone to the
modem's SIM number — the script then polls CMGL/CMGR and shows UCS2→UTF-8.
No credentials are printed.

Requires: modem_probe.ino flashed, SIM inserted, LTE registered
(CPIN READY, CEREG 0,1, CSQ>12, SMS DONE seen).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial  # type: ignore
    HAS_SERIAL = True
except ImportError:
    serial = None  # type: ignore
    HAS_SERIAL = False

PORT = "/dev/ttyACM0"
BAUD = 115200
READ_TIMEOUT_S = 1.0
STEP_TIMEOUT_S = 8

LOG_PATH_DEFAULT = Path("docs/research/modem-sim7670g-sms-poll.raw.log")


def ucs2_hex_to_utf8(hex_str: str) -> str:
    s = hex_str.strip().replace(" ", "").replace("\r", "").replace("\n", "")
    if len(s) % 4 != 0 or any(c not in "0123456789ABCDEFabcdef" for c in s):
        return f"<not UCS2 hex, raw={hex_str!r}>"
    try:
        chars = [chr(int(s[i : i + 4], 16)) for i in range(0, len(s), 4)]
        return "".join(chars)
    except Exception as e:  # noqa: BLE001
        return f"<decode error {e}: {hex_str!r}>"


# ---------------------------------------------------------------------------


class Modem:
    def __init__(self, ser: serial.Serial, log_file):
        self.ser = ser
        self.log_file = log_file

    def _log(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        line = f"[{ts}] {msg}"
        print(line, flush=True)
        if self.log_file:
            self.log_file.write(line + "\n")
            self.log_file.flush()

    def _write(self, cmd: str):
        self._log(f">>> {cmd}")
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\r\n").encode())

    def read_until(self, tokens: tuple[bytes, ...], timeout_s: float) -> bytes:
        buf = b""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            chunk = self.ser.read(512)
            if chunk:
                buf += chunk
                for t in tokens:
                    if t in buf:
                        return buf
            else:
                # no data — still check if token already arrived via earlier chunk
                time.sleep(0.02)
        return buf

    def command(self, cmd: str, timeout_s: float = STEP_TIMEOUT_S) -> str:
        self._write(cmd)
        raw = self.read_until((b"\r\nOK\r\n", b"\r\nERROR\r\n", b"+CMS ERROR", b"+CME ERROR"), timeout_s)
        text = raw.decode(errors="replace")
        # split echo vs reply for readability — keep raw for §8
        self._log(text.rstrip() or "<no reply>")
        # also show decoded UCS2 bodies if present (heuristic: long hex line)
        for ln in text.splitlines():
            s = ln.strip()
            if len(s) >= 8 and all(c in "0123456789ABCDEFabcdef" for c in s) and len(s) % 4 == 0:
                self._log(f"    decoded: {ucs2_hex_to_utf8(s)!r}")
        return text

    def wait_urc(self, prefix: bytes, timeout_s: float = 30) -> bytes:
        self._log(f"... waiting URC {prefix.decode()} up to {timeout_s:.0f}s (send SMS now)")
        buf = b""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            chunk = self.ser.read(512)
            if chunk:
                buf += chunk
                self._log(chunk.decode(errors="replace").rstrip())
                if prefix in buf:
                    return buf
            time.sleep(0.05)
        return buf


def prompt(msg: str) -> str:
    try:
        return input(f"\n{msg} [Enter=continue, s=skip, q=quit]: ").strip().lower()
    except EOFError:
        return "q"


def step_header(title: str, log: Modem):
    log._log("")
    log._log("=" * 72)
    log._log(title)
    log._log("=" * 72)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=PORT, help="USB CDC port (default %(default)s)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--log", type=Path, default=LOG_PATH_DEFAULT, help="raw log file")
    ap.add_argument("--yes-clear", action="store_true", help="auto run AT+CMGD=,4 without prompt")
    args = ap.parse_args()
    if not HAS_SERIAL:
        print("pyserial not installed: pip install pyserial", file=sys.stderr)
        print("  mise exec -- pip install pyserial   # или pipx", file=sys.stderr)
        return 2

    log_file = None
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        log_file = open(args.log, "a", encoding="utf-8")
        print(f"Logging to {args.log}", file=sys.stderr)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=READ_TIMEOUT_S)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}", file=sys.stderr)
        print("Hint: flash tools/modem_probe first, check dmesg, or --port /dev/ttyUSB0", file=sys.stderr)
        return 2

    m = Modem(ser, log_file)
    m._log(f"event=research_start port={args.port} baud={args.baud}")
    # Drain boot noise + check alive
    ser.reset_input_buffer()
    ser.write(b"AT\r\n")
    m.read_until((b"OK\r\n",), 3)
    m.command("ATE0", 3)
    m.command("ATV1", 3)
    m.command("AT+CMEE=2", 3)

    # --- §1 basics without SMS ---
    step_header("§1 Базовые запросы (без SMS в ящике)", m)
    for cmd in [
        'AT+CMGF=1',
        'AT+CSCS="UCS2"',
        "AT+CSDH=0",
        'AT+CPMS="ME","ME","ME"',
        "AT+CNMI=2,1,0,0,0",
        "AT+CSDH?",
        "AT+CSCS?",
        "AT+CPMS?",
        "AT+CNMI?",
        "AT+CMGF?",
        "AT+CSDH=?",
        "AT+CSCS=?",
        "AT+CPMS=?",
        "AT+CNMI=?",
    ]:
        m.command(cmd, 5)

    if args.yes_clear or prompt("Очистить ящик AT+CMGD=,4 и AT+CMGL?") not in ("s", "q"):
        m.command("AT+CMGD=,4", 8)
        m.command("AT+CPMS?", 5)
        m.command('AT+CMGL="REC UNREAD"', 8)
        m.command('AT+CMGL="ALL"', 8)
    else:
        m._log("skip clear")

    # quick health
    m.command("AT+CPIN?", 3)
    m.command("AT+CSQ", 3)
    m.command("AT+CEREG?", 3)
    m.command("AT+CCLK?", 3)

    # --- §2 Cyrillic CSDH 0 vs 1 ---
    step_header("§2 Одно SMS кириллица — CSDH=0 vs 1", m)
    m.command("AT+CSDH=0", 3)
    ans = prompt("Отправь на SIM-номер: «Привет тест 123» (1 часть, UCS2<70). После доставки")
    if ans == "q":
        return 0
    if ans != "s":
        m.command('AT+CMGL="REC UNREAD"', 8)
        # try to auto-detect idx
        raw = m.command('AT+CMGL="ALL"', 8)
        # extract first idx
        import re

        mm = re.search(r"\+CMGL:\s*(\d+)", raw)
        if mm:
            idx = mm.group(1)
            m.command(f"AT+CMGR={idx}", 5)
        m.command("AT+CSDH=1", 3)
        m.command('AT+CMGL="REC UNREAD"', 8)
        m.command('AT+CMGL="ALL"', 8)
        if mm:
            m.command(f"AT+CMGR={idx}", 5)
        m._log("Скопируй raw выше в docs/research/modem-sim7670g-sms-poll.md § Сырые логи (CSDH 0/1)")
        if not args.yes_clear and prompt("Удалить это SMS перед след. шагом?") not in ("s", "q"):
            if mm:
                m.command(f"AT+CMGD={idx}", 5)
                m.command('AT+CMGL="ALL"', 5)

    # --- §3 Latin ---
    step_header("§3 Одно SMS латиница 160 (GSM vs UCS2)", m)
    m.command('AT+CSCS="GSM"', 3)
    m.command("AT+CSDH=0", 3)
    ans = prompt("Отправь латиницу ~30 символов (например 'Hello test 123'). После доставки")
    if ans not in ("s", "q"):
        m.command('AT+CMGL="ALL"', 8)
        m.command('AT+CSCS="UCS2"', 3)
        m.command('AT+CMGL="ALL"', 8)
        m._log("Сравни GSM vs UCS2 hex для oa/body")

    # --- §4 Multipart ---
    step_header("§4 Multipart 2-3 части (длинная кириллица >70)", m)
    m.command('AT+CSCS="UCS2"', 3)
    m.command("AT+CSDH=1", 3)
    m.command('AT+CPMS="ME","ME","ME"', 3)
    ans = prompt("Отправь длинное сообщение ~150 кириллических символов (разобьётся на 2-3 части)")
    if ans not in ("s", "q"):
        raw = m.command('AT+CMGL="ALL"', 10)
        import re

        idxs = re.findall(r"\+CMGL:\s*(\d+)", raw)
        m._log(f"найдено индексов: {idxs}")
        for idx in idxs[:4]:
            m.command(f"AT+CMGR={idx}", 5)
        m.command("AT+CPMS?", 5)
        m._log("Вопросы: одинаковый oa/date? есть ли ref/seq/total в +CMGL хвосте при CSDH=1? idx монотонный?")

    # --- §5 PDU ---
    step_header("§5 PDU-альтернатива (если TEXT скрывает UDH)", m)
    if prompt("Проверить PDU AT+CMGF=0; AT+CMGL=4 ?") not in ("s", "q"):
        m.command("AT+CMGF=0", 3)
        m.command("AT+CMGL=4", 10)
        # try first idx in PDU
        import re

        # reuse last idxs if any
        try:
            if idxs:
                m.command(f"AT+CMGR={idxs[0]}", 5)
        except NameError:
            pass
        m.command("AT+CMGF=1", 3)
        m._log("Зафиксируй PDU-hex, ищи TP-UDH IE 0x00 (concat 8/16-bit)")

    # --- §6 Delete/verify ---
    step_header("§6 Удаление / верификация", m)
    if prompt("Проверить AT+CMGD точечно и bulk? (требует SMS в ящике)") not in ("s", "q"):
        raw = m.command('AT+CMGL="ALL"', 8)
        import re

        idxs = re.findall(r"\+CMGL:\s*(\d+)", raw)
        if idxs:
            m.command(f"AT+CMGD={idxs[0]}", 5)
            m.command('AT+CMGL="ALL"', 8)
            if len(idxs) > 1:
                m.command(f"AT+CMGD={idxs[1]}", 5)
                m.command('AT+CMGL="ALL"', 8)
        m.command("AT+CMGD=,4", 8)
        m.command("AT+CPMS?", 5)
        m._log("bulk 4 должен очистить ME полностью")

    # --- §7 URC ---
    step_header("§7 URC +CMTI vs +CMT", m)
    m.command("AT+CNMI=2,1,0,0,0", 3)
    ans = prompt("Отправь ещё одно SMS и наблюдай URC (ожидается +CMTI: \"ME\",<idx>) — жду 30с")
    if ans not in ("s", "q"):
        m.wait_urc(b"+CMTI:", 30)
        m.command('AT+CMGL="REC UNREAD"', 5)

    m._log("")
    m._log("Готово. Скопируй весь лог (или cat docs/research/modem-sim7670g-sms-poll.raw.log)")
    m._log("в docs/research/modem-sim7670g-sms-poll.md § «Сырые логи», заполни §8 выводы,")
    m._log("затем запускай парсер (PLAN §10.2).")
    ser.close()
    if log_file:
        log_file.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
