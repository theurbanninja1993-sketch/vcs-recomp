#!/usr/bin/env python3
"""Map a VU1_PROFILE sample file to symbols and source lines.

vu1_replay's profiler (VU1_PROFILE=<file>) writes one u32 per SIGPROF sample: the
host RIP at the time, minus the address of the tool's own profileHandler. That
difference is what makes the file portable across runs of an ASLR'd PIE binary -
adding profileHandler's *static* address from the symbol table gives the sample's
static address, and the slide cancels.

    rip_hist.py <profile.bin> <vu1_replay-binary> [--top N] [--lines]

Two things it does that matter:

  * It keeps only samples that land inside the binary's own text. The tool starts
    a GS backend and audio threads, and their RIPs are in shared libraries, so
    their "offsets" are meaningless - before this filter, the single biggest
    bucket in a real profile was the data section. The dropped count is reported
    rather than hidden.

  * With debug info (build the tool with -g) it resolves each sample to a
    file:line as well, which is the difference between "this generated program is
    hot" and "the per-instruction budget check in it is hot". The generated VU1
    programs are one enormous function each, so a symbol-only profile of them
    says almost nothing.

This exists because vu1_replay.cpp's comment refers to it and it was never
written down. It is plain ANSI Python 3 with no dependencies.
"""

import argparse
import bisect
import collections
import struct
import subprocess
import sys


def read_samples(path):
    with open(path, "rb") as handle:
        data = handle.read()
    whole = len(data) - (len(data) % 4)
    if whole != len(data):
        print("warning: %d trailing byte(s) ignored" % (len(data) - whole), file=sys.stderr)
    return list(struct.unpack("<%dI" % (whole // 4), data[:whole]))


def read_symbols(binary):
    """(address, name) for every defined function-ish symbol, sorted by address."""
    try:
        text = subprocess.run(["nm", "--defined-only", "-C", binary],
                              capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        sys.exit("cannot read symbols from %s: %s" % (binary, error))

    symbols = []
    for line in text.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3 or parts[1] not in "tTwW":
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        symbols.append((address, parts[2]))
    symbols.sort()
    if not symbols:
        sys.exit("no text symbols in %s (is it stripped?)" % binary)
    return symbols


def find_symbol_handler(symbols):
    for address, name in symbols:
        if "profileHandler" in name:
            return address
    sys.exit("no profileHandler symbol: was the binary built from vu1_replay.cpp?")


def resolve(addresses, symbols, addresses_of):
    """Nearest preceding symbol for each address."""
    out = []
    for address in addresses:
        index = bisect.bisect_right(addresses_of, address) - 1
        out.append(symbols[index][1] if index >= 0 else "<none>")
    return out


def resolve_lines(binary, addresses):
    """addr2line for a batch of addresses, via stdin to avoid argument limits."""
    if not addresses:
        return []
    payload = "".join("0x%x\n" % a for a in addresses)
    try:
        result = subprocess.run(["addr2line", "-e", binary, "-f", "-C"],
                                input=payload, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return ["<no debug info>"] * len(addresses)
    lines = result.stdout.splitlines()
    # addr2line emits two lines per address: function, then file:line.
    out = []
    for i in range(len(addresses)):
        function = lines[2 * i] if 2 * i < len(lines) else "?"
        where = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "?"
        out.append("%s  %s" % (where, function))
    return out


def main():
    parser = argparse.ArgumentParser(description="Map a VU1_PROFILE file to symbols and lines.")
    parser.add_argument("profile")
    parser.add_argument("binary")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--lines", action="store_true", help="also group by file:line (needs -g)")
    parser.add_argument("--only", help="keep only samples whose symbol contains this text, e.g. vu1p_")
    parser.add_argument("--raw", action="store_true", help="print the address of every sample")
    args = parser.parse_args()

    samples = read_samples(args.profile)
    if not samples:
        sys.exit("no samples in %s (was VU1_PROFILE set for long enough?)" % args.profile)

    symbols = read_symbols(args.binary)
    addresses_of = [address for address, _ in symbols]
    base = find_symbol_handler(symbols)
    text_lo, text_hi = addresses_of[0], addresses_of[-1]

    addresses = []
    dropped = 0
    for offset in samples:
        address = base + offset
        if not text_lo <= address <= text_hi:
            dropped += 1
            continue
        addresses.append(address)

    print("%d samples, %d inside this binary, %d elsewhere (other threads, libraries)"
          % (len(samples), len(addresses), dropped))
    if not addresses:
        sys.exit("nothing landed in the binary's text: is profileHandler the right symbol?")

    if args.raw:
        for address in addresses:
            print("0x%x" % address)
        return

    names = resolve(addresses, symbols, addresses_of)
    if args.only:
        # The profiler samples every thread, and the tool runs a GS backend of its
        # own, so a whole-process histogram is mostly GS. Restricting by symbol is
        # how the VU1 work is isolated without needing per-sample thread ids.
        kept = [(a, n) for a, n in zip(addresses, names) if args.only in n]
        print("%d of %d samples match '%s'" % (len(kept), len(addresses), args.only))
        if not kept:
            sys.exit("nothing matched; the symbols to look for are vu1p_ and VU1Interpreter")
        addresses = [a for a, _ in kept]
        names = [n for _, n in kept]

    labelled = list(zip(addresses, names))
    if args.lines:
        resolved = resolve_lines(args.binary, addresses)
        labelled = list(zip(addresses, resolved))

    print("\ntop %d:" % args.top)
    for label, count in collections.Counter(label for _, label in labelled).most_common(args.top):
        print("  %5.1f%%  %s" % (100.0 * count / len(addresses), label[:150]))


if __name__ == "__main__":
    main()
