#!/usr/bin/env bash
# Builds one game from the player's own disc, driven by that game's manifest.
#
#   ./build.sh --image <disc|image|folder> [--game <dir>] [options]
#
#     --image PATH   the source: an optical drive (/dev/sr0, E:), an .iso, a raw
#                    .bin, a .cue sheet naming one of those, a .chd, or an
#                    already-extracted folder (skips the extraction).
#     --game DIR     the game's manifest folder (default: the only games/*/ that
#                    has a game.toml, so a single-game checkout needs no flag).
#     --out DIR      where the finished game folder goes (default: $PWD/dist)
#     --work DIR     scratch for the intermediate steps (default: $PWD/build)
#     --from N       start at stage N (resume a long build)
#     --to N         stop after stage N
#     --windows      also cross-compile the Windows build (Linux only: it needs a
#                    Linux-hosted toolchain, which is fetched once if missing)
#     --toolchain D  use this llvm-mingw instead of fetching one
#     --dry-run      print what each stage would do, and change nothing
#     --optimize     compile the VU programs the game recorded while being played
#                    (<game folder>/<platform>/config/vu_learn, or the user config
#                    folder) into the vu1_programs plugin next to the game; needs
#                    a finished build (same --out/--work), not the disc
#     --learned DIR  another folder of recordings for --optimize
#
# Stages:
#   0 check      is this the right disc? (manifest hash vs the disc's boot file)
#   1 extract    pull the files off it
#   2 recompile  turn the game's ELF into C++ (ps2_recomp)
#   3 install    put that C++ into the runtime
#   4 build      compile the port
#   5 vu1        learn the VU1 microprograms by running the game, then compile them
#   6 assemble   make the portable folder
#
# Nothing about the game is in this script: it all comes from the manifest, so a
# second game needs a manifest and nothing else. See games/vcs/game.toml.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE=""
GAME_DIR=""
OUT="$PWD/dist"
WORK="$PWD/build"
FROM=0
TO=99
DRY_RUN=0
WINDOWS=0
TOOLCHAIN=""
OPTIMIZE=0
LEARNED=""

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift ;;
        --game) GAME_DIR="$2"; shift ;;
        --out) OUT="$2"; shift ;;
        --work) WORK="$2"; shift ;;
        --from) FROM="$2"; shift ;;
        --to) TO="$2"; shift ;;
        --windows) WINDOWS=1 ;;
        --toolchain) TOOLCHAIN="$2"; shift ;;
        --dry-run) DRY_RUN=1 ;;
        --optimize) OPTIMIZE=1 ;;
        --learned) LEARNED="$2"; shift ;;
        -h|--help) sed -n '2,37p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
