#!/usr/bin/env python3

import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

REGIONS = [
    ("AXISRAM1",     0x34000000, 0x34100000),
    ("AXISRAM2",     0x34100000, 0x34200000),
    ("AXISRAM3..6",  0x34200000, 0x343C0000),
]
SRAM_START = min(r[1] for r in REGIONS)
SRAM_END   = max(r[2] for r in REGIONS)


def blocks_spanned(addr, end):
    """None if the section leaves implemented memory at either end."""
    if addr < SRAM_START or end > SRAM_END:
        return None
    hit = [n for (n, s, e) in REGIONS if addr < e and end > s]
    return "+".join(hit) if hit else None


def toolchain_prefix():
    hits = sorted(glob.glob("/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/"
                            "com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/"
                            "tools/bin/arm-none-eabi-readelf"))
    if hits:
        return hits[-1]
    return "arm-none-eabi-readelf"


READELF = toolchain_prefix()


def segments(elf):
    out = subprocess.check_output([READELF, "-l", elf]).decode()
    res = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 6 and f[0] == "LOAD":
            res.append((int(f[2], 16), int(f[4], 16), int(f[5], 16)))
    return res


def sections(elf):
    out = subprocess.check_output([READELF, "-S", "-W", elf]).decode()
    res = []
    for line in out.splitlines():
        if "]" not in line:
            continue
        f = line.split("]", 1)[1].split()
        if len(f) < 7:
            continue
        name, addr, size, flags = f[0], f[2], f[4], f[6]
        try:
            addr_i, size_i = int(addr, 16), int(size, 16)
        except ValueError:
            continue
        if "A" not in flags or size_i == 0 or addr_i == 0:
            continue
        res.append((name, addr_i, size_i))
    return res


def overlap(a0, a1, b0, b1):
    lo, hi = max(a0, b0), min(a1, b1)
    return (lo, hi) if lo < hi else None


def newest(pattern):
    hits = glob.glob(pattern)
    return max(hits, key=os.path.getmtime) if hits else None


def main():
    appli = newest(os.path.join(ROOT, "Appli", "Debug", "*.elf"))
    fsbl = newest(os.path.join(ROOT, "FSBL", "Debug", "*.elf"))

    if appli is None:
        sys.exit("no Appli ELF found -- run `make` first")
    if not sections(appli):
        sys.exit("parsed no allocated sections out of the Appli ELF -- the ELF "
                 "parser is broken, not the memory map")
    print("Appli: %s" % os.path.relpath(appli, ROOT))

    failures = 0

    if fsbl is None:
        print("FSBL:  no ELF found; falling back to the reserved window declared "
              "in the linker script")
        fsbl_ranges = [("FSBL (declared)", 0x34180000, 0x34200000)]
    else:
        print("FSBL:  %s" % os.path.relpath(fsbl, ROOT))
        fsbl_ranges = [(n, a, a + s) for (n, a, s) in sections(fsbl)]
        if not fsbl_ranges:
            sys.exit("parsed no allocated sections out of the FSBL ELF -- refusing "
                     "to report success on an empty comparison")
        lo = min(r[1] for r in fsbl_ranges)
        hi = max(r[2] for r in fsbl_ranges)
        print("       footprint 0x%08X..0x%08X (%d KiB)" % (lo, hi, (hi - lo) // 1024))

    print()
    print("Appli allocated sections:")
    for name, addr, size in sections(appli):
        end = addr + size
        where = blocks_spanned(addr, end)
        clash = None
        for fn, fs, fe in fsbl_ranges:
            hit = overlap(addr, end, fs, fe)
            if hit:
                clash = (fn, hit)
                break
        status = "ok"
        if where is None:
            status = "NOT IN ANY IMPLEMENTED SRAM BLOCK"
            failures += 1
        if clash:
            status = "COLLIDES WITH FSBL %s at 0x%08X..0x%08X" % (
                clash[0], clash[1][0], clash[1][1])
            failures += 1
        print("  %-18s 0x%08X..0x%08X %8d B  %-20s %s"
              % (name, addr, end, size, where or "-", status))

    print()
    print("Appli PT_LOAD segments (what the debugger writes into a running FSBL):")
    for vaddr, filesz, memsz in segments(appli):
        if filesz == 0:
            print("  0x%08X  %8d B  (NOBITS -- not downloaded)" % (vaddr, memsz))
            continue
        end = vaddr + filesz
        clash = None
        for fn, fs, fe in fsbl_ranges:
            hit = overlap(vaddr, end, fs, fe)
            if hit:
                clash = fn
                break
        if clash:
            print("  0x%08X..0x%08X  %8d B  *** OVERWRITES FSBL %s ***"
                  % (vaddr, end, filesz, clash))
            failures += 1
        else:
            print("  0x%08X..0x%08X  %8d B  ok" % (vaddr, end, filesz))

    print()
    if failures:
        print("FAILED: %d collision%s" % (failures, "" if failures == 1 else "s"))
    else:
        print("no collisions: the Appli can be downloaded while the FSBL runs")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
