# GTA: Vice City Stories — PlayStation 2 recompilation

A source-only port of **Grand Theft Auto: Vice City Stories** (SLUS_215.90) to
PC, built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j.

**No game code or data is in this repository** — only our own code, the
recompiler configuration (addresses), and scripts. The game is generated on your
machine, from your own disc. That is deliberate: the point is that nothing of
Rockstar's is distributed here.

## Built with AI

This port was written with substantial help from AI. The code was produced by
coding assistants under human direction, and every change was built and run
before it was kept. The numbers in `docs/vcs-handoff.md` are measurements from
real runs, and the things that did not work are written down there with the
reason they were set aside.

This is here, near the top, on purpose: some people would rather not use
software made this way, and that is a fair position to hold. If it is yours, no
argument from me — and you have found out before spending an evening on a build
rather than after it.

For everyone else, the licence is unchanged: GPL-3.0, inherited from PS2Recomp.

## Building it

You need a PlayStation 2 copy of the game — the disc itself, or an `.iso` / `.bin`
you made from it — and a C++ toolchain.

```sh
./build.sh --image /dev/sr0          # read straight from a disc drive
./build.sh --image ~/vcs.iso         # or from an image
./build.sh --image /path/extracted   # or from a folder already extracted
```

It runs these stages, and each one is timed so you can see where the time goes:

| Stage | What it does | Time |
|---|---|---|
| tools | configures and builds the tools the stages call | ~2 min on a fresh clone, seconds after |
| 0 check | verifies the disc is the right game (boot-file hash) | instant |
| 1 extract | pulls the files off it | ~17 min from a disc, instant from a folder |
| 2 recompile | turns the game's ELF into C++ | ~1 s |
| 3 install | copies it into the runtime | seconds |
| 4 build | compiles the port (Linux, then Windows) | ~14 min |
| 5 vu1 | runs the game to learn its VU1 microprograms, twice | ~4 min |
| 6 assemble | makes a ready-to-run folder | seconds |

`--from N` / `--to N` resume a long build. The tools are built first whatever
range is asked for: a fresh clone has none of them, and every stage calls one.

The VU1 stage runs the game with `PS2X_HIDDEN_WINDOW=1`, so no window appears
while it learns.

## What is here

- `ps2xRuntime/` — the runtime the generated code runs on, including the GS
  renderer and the game-specific overrides.
- `ps2xRecomp/` — the recompiler.
- `ps2xBuilder/` — reading the disc (`ps2x_iso`: drives, info, list, hash,
  extract). Verified against real discs on Linux and Windows.
- `games/vcs/` — **the recompiler configuration and the per-game manifest**
  (`game.toml`), plus the analysis tools. See its README.
- `docs/vcs-handoff.md` — the working notes: what was tried, what was measured,
  and what is parked.

## Status

Playable and fast: full speed in gameplay, audio, cutscenes, the settings menu,
controllers (Xbox, DualSense, DualShock, Switch, Steam), texture packs,
rumble, and mouse look. See `docs/vcs-handoff.md` for the open list and the
things that are parked with reasons.

## Not affiliated

Not affiliated with, endorsed by, or connected to Rockstar Games or Take-Two
Interactive. "Grand Theft Auto" and the game names are their trademarks, used
here only to say which game this is for.

## Licence

GPL-3.0 — see `LICENSE`, inherited from PS2Recomp. The history here includes
upstream's commits; the port is the work on top of it.

## Special Thanks
Thank you to AL2009man for their gamepad asset pack. You can find their pack 
here: https://github.com/AL2009man/Gamepad-Asset-Pack
