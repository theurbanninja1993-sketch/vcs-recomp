#!/usr/bin/env bash
# Assembles a self-contained, portable build for a USB stick.
#
#   tools/make_portable_build.sh [options]
#
#     --linux | --windows | --both   which builds to assemble (default: both)
#     --out DIR                      output root (default: dist/)
#     --game DIR                     extracted disc files (required unless --no-game-data)
#     --no-game-data                 do not copy the disc files
#     --link-game                    symlink the disc files instead of copying
#                                    (handy for testing; a stick needs a copy)
#
# Each platform lands in <out>/<platform>/ as:
#
#   ps2EntryRunner[.exe]   the port
#   game/                  the disc's files (the boot file is found by name)
#   prompts/               button-prompt packs
#   music/                 the extra radio station's tracks
#   mods/                  texture/cheat mods
#   config/                created on first run (portable mode)
#   portable.txt           marks the folder as portable: settings, generated
#                          prompt art and logs stay in config/ next to the port
#   run.sh / play.cmd      launcher
#
# On Linux the libraries the host may not carry (FFmpeg, libxml2, the DVD and
# module-music readers) are bundled in lib/ and found through LD_LIBRARY_PATH.
# The GL and X11 stack is deliberately not bundled: those must come from the
# host, and a mismatched copy would be worse than none.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$PWD/dist"
GAME=""
DO_LINUX=0 DO_WINDOWS=0 DO_GAME=1 LINK_GAME=0

while [ $# -gt 0 ]; do
    case "$1" in
        --linux) DO_LINUX=1 ;;
        --windows) DO_WINDOWS=1 ;;
        --both) DO_LINUX=1; DO_WINDOWS=1 ;;
        --out) OUT="$2"; shift ;;
        --game) GAME="$2"; shift ;;
        --no-game-data) DO_GAME=0 ;;
        --link-game) LINK_GAME=1 ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
[ $DO_LINUX -eq 0 ] && [ $DO_WINDOWS -eq 0 ] && { DO_LINUX=1; DO_WINDOWS=1; }
if [ $DO_GAME -eq 1 ] && [ -z "$GAME" ]; then
    echo "--game is required: point it at the extracted disc files (build.sh passes it)" >&2
    exit 1
fi

say() { printf '%s\n' "$*"; }

# Clears a platform folder before writing it, but keeps config/: that is the
# player's settings, and re-running this script to update a build must not
# throw them away.
# Only a folder this script made (it carries a marker) or an empty one is
# cleared: --out pointed somewhere unexpected (a home folder that happens to
# have a "linux" directory) must not lose unrelated files.
reset_dest() {
    local dest="$1" marker=".ps2x-portable-build"
    mkdir -p "$dest"
    # A build made before the marker existed is recognised by its executable.
    if [ -n "$(ls -A "$dest")" ] && [ ! -e "$dest/$marker" ] && [ ! -e "$dest/ps2EntryRunner" ] &&
        [ ! -e "$dest/ps2EntryRunner.exe" ]; then
        echo "refusing to clear $dest: it has files this script did not put there" >&2
        echo "(choose an empty --out, or delete the folder first)" >&2
        exit 1
    fi
    find "$dest" -mindepth 1 -maxdepth 1 ! -name config ! -name "$marker" -exec rm -rf {} +
    : > "$dest/$marker"
}

# Assets the port reads from its own folder (see ps2ExecutableDirectory users).
copy_assets() {
    local dest="$1" src="$2"
    # -L: follow symlinks. The build tree may hold mods as links to a folder
    # elsewhere, and a stick (exFAT, NTFS) cannot store a symlink at all.
    # The achievement set, read from beside the runner. It is the game's own
    # content, like the prompt art: no game code and nothing of Rockstar's.
    if [ -e "$GAME/achievements.toml" ]; then
        cp "$GAME/achievements.toml" "$dest/achievements.toml"
    elif [ -e "$GAME/games" ]; then
        set_file=$(find "$GAME/games" -maxdepth 2 -name achievements.toml | head -1)
        [ -n "$set_file" ] && cp "$set_file" "$dest/achievements.toml"
    fi

    for item in prompts music mods custom_logo.png vu1_programs.so vu1_programs.dll; do
        [ -e "$src/$item" ] && cp -rL "$src/$item" "$dest/" || true
    done
    mkdir -p "$dest/mods"
}

