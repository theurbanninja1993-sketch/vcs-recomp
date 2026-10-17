#!/usr/bin/env bash
# Merge the .profraw files a clang -fprofile-instr-generate build wrote into the
# default.profdata that PS2X_PGO=use expects, so the two halves of the clang PGO route
# are in one place.
#
#   cmake -B build -DPS2X_PGO=generate ...           # build instrumented
#   LLVM_PROFILE_FILE=<dir>/%p.profraw ./build/...   # run the game and vu1_replay
#   tools/make_pgo_profile.sh <dir>                  # this: merge into <dir>
#   cmake -B build -DPS2X_PGO=use ...                # rebuild with it
#
# A profile is platform specific. The clang route usually transfers between an x86-64
# host build and the llvm-mingw cross build of the same source, but the honest profile
# is the one collected by running the game on the machine being built for.
#
# usage: make_pgo_profile.sh <profile-dir> [llvm-profdata]
set -euo pipefail

dir="${1:-build/pgo}"
profdata="${2:-}"

if [ -z "$profdata" ]; then
    profdata="$(command -v llvm-profdata || true)"
fi
if [ -z "$profdata" ] || [ ! -x "$profdata" ]; then
    # llvm-mingw keeps it beside the compilers it built the runtime for.
    for candidate in "$dir"/../toolchains/*/bin/llvm-profdata "${PS2X_LLVM_MINGW_ROOT:-}/bin/llvm-profdata"; do
        [ -x "$candidate" ] && profdata="$candidate" && break
    done
fi
if [ -z "$profdata" ] || [ ! -x "$profdata" ]; then
    echo "make_pgo_profile.sh: no llvm-profdata found; pass its path as the second" >&2
    echo "argument (llvm-mingw keeps it in <toolchain>/bin)." >&2
    exit 1
fi

count=$(find "$dir" -maxdepth 1 -name '*.profraw' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "make_pgo_profile.sh: no .profraw files in $dir - was the instrumented binary" >&2
    echo "run with LLVM_PROFILE_FILE pointing there?" >&2
    exit 1
fi

# A file per process is what LLVM_PROFILE_FILE's %p gives, and profraw merging wants
# them all at once.
"$profdata" merge -o "$dir/default.profdata" "$dir"/*.profraw
echo "make_pgo_profile.sh: merged $count profile file(s) into $dir/default.profdata"
