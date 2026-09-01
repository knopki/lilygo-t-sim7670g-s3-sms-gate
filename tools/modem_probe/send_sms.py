#!/usr/bin/env python3
# region MODULE_CONTRACT
# PURPOSE: Exercises one real SMS path so modem setup and payload failures are observable.
# SCOPE:
# - Sends one GSM SMS through the modem-probe serial bridge, configures
#   text-mode storage, and prints the resulting modem dialog.
# INVARIANTS:
# - Each command starts with a cleared input buffer;
# - text is limited to a single 160-byte GSM payload
# - no credentials are handled or printed.
# endregion MODULE_CONTRACT

"""One-shot SMS send test over the modem_probe bridge (run OUTSIDE the nono
sandbox: it opens /dev/ttyACM0 directly).

Usage:
    python3 tools/modem_probe/send_sms.py +79XXXXXXXXX "message text"

Sets text mode, GSM charset, ME storage, new-message indications, sends the
SMS, then lists whatever is stored. Never prints credentials (there are none
here), prints raw modem replies for diagnosis.
"""

import re
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200


# region FUNC_read_until
# PURPOSE: Waits for a modem result without hiding timeout or error replies.
def read_until(ser, token=b"OK\r\n", timeout_s=10):
    buf = b""
    deadline = time.monotonic() + timeout_s
    while b"ERROR" not in buf and token not in buf and time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if b"+CMS ERROR" in buf or b"+CME ERROR" in buf:
                break
    return buf.decode(errors="replace").strip()


# endregion FUNC_read_until

# region FUNC_command
# PURPOSE: Keeps each probe exchange bounded and visible for modem diagnosis.
def command(ser, cmd, timeout_s=10, expect=b"OK\r\n"):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    reply = read_until(ser, expect, timeout_s)
    print(f">>> {cmd}\n{reply}\n")
    return reply


# endregion FUNC_command

# region FUNC_main
# PURPOSE: Makes one-shot SMS failures visible to operators and automation.
def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    number, text = sys.argv[1], sys.argv[2]
    if len(text.encode()) > 160:
        print("text too long for a single GSM-7 SMS")
        return 1

    with serial.Serial(PORT, BAUD, timeout=1) as ser:
        # Wake the passthrough: any byte pair resyncs; send plain AT first.
        ser.write(b"AT\r\n")
        print(read_until(ser))

        command(ser, "ATE1")  # local echo makes the dialog readable
        command(ser, "AT+CMGF=1")
        command(ser, 'AT+CSCS="GSM"')
        command(ser, 'AT+CPMS="ME","ME","ME"')
        command(ser, "AT+CNMI=2,1,0,0,0")

        # Submit: prompt '>' arrives after the address line.
        ser.reset_input_buffer()
        ser.write(f'AT+CMGS="{number}"\r\n'.encode())
        prompt = read_until(ser, token=b">", timeout_s=5)
        print(f'>>> AT+CMGS="{number}"\n{prompt}\n')
        if ">" not in prompt:
            print("no '>' prompt from modem, aborting")
            return 1

        ser.write(text.encode() + bytes([0x1A]))  # Ctrl-Z submits
        result = read_until(ser, timeout_s=30)
        print(result)
        ok = "+CMGS:" in result

        command(ser, 'AT+CMGL="ALL"', timeout_s=15)
        return 0 if ok else 2


# endregion FUNC_main

if __name__ == "__main__":
    sys.exit(main())