copy_game() {
    local dest="$1"
    if [ $DO_GAME -eq 0 ]; then
        mkdir -p "$dest/game"
        say "    game/            (skipped: --no-game-data)"
        return
    fi
    if [ $LINK_GAME -eq 1 ]; then
        ln -sfn "$(cd "$GAME" && pwd)" "$dest/game"
        say "    game/            -> $GAME (symlink)"
        return
    fi
    say "    game/            copying $(du -shL "$GAME" | cut -f1) ..."
    mkdir -p "$dest/game"
    cp -rL "$GAME/." "$dest/game/"
}

# ---------------------------------------------------------------- Linux
build_linux() {
    local dest="$OUT/linux" src="$REPO/build/ps2xRuntime"
    say "Linux -> $dest"
    reset_dest "$dest"; mkdir -p "$dest/lib"

    cp "$src/ps2EntryRunner" "$dest/ps2EntryRunner"
    strip "$dest/ps2EntryRunner" 2>/dev/null || true
    copy_assets "$dest" "$src"

    # Only the C++ runtime is bundled. It is self-contained and the one thing a
    # host is genuinely likely to be older at; everything else (the media stack,
    # the SVG/GTK stack, X11, GL) is deliberately left to the host. Dragging in
    # the whole transitive closure would copy the dynamic loader, the GL stack
    # and systemd from the build machine, which is far more likely to break the
    # target than to help it.
    local bundled=0
    local ldds
    ldds=$(ldd "$dest/ps2EntryRunner" 2>/dev/null || true)
    for lib in libstdc++.so.6 libgcc_s.so.1; do
        local path
        path=$(printf '%s\n' "$ldds" | awk -v want="$lib" '$1 == want { print $3; exit }') || true
        if [ -n "$path" ] && [ -f "$path" ]; then
            cp -n "$path" "$dest/lib/" || true
            bundled=$((bundled + 1))
        fi
    done
    say "    lib/             $bundled libraries (the C++ runtime; the rest comes from the host)"

    printf '%s' '#!/bin/sh
# Runs the port from this folder, whatever the current directory is.
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/ps2EntryRunner" "$@"' > "$dest/run.sh"
    chmod +x "$dest/run.sh"
    : > "$dest/portable.txt"
    copy_game "$dest"
    cp "$REPO/docs/PORTABLE.txt" "$dest/README.txt" 2>/dev/null || true
}

# -------------------------------------------------------------- Windows
build_windows() {
    local dest="$OUT/windows" src="$REPO/build-win/ps2xRuntime"
    say "Windows -> $dest"
    reset_dest "$dest"

    cp "$src/ps2EntryRunner.exe" "$dest/"
    # The build already places the FFmpeg DLLs next to the exe.
    cp "$src"/*.dll "$dest/" 2>/dev/null || true
    copy_assets "$dest" "$src"

    printf '%s\r\n' '@echo off' \
        'rem Runs the port from this folder, whatever the current directory is.' \
        'cd /d "%~dp0"' \
        'ps2EntryRunner.exe %*' > "$dest/play.cmd"
    : > "$dest/portable.txt"
    copy_game "$dest"
    cp "$REPO/docs/PORTABLE.txt" "$dest/README.txt" 2>/dev/null || true
}

[ $DO_LINUX -eq 1 ] && build_linux
[ $DO_WINDOWS -eq 1 ] && build_windows
say ""
say "Done. Copy the folders inside $OUT to the stick."