run()  { if [ "$DRY_RUN" -eq 1 ]; then printf '    [dry-run] %s\n' "$*"; else "$@"; fi; }
die()  { printf '\n\033[31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- the manifest
# The manifest is ours and flat, so a small reader is enough - and keeping it in
# python means no TOML dependency and no version to be too old for. Values are
# shell-quoted, so evaluating the output cannot run anything.
read_manifest() {
    local values
    values=$(python3 - "$1" <<'PY'
import re, shlex, sys
section = ""
for line in open(sys.argv[1]):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    header = re.match(r"^\[(.+)\]$", line)
    if header:
        section = header.group(1).strip()
        continue
    pair = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$", line)
    if not pair:
        continue
    key, value = pair.group(1), pair.group(2)
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    # shell names cannot contain dots
    print(f'{section}_{key}={shlex.quote(value)}')
PY
)
    eval "$values"
}

manifest_require() {
    local name="$1" value
    eval "value=\${$name:-}"
    [ -n "$value" ] || die "the manifest does not set $name"
}

# Pick the game: an explicit --game, or the only manifest in games/.
if [ -z "$GAME_DIR" ]; then
    found=$(find "$ROOT/games" -maxdepth 2 -name game.toml 2>/dev/null | sort)
    count=$(printf '%s\n' "$found" | grep -c . || true)
    if [ "$count" -eq 1 ]; then
        GAME_DIR="$(dirname "$found")"
    elif [ "$count" -eq 0 ]; then
        die "no game.toml under $ROOT/games (is this the right checkout?)"
    else
        printf 'several games are available; pick one with --game:\n' >&2
        printf '%s\n' "$found" | sed "s|^$ROOT/||; s|/game.toml$||; s|^|  |" >&2
        exit 1
    fi
fi
MANIFEST="$GAME_DIR/game.toml"
[ -f "$MANIFEST" ] || die "no manifest at $MANIFEST"

game_name=""; game_boot_file=""; game_boot_hash_xxh3_128=""
recompiler_config=""; recompiler_function_map=""
runtime_override=""
vu1_learn_passes=""; vu1_learn_pad_script=""; vu1_learn_frames=""
read_manifest "$MANIFEST"
manifest_require game_boot_file
manifest_require game_boot_hash_xxh3_128
manifest_require recompiler_config
manifest_require recompiler_function_map
vu1_learn_passes="${vu1_learn_passes:-2}"

# The manifest's paths are relative to the manifest, which is what makes a game
# folder self-contained.
game_config="$GAME_DIR/$recompiler_config"
game_map="$GAME_DIR/$recompiler_function_map"
[ -f "$game_config" ] || die "missing $game_config"
[ -f "$game_map" ] || die "missing $game_map"
# A cheap sanity check that the runtime really knows this game.
if [ -n "$runtime_override" ] && ! grep -rqsF "$runtime_override" "$ROOT/ps2xRuntime/src/lib" 2>/dev/null; then
    note "warning: the runtime does not mention '$runtime_override'; the game may run without overrides"
fi

# ---------------------------------------------------------------- paths
PS2RECOMP="$ROOT"
ps2_recomp="$ROOT/build/ps2xRecomp/ps2_recomp"
ps2_iso="$ROOT/build/ps2xBuilder/ps2x_iso"
vu1_recomp="$ROOT/build/ps2xRuntime/vu1_recomp"
runner_dir="$ROOT/ps2xRuntime/src/runner"
include_dir="$ROOT/ps2xRuntime/include"
disc_dir="$WORK/disc"
generated="$WORK/generated"
learn="$WORK/vu1_learn"
vu1_dir="$WORK/vu1"

need_tool() {
    [ -x "$1" ] || die "missing tool $1 (configure and build first: cmake -S $ROOT -B $ROOT/build)"
    return 0
}
stage_start=0
stage_begin() { stage_start=$(date +%s); say "$*"; }
stage_end()   { [ "$DRY_RUN" -eq 1 ] || note "took $(( $(date +%s) - stage_start ))s"; }

# ---------------------------------------------------------------- optimize
# --optimize: compile the VU programs the game recorded while it was played.
# A program the build never saw runs interpreted (about ten times slower), so the
# runtime records each one it meets (<config>/vu_learn/vu<unit>_learn_*.bin plus
# a PENDING marker; see ps2VuLearnFile in ps2_runtime.cpp). This turns every
# recording made while playing (all sessions so far) into the vu1_programs.so /
# .dll plugin next to the game, which it loads at startup: no relink, a minute
# or so. The played recordings are kept in $WORK/vu_played, so
# a later full rebuild (stage 5) includes them too.
if [ "$OPTIMIZE" -eq 1 ]; then
    say "optimizing: compiling the VU programs recorded while playing"
    played="$WORK/vu_played"
    sources=()
    [ -n "$LEARNED" ] && sources+=("$LEARNED")
    sources+=("$OUT/linux/config/vu_learn" "$OUT/windows/config/vu_learn" "$OUT/config/vu_learn"
              "${XDG_CONFIG_HOME:-$HOME/.config}/ps2recomp/vu_learn")
    new=0
    for dir in "${sources[@]}"; do
        [ -d "$dir" ] || continue
        while IFS= read -r file; do
            note "recorded: $file"
            if [ "$DRY_RUN" -eq 0 ]; then
                mkdir -p "$played"
                # The file names carry a timestamp, so recordings from different
                # sessions or folders do not collide.
                mv "$file" "$played/$(basename "$file")"
            fi
            new=$((new + 1))
        done < <(find "$dir" -maxdepth 1 -name 'vu[01]_learn_*.bin' -size +0 | sort)
        [ "$DRY_RUN" -eq 1 ] || find "$dir" -maxdepth 1 \( -name PENDING -o -name 'vu[01]_learn_*.bin' \) -delete
    done
    if [ "$new" -eq 0 ]; then
        note "nothing new was recorded: every program the game has run is compiled"
        exit 0
    fi
    if [ ! -x "$vu1_recomp" ]; then
        run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DCMAKE_BUILD_TYPE=Release
        run cmake --build "$PS2RECOMP/build" --target vu1_recomp -j "$(nproc)"
    fi
    plugin_dir="$WORK/vu1_plugin"
    # Only what was recorded while playing: those are exactly the programs the
    # linked build lacks, so the plugin never shadows a linked program (the
    # plugin's calls go through an API table and are slower than linked ones).
    mapfile -t recordings < <(find "$played" -name 'vu[01]_learn_*.bin' -size +0 2>/dev/null | sort)
    note "${#recordings[@]} recording(s), $new of them new"
    if [ "$DRY_RUN" -eq 0 ]; then
        rm -rf "$plugin_dir"; mkdir -p "$plugin_dir"
        "$vu1_recomp" "$plugin_dir" "${recordings[@]}" || die "vu1_recomp failed"
        [ -f "$plugin_dir/vu1_programs.cpp" ] || die "vu1_recomp wrote no registry"
        note "$(ls "$plugin_dir"/vu1p_*.cpp 2>/dev/null | wc -l) program file(s)"
    fi
    run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DPS2X_VU1_PLUGIN_PROGRAMS="$plugin_dir"
    run cmake --build "$PS2RECOMP/build" --target ps2_vu1_plugin -j "$(nproc)"
    if [ -d "$OUT/linux" ]; then
        run cp "$PS2RECOMP/build/ps2xRuntime/vu1_programs.so" "$OUT/linux/"
        note "installed $OUT/linux/vu1_programs.so"
    fi
    if [ -f "$PS2RECOMP/build-win/CMakeCache.txt" ] && [ -d "$OUT/windows" ]; then
        run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build-win" -DPS2X_VU1_PLUGIN_PROGRAMS="$plugin_dir"
        run cmake --build "$PS2RECOMP/build-win" --target ps2_vu1_plugin -j "$(nproc)"
        run cp "$PS2RECOMP/build-win/ps2xRuntime/vu1_programs.dll" "$OUT/windows/"
        note "installed $OUT/windows/vu1_programs.dll"
    fi
    printf '\n\033[32mOptimize complete: restart the game to use the new programs.\033[0m\n'
    exit 0
fi

say "GTA PS2 build pipeline"
note "game  : $game_name (${game_boot_file})"
note "from  : ${IMAGE:-<none>}"
note "out   : $OUT"
note "work  : $WORK"
note "stages: $FROM..$TO$([ "$DRY_RUN" -eq 1 ] && echo " (dry run)")"
[ -n "$IMAGE" ] || die "no --image given"

mkdir -p "$WORK"

# ---------------------------------------------------------------- tools
# Every stage below calls a tool out of the build tree, so they have to exist
# before the first one runs. On a fresh clone this is where they are built; after
# that cmake finds nothing to do and it costs a couple of seconds. It is not a
# numbered stage because it is a precondition rather than a step of the build.
needs_tools=0
for stage in 0 1 2 4 5; do
    if [ "$FROM" -le "$stage" ] && [ "$TO" -ge "$stage" ]; then
        needs_tools=1
    fi
done
if [ "$needs_tools" -eq 1 ]; then
    stage_begin "tools (configuring and building what the stages call)"
    note "$ps2_recomp"
    note "$ps2_iso"
    note "$vu1_recomp"
    run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DCMAKE_BUILD_TYPE=Release
    run cmake --build "$PS2RECOMP/build" --target ps2_recomp ps2x_iso vu1_recomp -j "$(nproc)"
    for tool in "$ps2_recomp" "$ps2_iso" "$vu1_recomp"; do
        [ "$DRY_RUN" -eq 1 ] || [ -x "$tool" ] || die "$tool did not build"
    done
    stage_end
fi

# ---------------------------------------------------------------- windows
# The Windows build is a cross-compile, so its compiler runs here rather than
# there: llvm-mingw, a Linux-hosted toolchain. It is a one-time 88 MB download and
# it goes inside the source, which keeps a machine's copy with the tree that uses
# it. An existing toolchain is preferred over fetching one, including the one an
# already-configured build-win was built with.
llvm_mingw_version="20241217"
llvm_mingw_asset="llvm-mingw-${llvm_mingw_version}-ucrt-ubuntu-20.04-x86_64"
llvm_mingw_url="https://github.com/mstorsjo/llvm-mingw/releases/download/${llvm_mingw_version}/${llvm_mingw_asset}.tar.xz"
toolchain_dir="$PS2RECOMP/toolchains"

toolchain_works() { [ -n "${1:-}" ] && [ -x "$1/bin/x86_64-w64-mingw32-clang++" ]; }

ensure_toolchain() {
    if toolchain_works "$TOOLCHAIN"; then
        note "llvm-mingw: $TOOLCHAIN (given)"
        return 0
    fi
    if toolchain_works "${LLVM_MINGW_ROOT:-}"; then
        TOOLCHAIN="$LLVM_MINGW_ROOT"
        note "llvm-mingw: $TOOLCHAIN (from LLVM_MINGW_ROOT)"
        return 0
    fi
    if [ -f "$PS2RECOMP/build-win/CMakeCache.txt" ]; then
        local cached
        cached=$(sed -n 's/^LLVM_MINGW_ROOT:[^=]*=//p' "$PS2RECOMP/build-win/CMakeCache.txt" | head -1)
        if toolchain_works "$cached"; then
            TOOLCHAIN="$cached"
            note "llvm-mingw: $TOOLCHAIN (the one this build-win used)"
            return 0
        fi
    fi
    if toolchain_works "$toolchain_dir/$llvm_mingw_asset"; then
        TOOLCHAIN="$toolchain_dir/$llvm_mingw_asset"
        note "llvm-mingw: $TOOLCHAIN (already fetched)"
        return 0
    fi
    note "fetching llvm-mingw, 88 MB, once"
    run mkdir -p "$toolchain_dir"
    run curl -fL --retry 3 -o "$toolchain_dir/$llvm_mingw_asset.tar.xz" "$llvm_mingw_url"
    run tar -xJf "$toolchain_dir/$llvm_mingw_asset.tar.xz" -C "$toolchain_dir"
    TOOLCHAIN="$toolchain_dir/$llvm_mingw_asset"
    if [ "$DRY_RUN" -eq 0 ] && ! toolchain_works "$TOOLCHAIN"; then
        die "llvm-mingw did not unpack where expected ($TOOLCHAIN)"
    fi
}

wants_windows=0
if [ "$WINDOWS" -eq 1 ]; then
    for stage in 4 5 6; do
        if [ "$FROM" -le "$stage" ] && [ "$TO" -ge "$stage" ]; then
            wants_windows=1
        fi
    done
fi
if [ "$WINDOWS" -eq 1 ] && [ "$wants_windows" -eq 0 ]; then
    note "--windows given but no stage that uses it is in range; nothing to do"
fi
if [ "$wants_windows" -eq 1 ]; then
    stage_begin "windows (configuring the cross-compile)"
    ensure_toolchain
    # Crossing needs the host's own build too: the VU1 microprograms are learnt by
    # *running the game*, and the Linux runner is the one that can run here.
    run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build-win" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$PS2RECOMP/cmake/toolchains/llvm-mingw-x86_64.cmake" \
        -DLLVM_MINGW_ROOT="$TOOLCHAIN"
    stage_end
fi

# ---------------------------------------------------------------- stage 0
if [ "$FROM" -le 0 ] && [ "$TO" -ge 0 ]; then
    stage_begin "0. checking the disc"
    if [ -d "$IMAGE" ]; then
        [ -f "$IMAGE/$game_boot_file" ] || die "no $game_boot_file in $IMAGE"
        need_tool "$ps2_iso"
        hash=$("$ps2_iso" hashfile "$IMAGE/$game_boot_file" | cut -d' ' -f1)
        note "extracted folder; boot $game_boot_file"
    else
        need_tool "$ps2_iso"
        info=$("$ps2_iso" check "$IMAGE" --boot "$game_boot_file" 2>&1) || die "can't read $IMAGE"
        printf '%s\n' "$info" | sed 's/^/    /'
        hash=$(printf '%s\n' "$info" | sed -n 's/.*boot hash: \([0-9a-f]*\).*/\1/p')
        [ -n "$hash" ] || die "could not read this disc (the reason is above)"
    fi
    [ "$hash" = "$game_boot_hash_xxh3_128" ] || die "this disc is not $game_name (boot hash $hash)"
    note "recognised: $game_name"
    stage_end
fi

# ---------------------------------------------------------------- stage 1
if [ "$FROM" -le 1 ] && [ "$TO" -ge 1 ]; then
    stage_begin "1. extracting the disc"
    if [ -d "$IMAGE" ]; then
        note "already extracted; linking"
    else
        need_tool "$ps2_iso"
        # Straight off the disc rather than image-then-extract: a 4 GB disc reads
        # at ~4 MB/s, so a second pass would double the wait.
        run "$ps2_iso" extract "$IMAGE" "$disc_dir"
    fi
    stage_end
fi

# A folder source needs no extraction, so link it here rather than inside stage 1:
# a --from that skips stage 1 still needs $disc_dir to resolve.
if [ "$DRY_RUN" -eq 0 ] && [ -d "$IMAGE" ]; then
    mkdir -p "$(dirname "$disc_dir")"
    ln -sfn "$(cd "$IMAGE" && pwd)" "$disc_dir"
fi

# ---------------------------------------------------------------- stage 2
if [ "$FROM" -le 2 ] && [ "$TO" -ge 2 ]; then
    stage_begin "2. recompiling the game's ELF"
    need_tool "$ps2_recomp"
    # The shipped config names the map and the output; the input and output paths
    # are the player's, so rewrite those three keys rather than hand-editing.
    work_config="$WORK/vcs_clean.toml"
    note "config: $game_config -> $work_config"
    if [ "$DRY_RUN" -eq 0 ]; then
        python3 - "$game_config" "$work_config" "$disc_dir/$game_boot_file" "$generated" "$game_map" <<'PY'
import re, sys
src, dst, elf, out, csv = sys.argv[1:6]
text = open(src).read()
def set_key(text, key, value):
    return re.sub(r'(?m)^\s*' + key + r'\s*=.*$', f'{key} = "{value}"', text, count=1)
text = set_key(text, 'input', elf)
text = set_key(text, 'output', out + '/')
text = set_key(text, 'ghidra_output', csv)
open(dst, 'w').write(text)
PY
        rm -rf "$generated"; mkdir -p "$generated"
        ( cd "$ROOT" && "$ps2_recomp" "$work_config" ) || die "ps2_recomp failed"
        count=$(find "$generated" -name '*.cpp' | wc -l)
        [ "$count" -gt 0 ] || die "the recompiler produced nothing"
        note "generated $count .cpp files"
    else
        note "[dry-run] rewrite the config and run ps2_recomp"
    fi
    stage_end
fi

# ---------------------------------------------------------------- stage 3
if [ "$FROM" -le 3 ] && [ "$TO" -ge 3 ]; then
    stage_begin "3. installing the generated code into the runtime"
    [ -d "$generated" ] || [ "$DRY_RUN" -eq 1 ] || die "no generated code at $generated (run stage 2)"
    # Copy, never clean: src/runner also holds main.cpp, which is the runtime's.
    # And copy only what differs - the build keys off timestamps, so rewriting
    # identical files would force a full rebuild every single run.
    if [ "$DRY_RUN" -eq 0 ]; then
        mkdir -p "$runner_dir" "$include_dir"
        changed_skip=0 changed_copy=0
        while IFS= read -r f; do
            name="$(basename "$f")"
            if [ -f "$include_dir/$name" ] && cmp -s "$f" "$include_dir/$name"; then
                changed_skip=$((changed_skip + 1))
            else
                cp -f "$f" "$include_dir/$name"; changed_copy=$((changed_copy + 1))
            fi
        done < <(find "$generated" -maxdepth 1 -type f -name '*.h')
        while IFS= read -r f; do
            name="$(basename "$f")"
            if [ -f "$runner_dir/$name" ] && cmp -s "$f" "$runner_dir/$name"; then
                changed_skip=$((changed_skip + 1))
            else
                cp -f "$f" "$runner_dir/$name"; changed_copy=$((changed_copy + 1))
            fi
        done < <(find "$generated" -maxdepth 1 -type f -name '*.cpp')
        note "updated $changed_copy, left $changed_skip unchanged"
        note "runner sources: $(find "$runner_dir" -name '*.cpp' | wc -l)"
    else
        note "[dry-run] copy the generated sources into $runner_dir"
    fi
    stage_end
fi

# ---------------------------------------------------------------- stage 4
if [ "$FROM" -le 4 ] && [ "$TO" -ge 4 ]; then
    stage_begin "4. building the port"
    # Configure even when the cache exists: the VU1 directory only becomes
    # populated at stage 5, and re-configuring is what lets the build pick it up.
    note "configuring"
    run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DCMAKE_BUILD_TYPE=Release -DPS2X_VU1_PROGRAMS="$vu1_dir"
    run cmake --build "$PS2RECOMP/build" --target ps2EntryRunner -j "$(nproc)"
    # The Windows build is *not* done here. It is a cross-compile, and it has to
    # come after stage 5 for a reason that is not obvious: ps2_vu1_programs is a
    # link dependency of the runner, and without it on the link line the resolver
    # takes raylib's CloseWindow/ShowCursor from USER32 instead and stops with
    # "duplicate symbol". Stage 5 ends by configuring build-win with the VU1
    # directory and building it, which is where it belongs and the only place it
    # needs doing.
    stage_end
fi

# ---------------------------------------------------------------- stage 5
# The VU1 microprograms come from the game itself, so the only honest way to
# compile them is to run it and record what it asks for. Pass 1 starts from
# nothing; its programs are compiled before pass 2, which then gets further and
# asks for ones the first never reached - that is what closes the gap.
if [ "$FROM" -le 5 ] && [ "$TO" -ge 5 ]; then
    stage_begin "5. learning the VU1 microprograms"
    need_tool "$vu1_recomp"
    runner="$PS2RECOMP/build/ps2xRuntime/ps2EntryRunner"
    [ -x "$runner" ] || [ "$DRY_RUN" -eq 1 ] || die "no runner to learn from (run stage 4)"
    if [ "$DRY_RUN" -eq 0 ]; then rm -rf "$learn"; mkdir -p "$learn"; fi

    for pass in $(seq 1 "${vu1_learn_passes:-2}"); do
        note "pass $pass: recording what the game asks for"
        uncompiled=""
        [ "$pass" -eq 1 ] && uncompiled="PS2X_VU1_NO_COMPILED=1"   # pass 1 must start from nothing
        if [ "$DRY_RUN" -eq 0 ]; then
            # VU0 programs are recorded too (PS2X_VU0_CAPTURE): without them every
            # VU0 call is interpreted on the game thread. Pass 1 runs everything
            # interpreted, so it is slow - hence the long timeout.
            ( cd "$learn" && env $uncompiled PS2X_HIDDEN_WINDOW=1 PS2X_NO_LAUNCH_MENU=1 \
                  PS2X_VU1_LEARN="$learn" PS2X_VU0_CAPTURE="$learn/vu0_learn_pass$pass.bin" \
                  PS2X_PAD_SCRIPT="$vu1_learn_pad_script" \
                  ${vu1_learn_frames:+PS2X_EXIT_AFTER_FRAME="$vu1_learn_frames"} \
                  timeout 900 "$runner" "$disc_dir/$game_boot_file" >"$learn/pass$pass.log" 2>&1 ) || \
                note "pass $pass ended early (fine if it recorded something)"
            note "  $(find "$learn" -name 'vu1_learn_*.bin' | wc -l) capture(s) total"
            rm -rf "$vu1_dir"; mkdir -p "$vu1_dir"
            # vu1_recomp takes an OUTPUT DIRECTORY - the usage comment in its
            # source says <out.cpp> and is stale - and writes vu1p_<key>.cpp per
            # program plus the registry vu1_programs.cpp.
            # Every recording so far (a later pass only adds what is still
            # missing); empty files are passes that found nothing new.
            # Plus anything recorded while playing and compiled by --optimize.
            # Plus $WORK/vu_pinned: full-playthrough captures that must always be in
            # the set. A scripted learn route only reaches the intro, so without them
            # a rebuild quietly drops back to ~21 programs and everything else runs
            # interpreted (about ten times slower). Symlinks are fine there.
            mapfile -t recordings < <({ find "$learn" "$WORK/vu_played" -name 'vu[01]_learn_*.bin' -size +0 2>/dev/null
                                        find -L "$WORK/vu_pinned" -name '*.bin' -size +0 2>/dev/null; } | sort)
            "$vu1_recomp" "$vu1_dir" "${recordings[@]}" || die "vu1_recomp failed"
            [ -f "$vu1_dir/vu1_programs.cpp" ] || die "vu1_recomp wrote no registry"
            note "  $(ls "$vu1_dir"/vu1p_*.cpp 2>/dev/null | wc -l) program file(s)"
            # Re-configure as well as build: the CMake only adds the VU1 sources
            # if vu1_programs.cpp existed when it last configured.
            cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DCMAKE_BUILD_TYPE=Release \
                  -DPS2X_VU1_PROGRAMS="$vu1_dir" >/dev/null
            cmake --build "$PS2RECOMP/build" --target ps2EntryRunner -j "$(nproc)"
        else
            printf '    [dry-run] run the game with PS2X_VU1_LEARN (pad script "%s", %s frames), then vu1_recomp and rebuild\n' \
                   "$vu1_learn_pad_script" "${vu1_learn_frames:-until the timeout}"
        fi
    done
    if [ "$DRY_RUN" -eq 0 ] && [ "$(find "$learn" -name 'vu1_learn_*.bin' | wc -l)" -eq 0 ]; then
        die "no VU1 captures recorded; the game may not have reached gameplay"
    fi
    # Windows once, at the end - and it must be after this stage, not before it:
    # the VU1 programs are a link dependency of the runner, and a Windows link
    # without them resolves raylib's CloseWindow/ShowCursor against USER32's and
    # fails with "duplicate symbol". The passes themselves only need the Linux
    # runner, so this is also the only time the Windows build is needed.
    if [ -f "$PS2RECOMP/build-win/CMakeCache.txt" ]; then
        run cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build-win" -DPS2X_VU1_PROGRAMS="$vu1_dir"
        run cmake --build "$PS2RECOMP/build-win" --target ps2EntryRunner -j "$(nproc)"
    fi
    stage_end
fi

# ---------------------------------------------------------------- stage 6
if [ "$FROM" -le 6 ] && [ "$TO" -ge 6 ]; then
    stage_begin "6. assembling the portable folder"
    make_portable="$ROOT/tools/make_portable_build.sh"
    [ -x "$make_portable" ] || die "missing $make_portable"
    # Only ask for what was actually built. A configured build-win does not mean a
    # Windows binary exists - --windows configures it before anything is compiled -
    # and asking for one that is not there aborted the whole run after the Linux
    # folder had already been produced.
    make_platforms="--linux"
    if [ -f "$PS2RECOMP/build-win/ps2xRuntime/ps2EntryRunner.exe" ]; then
        make_platforms="--linux --windows"
    fi
    run "$make_portable" $make_platforms --out "$OUT" --game "$disc_dir"
    [ "$DRY_RUN" -eq 1 ] || note "game folder: $OUT"
    stage_end
fi

if [ "$DRY_RUN" -eq 1 ]; then
    printf '\n\033[32mDry run: nothing was changed.\033[0m\n'
else
    printf '\n\033[32mPipeline complete.\033[0m\n'
    note "game: $OUT"
fi
