#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode an Exynos ECT dump (the bootloader's table; on husky at
0x90000000, 0x60000 bytes).  Format as parsed by downstream
android-gs drivers/soc/google/ect_parser.c.

  ect-decode.py ect.bin            # blocks, DVFS levels, GEN MINMAX tables
  ect-decode.py ect.bin --opp      # zuma CPU OPP tables as DT source

Dumping it needs /dev/mem without CONFIG_STRICT_DEVMEM (build.sh --config
with a copy of the config that unsets it, one boot only), e.g. in Python:
mmap(/dev/mem, 0x60000, offset=0x90000000).

The --opp voltages are ASV table 2, group 4 (a mid bin): ACPM programs the
real fused voltages itself, so they only feed the Energy Model.
"""
import struct, sys

b = open(sys.argv[1], "rb").read()
u32 = lambda o: struct.unpack_from("<I", b, o)[0]


def string(o):
    n = u32(o) + 1
    s = b[o + 4:o + 4 + n].split(b"\0")[0].decode(errors="replace")
    return s, o + 4 + ((n + 3) & ~3)


def named(base):
    o, out = base + 12, {}
    for _ in range(u32(base + 8)):
        name, o = string(o)
        out.setdefault(name, base + u32(o))
        o += 4
    return out


assert b[:4] == b"PARA", "not an ECT dump"
o, blocks = 16, {}
for _ in range(u32(12)):
    name, o = string(o)
    blocks[name] = u32(o)
    o += 4
dvfs, asv, gen = named(blocks["DVFS"]), named(blocks["ASV"]), named(blocks["GEN"])


def dvfs_levels(a):
    mode, nclk, nlvl = u32(a + 16), u32(a + 20), u32(a + 24)
    a += 28
    if mode == 2:
        a += 4 * nclk
    else:
        for _ in range(nclk):
            _, a = string(a)
    return [u32(a + 8 * i) for i in range(nlvl)]


def asv_volts(a, table, group):
    ng, nl, nt = u32(a), u32(a + 4), u32(a + 8)
    a += 12 + 4 * nl
    for t in range(nt):
        a += 4 + 8 + 4 * nl
        if t == table:
            return [b[a + l * ng + group] * 6250 for l in range(nl)]
        a += ng * nl


def gen_table(name):
    a = gen[name]
    col, row = u32(a), u32(a + 4)
    v = [u32(a + 8 + 4 * i) for i in range(col * row)]
    return [v[r * col:(r + 1) * col] for r in range(row)]


# downstream zuma-cpu.dtsi exynos-acme min/max (kHz)
CAPS = {"CPUCL0": (324000, 1704000), "CPUCL1": (402000, 2367000),
        "CPUCL2": (500000, 2914000)}

if "--opp" in sys.argv:
    for i, (dom, (lo, hi)) in enumerate(CAPS.items()):
        f, v = dvfs_levels(dvfs[dom]), asv_volts(asv[dom], 2, 4)
        print(f"\tzuma_cpucl{i}_opp: opp-table-cpucl{i} {{")
        print('\t\tcompatible = "operating-points-v2";\n\t\topp-shared;')
        for fk, uv in sorted(x for x in zip(f, v) if lo <= x[0] <= hi):
            print(f"\n\t\topp-{fk * 1000} {{\n\t\t\topp-hz = /bits/ 64 <{fk * 1000}>;"
                  f"\n\t\t\topp-microvolt = <{uv}>;\n\t\t\tclock-latency-ns = <500000>;\n\t\t}};")
        print("\t};\n")
else:
    print("ECT", b[4:8].decode(), "blocks:", ", ".join(blocks))
    for name, a in dvfs.items():
        if not name.startswith("X"):
            print(f"DVFS {name}: {dvfs_levels(a)}")
    for name in gen:
        if name.startswith("MINMAX_"):
            print(f"GEN {name} (row: idx ver min max boot resume): {gen_table(name)}")
