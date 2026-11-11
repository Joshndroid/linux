#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print the CPU rate/voltage tables ACPM applies on this chip (zuma).

ACPM builds its frequency-voltage map (ASV group + fused margins) in the
non-secure APM SRAM.  Dump that region read-only on the phone:

    mmap(/dev/mem, 0x53000, offset=0x15705000), 32-bit reads only

(NOT from 0x15700000: the first 0x5000 bytes are secure and reading them
hangs the kernel.)  Then:

  fvmap-decode.py apm-sram-ns.bin           # all tables found
  fvmap-decode.py apm-sram-ns.bin --opp     # zuma.dtsi opp-microvolt per CPU level

Tables are found by content (descending kHz with plausible uV), so the
script does not depend on ACPM's header layout.
"""
import struct, sys

NS = 0x15705000 - 0x15700000
d = open(sys.argv[1], "rb").read()
CAPS = {"cpucl0": (324000, 1704000, 2253000), "cpucl1": (402000, 2367000, 2600000),
        "cpucl2": (500000, 2914000, 3015000)}  # acme min, max, ACPM top level


def runs():
    o, out = 0, []
    while o + 8 <= len(d):
        cur, p = [], o
        while p + 8 <= len(d):
            r, v = struct.unpack_from("<2I", d, p)
            if not (10000 <= r <= 4000000 and 300000 <= v <= 1400000) or (cur and r >= cur[-1][0]):
                break
            cur.append((r, v))
            p += 8
        if len(cur) >= 3:
            out.append((o + NS, cur))
            o = p
        else:
            o += 4
    return out


tables = runs()
if "--opp" in sys.argv:
    for dom, (lo, hi, top) in CAPS.items():
        t = next(c for _, c in tables if c[0][0] == top)
        print(f"{dom}:")
        for r, v in sorted(t):
            if lo <= r <= hi:
                print(f"  {r // 1000:5d} MHz  {v} uV")
else:
    for off, t in tables:
        print(f"SRAM+{off:#x}:", " ".join(f"{r // 1000}:{v / 1000:g}" for r, v in t))
