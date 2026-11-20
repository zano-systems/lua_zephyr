#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Statistical profiler for a Cortex-M target over J-Link.

Halts the CPU at random intervals, reads PC and LR, resumes, and maps the
samples to functions with the ELF symbol table. Build the target with
tests/async_bench and CONFIG_BENCH_PROFILE="<case>" so one workload runs in a
loop, flash it, then:

    pc_sample.py -e build/zephyr/zephyr.elf -d NRF52833_XXAA -n 2000

Self time is where the PC was; "via" names the caller from LR, which is
exact for leaf functions and a hint otherwise.

Needs pylink (tested with 1.7.0) and pyelftools. Sampling uses pylink's
private J-Link DLL handle, since JLink.halt() sleeps 1 s after each halt.
"""

import argparse
import bisect
import collections
import random
import sys
import time

import pylink
from elftools.elf.elffile import ELFFile

TESTED_PYLINK = "1.7.0"


class Symbols:
    def __init__(self, path):
        funcs = []
        with open(path, "rb") as f:
            symtab = ELFFile(f).get_section_by_name(".symtab")
            for sym in symtab.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"]:
                    funcs.append((sym["st_value"] & ~1, sym["st_size"], sym.name))
        funcs.sort()
        self.addr = [a for a, _, _ in funcs]
        self.funcs = funcs

    def name(self, pc):
        pc &= ~1
        i = bisect.bisect_right(self.addr, pc) - 1
        if i < 0:
            return "?"
        start, size, name = self.funcs[i]
        return name if size == 0 or pc < start + size else "?"


def raw_dll(jl):
    """The J-Link DLL behind pylink, or exit with a clear message."""
    dll = getattr(jl, "_dll", None)
    if dll is None or not all(hasattr(dll, f) for f in ("JLINKARM_Halt", "JLINKARM_GoEx")):
        sys.exit(f"pc_sample.py: pylink {getattr(pylink, '__version__', '?')} no longer "
                 f"exposes the J-Link DLL as JLink._dll (tested with {TESTED_PYLINK}); "
                 "install that version or update raw_dll().")
    return dll


def reg_index(jl, wanted):
    for i in jl.register_list():
        if jl.register_name(i).startswith(wanted):
            return i
    raise RuntimeError(f"no register {wanted}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-e", "--elf", required=True)
    ap.add_argument("-d", "--device", default="NRF52833_XXAA")
    ap.add_argument("-s", "--serial", type=int, help="J-Link serial number")
    ap.add_argument("-n", "--samples", type=int, default=2000)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--exclude", default="arch_cpu_idle,k_cpu_idle",
                    help="comma-separated functions left out of the percentages")
    args = ap.parse_args()

    syms = Symbols(args.elf)
    jl = pylink.JLink()
    jl.open(serial_no=args.serial)
    jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jl.connect(args.device, speed=4000)
    pc_i, lr_i = reg_index(jl, "R15"), reg_index(jl, "R14")
    dll = raw_dll(jl)
    if getattr(pylink, "__version__", None) != TESTED_PYLINK:
        print(f"note: tested with pylink {TESTED_PYLINK}, running {pylink.__version__}",
              file=sys.stderr)

    self_t = collections.Counter()
    via = collections.defaultdict(collections.Counter)
    for _ in range(args.samples):
        time.sleep(random.uniform(0.0005, 0.003))
        # JLink.halt() sleeps 1 s after halting; call the DLL directly.
        dll.JLINKARM_Halt()
        pc, lr = jl.register_read(pc_i), jl.register_read(lr_i)
        dll.JLINKARM_GoEx(0, 0)
        fn = syms.name(pc)
        self_t[fn] += 1
        via[fn][syms.name(lr)] += 1
    jl.close()

    skip = set(filter(None, args.exclude.split(",")))
    total = sum(c for f, c in self_t.items() if f not in skip)
    idle = sum(c for f, c in self_t.items() if f in skip)
    print(f"{args.samples} samples, {idle} idle ({idle * 100 / args.samples:.1f}%) excluded\n")
    print("| self % | samples | function | top caller (LR) |")
    print("|---:|---:|---|---|")
    for fn, c in self_t.most_common():
        if fn in skip:
            continue
        caller, cc = via[fn].most_common(1)[0]
        print(f"| {c * 100 / total:.1f} | {c} | `{fn}` | `{caller}` ({cc * 100 // c}%) |")
        args.top -= 1
        if args.top == 0:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
