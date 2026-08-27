#!/usr/bin/env python3
"""NTP conformance probe for the sms_gate NTP server.

Checks the device's UDP/123 replies against the acceptance rules that
chrony (ntp_core.c) and ntpd (ntp_proto.c) apply:

  normal reply : mode 4, LI=0, stratum 1..15, origin == request transmit,
                 nonzero rec/xmt, rec != xmt, usec-resolution fractions
  VN clamp     : request VN=2 -> reply VN=3, request VN=7 -> reply VN=4
  mode filter  : mode 1/4/5 requests get no reply (anti-reflection)
  KoD RATE     : burst > 20 req/s -> LI=3 + stratum 0 + refid "RATE" and
                 org == rec == xmt == request transmit (ntpd requirement)

Usage: python3 tools/ntp_probe.py <device-ip|hostname> [port]
Exit code 0 = all checks passed.

Note: the rate-limit burst is the last test so earlier checks run while
the per-second budget is intact.
"""

import socket
import struct
import sys
import time

NTP_EPOCH_OFFSET = 2208988800
RATE_LIMIT = 20  # kNtpRateLimitPerSecond in sms_gate/ntp_server.cpp


def frac_to_float(sec, frac):
    return sec - NTP_EPOCH_OFFSET + frac / 2**32


def build_request(mode, vn, xmt_sec, xmt_frac):
    pkt = bytearray(48)
    pkt[0] = ((vn & 0x07) << 3) | (mode & 0x07)
    struct.pack_into(">II", pkt, 40, xmt_sec, xmt_frac)
    return bytes(pkt)


class Client:
    def __init__(self, host, port, timeout=1.0):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def query(self, mode=3, vn=4):
        """Send one request; return (request, response_or_None)."""
        now = time.time()
        xmt_sec = int(now) + NTP_EPOCH_OFFSET
        xmt_frac = int((now % 1) * 2**32)
        req = build_request(mode, vn, xmt_sec, xmt_frac)
        self.sock.sendto(req, self.addr)
        try:
            resp, _ = self.sock.recvfrom(1024)
        except socket.timeout:
            return req, None
        return req, resp


def unpack(resp):
    return {
        "lvm": resp[0],
        "li": resp[0] >> 6,
        "vn": (resp[0] >> 3) & 0x07,
        "mode": resp[0] & 0x07,
        "stratum": resp[1],
        "refid": resp[12:16],
        "org": resp[24:32],
        "rec": struct.unpack(">II", resp[32:40]),
        "xmt": struct.unpack(">II", resp[40:48]),
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 123
    cli = Client(host, port)
    failures = []

    def check(name, ok, detail=""):
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name}{(' — ' + detail) if detail and not ok else ''}")
        if not ok:
            failures.append(name)

    # --- normal reply -------------------------------------------------
    req, resp = cli.query()
    if resp is None:
        print("[FAIL] server reachable — no reply to client-mode request")
        print("       (is the device STA-connected and stratum>0?)")
        return 1
    r = unpack(resp)
    check("reply is 48 bytes", len(resp) == 48, f"got {len(resp)}")
    check("mode 4 (server)", r["mode"] == 4, f"got {r['mode']}")
    check("LI=0 (synced)", r["li"] == 0, f"got {r['li']}")
    check("stratum 1..15", 1 <= r["stratum"] <= 15, f"got {r['stratum']}")
    check("origin echoes request transmit", r["org"] == req[40:48])
    check("rec/xmt nonzero", any(r["rec"]) and any(r["xmt"]))
    check("rec != xmt (separate t2/t3)", r["rec"] != r["xmt"])
    check("refid set", any(r["refid"]), f"got {r['refid']!r}")

    # --- VN clamp ------------------------------------------------------
    _, resp = cli.query(vn=2)
    check("VN=2 request -> VN=3 reply", resp is not None and unpack(resp)["vn"] == 3)
    _, resp = cli.query(vn=7)
    check("VN=7 request -> VN=4 reply", resp is not None and unpack(resp)["vn"] == 4)

    # --- usec resolution (ms quantization check) ------------------------
    # A ms-quantized fraction f satisfies (f*1000) mod 2^32 <= 1000; a
    # usec fraction only satisfies (f*1000000) mod 2^32 <= 1000.
    fracs = []
    for _ in range(6):
        _, resp = cli.query()
        if resp:
            fracs.append(unpack(resp)["xmt"][1])
    sub_ms = [
        f
        for f in fracs
        if min((f * 1000) % 2**32, 2**32 - ((f * 1000) % 2**32)) > 2000
    ]
    check("usec-resolution fractions", len(sub_ms) >= 3, f"{len(sub_ms)}/6 samples sub-ms")

    # --- mode filter (anti-reflection) ---------------------------------
    for mode in (1, 4, 5):
        _, resp = cli.query(mode=mode)
        check(f"mode {mode} request ignored", resp is None)

    # --- stray-datagram recovery ----------------------------------------
    # A short (<48 B) or oversized (>48 B) datagram used to wedge reception
    # forever: NetworkUDP::parsePacket() returns 0 while the previous packet
    # stays unread. After both junk datagrams the server must still answer.
    cli.sock.sendto(bytes([0x1B]) + 19 * b"\0", cli.addr)   # 20 B short
    cli.sock.sendto(bytes([0x1F]) + 79 * b"\0", cli.addr)  # 80 B oversized
    _, resp = cli.query()
    check("server survives stray short/oversized datagrams", resp is not None)

    # --- KoD RATE burst (last: drains the per-second budget) ------------
    kod_ok = False
    kod_detail = "no RATE reply in burst"
    normal = 0
    rps = 50
    total = 150  # 3 s at 50 rps: survives ~50% UDP loss and still exceeds
    t0 = time.time()
    for i in range(total):
        req, resp = cli.query()
        if resp is None:
            pass  # lost on the wire or queue; the pace is what matters
        else:
            r = unpack(resp)
            if r["stratum"] == 0 and r["li"] == 3 and r["refid"] == b"RATE":
                echo = r["org"] == req[40:48] and r["rec"] == r["xmt"] == (
                    struct.unpack(">II", req[40:48])
                )
                if echo:
                    kod_ok = True
                    kod_detail = ""
                else:
                    kod_detail = "RATE seen but org/rec/xmt not all == request transmit"
            else:
                normal += 1
        target = t0 + (i + 1) / rps
        while time.time() < target:
            pass
    check(f"KoD RATE above {RATE_LIMIT} req/s (chrony+ntpd form)", kod_ok, kod_detail)
    print(f"       burst: {normal} normal + RATE replies over {total / rps:.0f} s")

    print()
    if failures:
        print(f"{len(failures)} check(s) failed: {', '.join(failures)}")
        return 1
    print("all checks passed — reply form is accepted by chrony and ntpd")
    return 0


if __name__ == "__main__":
    sys.exit(main())
