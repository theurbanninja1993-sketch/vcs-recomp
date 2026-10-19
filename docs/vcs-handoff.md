# GTA Vice City Stories (SLUS_215.90) recomp: handoff notes

State as of 2026-10-18 (`vcs-bringup` is the tip the `release` branch players build
from was cut at, tagged `stories-v0.1.6`; the launcher is its own repo at `397239b` =
`v0.1.14`). Read this before changing anything. A reviewer will check work against it.

## 2026-10-18 evening (Claude): Windows players can build, without a command line

Owner: *"is there a way to make the windows launcher automatically run the command
when you try to build. Windows users are notoriously affraid of command lines"* and
*"also is WSL incluided in Windows or would that need to be installed? if it needs
to be installed can our launcher do that too?"* - the answer to both is yes, and it
is now what happens. Launcher `v0.1.12` through `v0.1.14` (`397239b`).

**B on Windows hands the build to WSL itself.** The Windows launcher starts the Linux
launcher under WSL on the build for that game - the same folder, so the same
`launcher.ini` and the same `port/`, and under WSL the launcher cross-compiles the
*Windows* port, which is the build the Windows launcher then plays. `--build <n>`
and `--image <path>` are how it asks (the game by its place in the same config), and
`make_release.sh` now puts the Linux launcher in the Windows folder, because that one
line is the whole of the Windows build story.

**WSL is not part of Windows**, and the launcher offers both halves of setting it up,
each as a script in a window that stays open - the last line of an install is usually
the one that says what to do next:

- no WSL -> `wsl --install`, elevated through `ShellExecute`'s "runas", which is what
  raises Windows' consent prompt;
- WSL without a compiler -> cmake and a C++ toolchain installed as root *inside* WSL,
  which needs no password and cannot reach anything outside WSL.

**Three reports in a row, and what each one taught:**

1. *"clicking B appears to crash the launcher"* - it did not. `wsl.exe -e uname`
   *starts* Linux, and three such questions were being asked on the thread that draws
   the menu, so the window stopped answering. The question is asked in the background
   now, with a **waiting** screen whose dots move, because a still screen is what a
   crash looks like.
2. *"it says to press b again but nothing happens"* - a design error: the answer was
   remembered, including the **no**, so pressing B after installing WSL re-read the
   old no and changed nothing. Only a yes is remembered now.
3. *"it still keeps saying WSL is not setup everytime I click B"* - the message was
   right (a restart is what an install needs) and the owner reached for it from that
   message alone. But a no should not need a person to work out what it means, so
   `v0.1.14` keeps the **evidence**: what was asked, the exit code, and the first line
   of the answer, shown in the notice. WSL prints UTF-16, so the report makes that
   readable rather than showing mojibake, and a mention of a restart, the virtual
   machine platform or no installed distributions is called out explicitly.

The lesson worth carrying: every one of these was a *silent* or *misleading* answer
from the launcher, and every fix was to make it say what it actually knew. The two
boolean probes that `v0.1.14` replaced were removed rather than left, because two
ways to ask one question is two ways to disagree.

## 2026-10-18 later still (Claude): B says why it cannot build on Windows

Owner, after `v0.1.10`: *"nothing happens when I press B on the windows version. the
Linux version works"* - and that was exactly right, and the worst possible answer.
The Windows launcher has no build to drive: `runTool` on Windows is a stub returning
-1 ("there is no Windows build to drive yet"), because the port is built by Linux and
the Windows port is cross-compiled from Linux. The B handler's guard was an empty
block with a comment. A key that does nothing reads as a broken key, not as a
deliberate limit - the same lesson as "pressing U does nothing".

Launcher `v0.1.11` (`171e2b3`). B now says which of the three things is missing - no
`port=` in launcher.ini, a source that has not been fetched, or a machine that cannot
build - in the same full-screen notice style as the other reports, and the last one
prints the route with the player's own folder in it:

    wsl --cd "C:\...\the-launcher" ./ps2x_launcher

**The Windows release now carries the Linux launcher**, which is what makes that one
line work: same folder, so the same `launcher.ini` and the same `port/`, and under
WSL the launcher cross-compiles the *Windows* port - so what the WSL build produces
is what the Windows launcher plays. Before this the Windows build story was "download
the other release too and work out which folder to be in". `make_release.sh` takes an
optional extra binary for the folder now.

The notice takes the `--screenshot` capture the other screens take, so it can be
looked at without a person at the screen. Verified that way: heading, wrapped
paragraphs and the command on its own line all render as intended.

**Note for whoever reads this next:** `v0.1.10` is the first launcher whose update
button fetches and installs, and `v0.1.11` is the first release it can be tested
against, because it needs something newer to exist. From here an update is a click.

## 2026-10-18 late (Claude): the launcher updates itself, and two fetch bugs

Owner: *"updating brings us to the release page which is an okay way to do it I
guess. is it possible to make it actually fetch the new version and install the
files to the folder where the launcher exe is?"* - it is, and it does. Launcher
`v0.1.10` (`d462890`), released for Linux and Windows in both archive formats.

**The shape of it.** A running program cannot replace its own files: on Windows
they are locked, and on either platform the code that would run afterwards is not
the code being replaced. So the work is split. The launcher downloads the release
asset for its platform into the system temp folder and unpacks it there (PowerShell
`Expand-Archive` on Windows, `tar` elsewhere - the zip exists so that the Windows
path needs nothing installed, which the `rm` bug below is the argument for). The
archive's single top folder is flattened, the player's own `launcher.ini` is copied
over the one that ships, and the staged executable has to actually be there. Only
then is a script written and handed the swap, and only then does the launcher exit:
the script waits for the process to end, renames the old executable to
`ps2x_launcher.old`, overlays the staged files, removes the staging folder and the
download, starts the launcher again and deletes itself. A failure at any point
leaves the install untouched and falls back to the release page.

**Two real bugs, both mine, both found by testing this:**

1. **`rm` in the Windows fetch chain.** *"'rm' is not recognized as an internal or
   external command"* was the whole of a failed fetch - and the fetch log added in
   `v0.1.9` is what said so, which is the point of it. `rm -f` had been left in the
   chain by the relative-path rewrite. Deleting a file is now a platform command
   like the others (`del /q` / `rm -f`).
2. **The fetch changed into a folder before creating it.** On a *first* fetch the
   port's parent folder does not exist, and a `cd` into a folder that is not there
   carries on regardless - so every relative name after it pointed elsewhere: the
   download landed beside the port instead of in it, and the unpack then failed on
   the folder it had just been told to create. The parent is now created before the
   shell moves into it. The owner's install already had `port/` from its git-clone
   days, which is why his failure was the `rm` and not this.

Verified here end to end on Linux, against a real install with a marker in
`launcher.ini`, a picture in `art/`, music in `music/` and an image in `discs/`:
the executable was replaced, the marker and every player file survived, the release
files arrived, `ps2x_launcher.old` was kept, and the staging folder, the download
and the script were all gone afterwards. A fetch into a folder that did not exist
was verified too. `--self-update <version>` is the same update without the window -
it is how this was tested, and it is how a stuck player can be walked through it.
The button and the flag build their command from one function so they cannot drift
apart.

**Note for the owner:** the version he has cannot update itself - the button that
does the work is in `v0.1.10` - so this one is a manual download. Every one after
it is a click.

## 2026-10-18 (Claude): the local achievement set is built

Owner: *"do you think you can build it?"* - it is built, and verified end to end.
Item 16 below has the design and the reasoning; this is what exists now.

**The engine** (`ps2xRuntime/src/lib/ps2_achievements.cpp`, ~450 lines, plus a
tab in the settings panel):

- **rcheevos** (MIT) is the evaluator, fetched by the runtime's CMake and pinned
  to a revision. Only its runtime half is built - the api and client halves are
  for talking to RetroAchievements, which a set that lives beside the port does
  not do. Nothing here opens a socket.
- The set is read from `$PS2X_ACHIEVEMENTS`, else `achievements.toml` beside the
  runner, else beside the settings. Conditions are RetroAchievements condition
  strings, so the set is portable if that rule ever changes.
- Evaluation happens **once per game frame**, in the present path's new-picture
  block, not per host frame - a host frame that shows the same game frame twice
  must not count twice, or a "hold this for a second" condition is met at half
  speed.
- Unlocks persist in two files beside the settings: the ids (ours, so the list is
  right even if the evaluator's state is lost) and rcheevos' progress blob (theirs,
  where hit counts live).
- A capstone over the others cannot be a condition on memory, so the port grants
  it: a trigger-less achievement in the `Capstone` category unlocks once every
  achievement that does have a condition is unlocked.
- `PS2X_ACHIEVEMENTS_DEBUG=1` prints every state change, the read and event
  counts, and the two self-test flags - which is how a condition that is not
  behaving is told apart from one whose address is wrong.

**Three things cost real time and are worth knowing before touching this:**

1. **The RAM mask.** rcheevos' read callback has to mask the guest address the way
   the hardware does, and the mask is `PS2_RAM_MASK` (32 MB - 1). Using 2 MB - 1
   by mistake aliases every address above 2 MB - which is every address this game
   keeps anything interesting at - and looks exactly like a set whose addresses are
   all wrong.
2. **Validation.** rcheevos will not evaluate a trigger whose addresses the client
   has not declared readable (`rc_runtime_validate_addresses`). Without it, every
   trigger sits there being read and never tested. Validation must come **after**
   restoring the progress blob, because a restored memref state can otherwise leave
   them unevaluable - and a stale blob then wedges the engine for good. The blob is
   therefore stamped (`PS2XACH1`) and ignored if the stamp does not match.
3. **A trigger that is already true when it is activated never fires.** rcheevos
   waits for a condition to be false once first, so "the game is running" has to be
   written as a transition (paused, then not) rather than as a state. Every
   condition in the real set is a counter going up, which is the shape that works.

**The set** (`games/vcs/achievements.toml`, 35 achievements): the shape and the
counts are in item 16, and the story milestones are now decided - the first and
last mission of each chapter, the way the official sets name theirs after the
mission:

| chapter | first | last |
|---|---|---|
| 1 | **Soldier** | **To Victor, the Spoils** |
| 2 | **Jive Drive** | **From Zero to Hero** |
| 3 | **Brawn of the Dead** | **Last Stand** |

Every trigger is still to be authored, and that is the playthrough's work: F9 for
a RAM dump, do the thing, diff, then confirm the address live in
**Settings > Achievements > Address watcher** - which is why that watcher exists.
The file's own comments say how, and one hint is worth repeating here: the six
story milestones are probably **one** progression counter, so they may become six
`>= n` conditions on a single address rather than six flags.

**What was verified, and what was not.** The engine: a self-test set
(`games/vcs/achievements-selftest.toml`) on addresses this port already reads for
its own overrides - the pause flag at `0x4CD2AC` and the cutscene flag at
`0x6F4D14`. Both a `!=0` transition (pause) and a `=0` transition (unpause)
unlocked, drew the banner, and persisted across a restart. The panel was captured
and checked: the list, the counts, the reset button and the address watcher all
render. **Not verified:** the banner's on-screen appearance - the guest-frame
screenshot mode deliberately excludes host UI, and the host capture needs a key
press - and the real set's conditions, which do not exist yet.

**Cheating stops unlocks (owner, 2026-10-18: "using cheats should prevent
achievements from unlocking in the official release").** Two ways in, and the
distinction matters:

- **The port's own `.pnach` patches**: cheating is an *enabled mod that actually
  writes to memory* (`mod.enabled && mod.patches > 0`), not the `pnach_cheats`
  switch, which is on by default and only means "apply patches from the mods I
  enabled". A texture mod is not cheating and must not block anything.
- **The game's own cheat codes** - button sequences, whose meaning the port cannot
  see - set a flag in the game's memory. The set names its address in
  `cheat_flag` in `[set]`, and the rule reads it as a byte. **That address is not
  found yet**, and finding it is a five-minute job with the memory search: enter a
  cheat that changes almost nothing else (a weather one), search "Changed", and
  take the address that latched. The file's comments say so where it belongs.

A blocked unlock is not silent: the panel says cheats are on and nothing will
unlock, and says if something was earned while cheating and not counted. The
condition is still evaluated, so the list keeps showing progress - which is how a
player tells a cheat block from a broken set.

**One thing left for the set to be complete:** the four hidden mission challenges
(ids 7-10) are held back on purpose, to be chosen against the missions once the
playthrough has been done. That makes the set **31 defined and 4 to come** - the
"35" in the design table below is the finished size, not what is in the file.

**A memory search, not just a watcher, is in the panel now** - find a value, then
narrow it by what it does (changed, increased, the same). That is the Cheat-Engine
loop, and it is what makes the other thirty conditions a matter of minutes each
rather than an afternoon of dump-diffing: the F9 dumps still work and are what the
wanted level was found with, but live is faster. Its logic is covered by a test
against a synthetic 32 MB guest, and the first real condition was found and
verified with it - *Catch Me if You Can* is `0xH52FC74>=6`, found by entering the
raise-wanted-level cheat twice and the lower one once with a dump after each:
exactly one address in 32 MB goes 0, 2, 4, 0.

## 2026-10-18 (Claude): a converted image that would not read, and what a hash mismatch means

The owner converted his CHD back to an image and the build rejected it. Working
through it with his own CHD turned up two layouts the reader did not know, and
one thing worth writing down about what the hash check can and cannot tell you.

**Every conversion of *his* CHD, done here, reads correctly.** `chdman extractcd`
writes a 2048-byte image and it hashes exactly like the CHD; so does the `.bin`
his own Windows tool produced, and the `iso/vcs.iso` in this project. All four
CHD copies on the machine are md5-identical. So the image that failed is not on
this machine, and could not be reproduced from the CHD.

**Two layouts were genuinely unreadable, and are now read** - both of them things
a person converting a CHD can easily end up with:

| what | stride | how it arises | before | now |
|---|---|---|---|---|
| `chdman extractdvd` output | 2448 | the wrong subcommand for a CD-based CHD | not an ISO9660 disc image | reads |
| raw DVD sectors | 2064 | a raw dump of a PlayStation 2 DVD | not an ISO9660 disc image | reads |
| 2048-byte ISO | 2048 | `chdman extractcd` | reads | reads |
| raw CD, data at 0 / 16 / 24 | 2352 | mode 1, mode 2 form 1, or a pass-through tool | reads | reads |

The reader now tries all of them and lets the volume descriptor decide, so a wrong
guess is never accepted and a right one is never missed. Verified: all six report
the same boot hash, and each is byte-identical on the boot ELF.

**What a hash mismatch means, since the message said too little.** If an image
parses as ISO9660 and the boot file still hashes differently, the bytes really do
differ: it is a different region, a different pressing, or an image a tool has
edited. That is a different problem from a layout the reader cannot handle, and
the two used to look alike - "this disc is not the one this source is for" with no
explanation. The launcher now says which, and `ps2x_iso check` already names the
disc it found ("Recognised: Grand Theft Auto: Liberty City Stories", or "not one
of the two games this is for"), which the build log carries. Tested by pointing
the VCS recipe at the LCS disc.

## 2026-10-18 (Claude): cut `stories-v0.1.3`, with launcher `v0.1.2`

Both are published. The port's `release` branch is a fast-forward of the dev tip,
tagged `stories-v0.1.3`, and the launcher is `d964ca8` = `v0.1.2` with Linux and
Windows folders attached to the release.

What a player gets over `stories-v0.1.2`: their disc in whichever shape they keep
it - `.iso`, `.bin`, `.cue` or `.chd` - plus everything in the launcher section
below (the fetch recovery and the build log).

Verified end to end, which is what makes this one safe to ship: the launcher's own
build engine (driven headlessly, `builder::run` against a fresh clone) read the
owner's **CHD** through every step - check, extract, recompile, install, compile,
two VU1 learning passes - and the runner it produced boots and renders the game.
The reads are correct rather than merely plausible: the boot ELF, `/SYSTEM.CNF`,
`/MAINLA.IMG` (282 MB) and `/BEACH.IMG` (230 MB) all come out byte-identical to the
same disc extracted from a `.bin`.

**One thing to look at, and it is not something this release changed.** A fresh
build's VU1 learning passes recorded **15** microprograms with an empty card and
**13** with the owner's save on it, where `games/vcs/game.toml` documents 22 for
that route ("the fast-boot script, the opening cutscene, then taking Sgt.
Martinez's bike"). Same route, same 7000 frames, so the count is not about the
card: the presses are frame-indexed, and if the game is still in its intro when
they land, the route never reaches the bike. Only the disc path moved in this
release, so a player's build has always behaved this way - but a first build
compiling fewer microprograms than the manifest promises is precisely the thing
that costs frame rate, so it is worth either re-tuning the route against a fresh
boot or letting the launcher bake in what the player's own play records (which is
what `build.sh --optimize` does, and how the 30-program builds were made).

## 2026-10-18 (Claude): CHD discs, and what the launcher does when a build breaks

The owner played the 0.1.1 release as a player would and came back with three
things. All three are fixed; the first two were the release's own fault, and the
third is the one that made the release useless to him.

**1. A source folder that a dead fetch or clone left behind could never be
fetched again.** `fetchCommand` ran `git clone` into `port_dir`, and `git clone`
refuses a directory that exists and is not empty, so a clone that died partway
(leaving files, no HEAD) put the launcher in a state where the card said "source
not fetched - press U", the Build key was refused because there was no revision,
and `U` could not fix it either. The only way out was to find and delete the
folder by hand - exactly what the owner did, and why "after deleting the VCS
folder it started again". Now the folder is cleared first when there is no
revision to keep, and an existing checkout that can no longer be updated falls
back to a fresh clone rather than reporting a failure nobody can act on. Wiping
is guarded (`safeToWipe`) because `port_dir` comes from a config file and
`rm -rf` should not be able to follow a typo into a home directory.

**2. A failed build dropped the player back on the menu and said nothing at
all.** Everything the build printed went to `stderr`, which is nowhere when the
launcher is started from a file manager, and there is no log. The wizard's own
RECIPE error was the only failure ever surfaced. Now the whole build is written
to `logs/<id>-build.log` beside the launcher (so it survives the source folder
being deleted), and when a build ends - finished, failed *or* stopped - the
launcher says which, puts the last line the build printed on the screen, and
names the log. A stopped build is distinguished from a failed one.

**3. The owner keeps his PS2 library as CHD, and the launcher could not see
it.** The picker only offered `.iso`/`.bin`, and what he did instead - renaming a
`.cue` sheet to `.iso` - was reported as "not an ISO9660 disc image", which is
true and no help at all.

`ps2x_iso` now sniffs the content rather than the extension:

- `MComprHD` → a CHD, read with **libchdr** (fetched in `ps2xBuilder`, host
  tools only: `ps2x_iso` is never cross-compiled, so a codec stack that never
  runs on the target is not worth a configure that can fail). It brings its own
  lzma, miniz and dr_flac, so it needs nothing installed - which is why it was
  chosen over writing a reader against system liblzma/libFLAC.
- text naming a `FILE`/`DATAFILE` → a `.cue` sheet, and the image beside it is
  opened instead. `DATAFILE` is what the Windows tools write, and is what the
  owner's own sheet used. A sheet whose image is missing says so by name.
- otherwise → the image, exactly as before.

**Where the sector data sits in a CHD unit is not fixed.** A CD CHD's unit is
2448 bytes (2352 + 96 of subcode) and a DVD's is a flat 2048, but chdman passes
2048-byte sectors *through*: the owner's VCS CHD has unitbytes 2448 with the ISO
data at offset 0, not behind a 2352-byte sync header. The layout probe therefore
tries offsets 0, 16 and 24 within the unit, guarded by the unit size. (`2048/0`
and `2352/0` were added to the plain-image list for the same reason.) Finding
this cost an hour: the first version only tried 16 and 24 and reported "not an
ISO9660 disc image" for a CHD whose descriptor was plainly at 0.

**Verified against the owner's own files**, since a wrong reader here is silent
corruption in a game build:

| what | result |
|---|---|
| VCS CHD boot ELF | `7f69b961cc427e3dcfd6582e25362be8` - matches the manifest |
| LCS CHD boot ELF | `3d2280cadf1a74ce39e121d1ea6941b2` - matches the manifest |
| `/SYSTEM.CNF` from the CHD vs the extracted folder | identical |
| `/MAINLA.IMG` (282 MB) and `/BEACH.IMG` (230 MB) | identical to the folder |
| the owner's `.cue` sheet, renamed `.iso` | reads, follows to its `.bin` |
| a sheet whose `.bin` is missing | says which file it wants, by name |
| a file that is not a disc image | says so instead of "not an ISO9660 image" |

Also fixed on the way past: the probe cleared its read error *before* deciding
the disc was unreadable, so every failure - a drive not ready, a permission
problem, a short file - came out as "not an ISO9660 disc image". The reason now
survives to the message.

## 2026-10-18 (Claude): cut `stories-v0.1.2`

The release branch is a fast-forward of the dev tip, tagged `stories-v0.1.2`.
What a player gets over `stories-v0.1.1` (`f4a8c39`):

- memory-card saves load at last - the listing is in creation-like order, not
  alphabetical, because the game finds its saves by position (`905abf2`);
- the card no longer has to live in the game folder (`921e48b`), and
  `GetEntSpace` reports real free space instead of a hardcoded 1024 (`402fc66`);
- the mouse is captured automatically at launch in fullscreen and borderless,
  and re-captured on leaving the settings menu (`1244c2d`..`ccef7b7`) - the fix
  is that the raw cursor mode is re-asserted, not raylib's `DisableCursor()`,
  which re-centres the pointer on every call and zeroed the look delta;
- modern free aim on keyboard/mouse and the Modern/Custom gamepad layouts
  (`49f137c`), an FPS overlay, display options in display terms with Auto scale
  (`4305b9f`), no music-folder station during in-engine cutscenes (`35e63af`);
- the GPU renderer's per-draw overhead and presentation work
  (`1443223`..`5b7bdfe`), and the VU program set learned during play in player
  builds (`97ae75b`, `a260eb5`) - the two together are what took the Windows
  port from 20-30 fps to a steady 60;
- the generated function registry is untracked again, so a fresh clone builds
  (`656b6f9`), which is what the launcher hands a player.

Not in it: the game's own invert-look default, which the owner parked
deliberately - the two RAM dumps taken to find its settings byte were not
paused, so the byte never surfaced - and frame interpolation, parked earlier.

The launcher is released with it as `v0.1.1` (`d632394`), carrying the
dual-GPU Optimus/PowerXpress exports and nothing else new. That release is a
courtesy, not the mechanism: the launcher already follows the port's `release`
branch, so a player's update picks up `stories-v0.1.2` without a new download.

## 2026-10-18 (Claude): memory card saves now load

**The load never happened - not because of the save, but because the port's
`GetDir` sorted the save folder's listing alphabetically.** On a real card the
listing is in directory (creation) order: the game writes its fixed files first
(`icon.sys`, `vcsicon.ico`, `BASLUS-21590GTA40001`) and its numbered save files
last (`1 Soldier`). VCS finds its saves *by position*: it reads entries 5..14
and matches the leading digit against the save slot ("%d"). Alphabetising
pulled `1 Soldier` forward to position 2, so the game read `icon.sys`,
concluded the card was valid, and silently started a new game - which looked
exactly like "the save is not loading" because a fresh game and a save at
mission 1 start in the same barracks. Fixed in `905abf2`: the listing keeps the
two special entries first, then named files, then numbered saves (the split
that reproduces the real creation order, deterministically, on any host
filesystem). Verified from the card logs: the game now reads the full save file
(98304 bytes, byte-exact). Owner-confirmed working on a real play session.

**How this was found** (in case the save path breaks again): the port's card
emulation was verified correct end to end - `GetInfo`/`Chdir`/`GetDir`/open/
read all answer what the game asks, byte-perfect. The game's own recompiled
card library (EE code at `0x274d40`-`0x276560`, files `runner/FUN_0027*.cpp`)
validates the save: finds five files by name, memcmp's `icon.sys` against
`"PS2D"`, then runs a date-record validator (24-byte records at
`0x510940 + slot*0x4C0`, u32 Sec/Min/Hour/Day/Month/Year, needs a non-zero
month). Those records stay zero unless the game's entry-table fill succeeds,
and that fill only keeps entries whose leading digit matches the slot number -
hence the positional dependency. The game's decision chain that matters:
`FUN_277688` (card decision) -> `FUN_276AB0` (validate the save, returns 1) ->
`FUN_2762A8` (date-record check) -> `FUN_277318` (entry-table scan, gates at
`+0x120` name byte / `+0x114` attr &0x80 / `+0x110` size <= 0x17FFF).

**Also on the card (`921e48b`, `402fc66`).** The card root no longer defaults
to inside the game folder (updating game data would take saves with it);
portable builds put it beside `portable.txt`, and a card already beside the ELF
still wins. `GetEntSpace` now reports the real free space (8192 clusters)
instead of a hardcoded 1024.

**Not investigated further:** in headless test runs the post-load world stream
sat on the loading screen for minutes (the game idles at ~4% CPU, so it is a
wait, not work). The owner's real sessions load fine, so this was left alone;
if loads ever feel slow in play, the CD stream timing model
(`cdStreamWakeTickForSectors` in `CD.cpp`) is the suspect.

## 2026-10-18 evening (Claude): settings.ini changes the launcher must accept

- `internal_scale=0` now means **Auto** (pick 1x-4x from the window height;
  1080p -> 3x). The launcher must not clamp it to 1-4.
- `free_aim=0|1` (default 1) - Controls > Aiming > Free aim.
- Cutscene dialogue: IOP clocked by game time during in-engine cutscenes
  (`8d9658a`), IOP backlog dropped only after 20 VBlanks.

## 2026-10-18 later (Claude): laptop cutscenes - profile, don't guess

Perf-log thread % is of *emulated* time (>100% = can't keep up). On the owner's
RTX 3050 laptop the game thread read 200-280% in dense cutscenes vs ~35% on the
desktop. A Windows sampling profiler now exists: `PS2X_HOST_PROFILE=<file>`
(stick: `play-profile.cmd`); symbolize with `llvm-nm -C` of the same
`build-win` exe (nearest symbol; llvm-symbolizer fails on no-debug-info code).
It cannot run under Wine (thread suspension breaks X). Fixed from its data:
frame copies on the game thread (`b13553a`), the readback fence wait in the
NVIDIA driver (`c17a5e6`: never wait, hand over the newest finished copy) and
stream-ring stalls (`049ef80`: 64 MiB, 8 segments). Owner after `c17a5e6`:
cutscenes ~60, desync mostly gone, some stutter near the end of the dense one.
The profiler merges threads that share a name (the IOP worker is also called
"GameThread").

## 2026-10-18 (Claude): GPU renderer cost on weak GPUs, and free aim

**Renderer (`1443223`, `675f2fb`).** The owner's Windows box is an RTX 3050
*Laptop*. Three costs fixed, each with an env switch back to the old path:
- Presentation readback was a synchronous `glGetTextureSubImage` every frame
  (CPU waited for the whole frame on the GPU). Now a fenced pack-buffer ring,
  one present of latency; repeat VBlanks collect it via
  `GSRasterBackend::PollPresentation`. `PS2X_GS_ASYNC_READBACK=0` = old.
- Stale render-target rows were all reloaded on any draw/texture read/present;
  the display target is grown to 1024 rows (pow2 texture reads) over the memory
  the game streams textures into, so ~100 needless reloads a frame at the
  upscaled size. Loads are now limited to the rows drawn/sampled/shown (dirty
  and stale kept disjoint). `PS2X_GS_FULL_STALE_LOAD=1` = old. 4x GPU time per
  game frame on a 9070: 9.5 -> 5.3 ms.
- Per-draw driver CPU on NVIDIA (cutscenes late at *any* scale, driver thread
  60-83%): vertices and upload data now stream through persistently mapped
  rings (`PS2X_GS_STREAM_RING=0` = old `glBufferSubData` path), replacement
  textures use sampler objects, and the overlay-mask draws no longer issue a
  `glMemoryBarrier` each. Result on the laptop: gameplay locked 60 at 4x (owner
  confirmed after the first two). Cutscene result for the third: awaiting the
  owner's `play-cutscene-new.cmd` / `play-cutscene-old.cmd` A/B on the stick.
- Measuring: `PS2X_GS_GPU_TIMERS=1` prints GPU ms per present by kind
  (GL_TIME_ELAPSED) plus ring stalls; `PS2X_GS_LOAD_TRACE=<present>` logs one
  frame's loads/resolves/stale marks. A fast desktop GPU does not reproduce
  laptop behaviour - ask for the owner's logs.

**Free aim (`49f137c`).** On foot with a gun the aim button (RMB; LT on the
Modern/Custom pad layouts) no longer sends R1: crosshair (host overlay), camera
over the shoulder and levelled, player faces the camera when standing, and the
player's instant-hit shots are re-aimed at the crosshair ray's hit (rotation
from the ped's facing, so shotgun spread is kept). Setting `free_aim` (Controls
> Aiming, default on). Key addresses and hooks are in the commit message and
next to `vcsFreeAimShot` in `game_overrides.cpp`. Not done: projectiles
(rockets/grenades) still fly along the ped's heading; no strafe animation while
moving (the game only strafes in lock-on); LCS parity.

## Read first: the compiled VU program set (2026-10-17, Claude)

**Slowdown since 10-16 found and fixed in the dev build.** The owner saw dips to
~20 fps, including the cutscenes, with the cutscene audio drifting. The cause is
the set of compiled VU programs, not textures, scale, draw distance or prompts:
an A/B of each of those changed nothing.
- `build` and `build-win` were configured with
  `PS2X_VU1_PROGRAMS=/tmp/opencode/workspace/build/vu1`, which holds **15
  programs**: the pipeline's intro-only learn, with no VU0 programs at all. The
  full set from the three gameplay captures plus the VU0 capture is **30**
  (`vcs-vu1`). Everything missing ran interpreted: VU1 worker 72-88% (was 14-21%
  in the 10-14 build), VU0 ~2 s per 10 s on the game thread, and the opening
  cutscene 155-181/600 VBlanks late with 60-76k audio underruns.
- **Fix:** regenerated all 30 with today's `vu1_recomp` (replay: mismatches=0 on
  all four captures, 4.7-6.3 us per VU1 call). Installed into `vcs-vu1` (old set
  kept in `scratch/vcs-vu1.backup-20261017`). `build` is reconfigured to it.
  Same test after: cutscene 0-1/600 late, no underruns, VU1 worker 12-20%. The
  owner reports a locked 60.
- **Windows: same cause, now fixed.** `build-win` is reconfigured to `vcs-vu1`
  and rebuilt. The owner's last Windows perf log (RX 9070 XT, real Windows) had
  17-49k interpreted VU1 calls per 10 s and VU0 ~2.5-3 s per 10 s, at 21-33 fps.
  OpenCode's "126 us per VU1 call, VU1-bound and serial" in the Windows section
  below is the interpreter, not compiled VU1. Under Proton with the owner's
  settings (HD pack, 4x): old exe 45-49 fps in the opening cutscene, 98-117/600
  late, 112k underruns; new exe 60 fps, 0-1/600 late, no underruns, VU1 worker
  16-25%. The new exe is on the stick (`windows/ps2EntryRunner.exe`, old kept as
  `ps2EntryRunner.before-vu-fix.exe.bak`, CHECKSUMS.txt regenerated). Pending:
  the owner's test on real Windows.
- **Player builds (`build.sh`):** the learn run now records VU0 programs too
  (`PS2X_VU0_CAPTURE`), and `games/vcs/game.toml` rides Sgt. Martinez's bike after
  the intro (7000 frames). Measured with this route: 22 programs (8 VU0) instead
  of 15. That is still short of 30, because a scripted route cannot reach
  everything. So the runtime keeps learning during normal play (below).
- **Learning during play (done, needs a launcher button).** By default the
  runtime records every VU1/VU0 program it meets that has no compiled version
  (4 runs each, the same capture format) into `<config>/vu_learn/`, where
  `<config>` is `<game>/<platform>/config` for portable installs, and writes a
  `PENDING` marker there. A complete build writes nothing. `PS2X_VU_LEARN=0`
  turns it off. **Launcher contract:** if `<config>/vu_learn/PENDING` exists,
  offer "Optimize", which runs `build.sh --optimize --out <game folder> --work
  <the build's work folder>`. That compiles every played recording so far into
  `vu1_programs.so` / `.dll` next to the game (the plugin the runtime already
  loads at startup; no relink, about a minute), keeps the recordings in
  `<work>/vu_played` (a later full rebuild links them in), and clears the
  marker. The plugin only holds programs the linked build lacks: plugin calls go
  through the API table and measured ~8-10 us/call against ~6 linked (with
  another game running alongside), against ~57 interpreted. Tested end to end
  in a sandbox: recordings moved, 22 programs built, plugin loaded ("[vu1] 22
  compiled programs from .../vu1_programs.so"), nothing interpreted.

## If you are reviewing this

Three things are waiting for a second pair of eyes, and they are the three worth
your time:

- **Frame interpolation is parked again, and what is left is pacing, not the
  picture.** The warp is gone (the two-way blend and the static-pixel rule got it:
  `cc5abea`, `7e5bf2d`), but on the owner's machine at 120-240 the frame times are
  still not steady and the result tears, so the option is out of the menu and saved
  rates above 60 are ignored (`d2df272`). The code is intact behind
  `PS2X_FRAME_RATE=<hz>`. Search for `FOR THE NEXT PAIR OF EYES`; the six earlier bugs
  that each produced the *same warp* are written down next to it and are still the
  right reading of how it was fixed.
- **Modern motion blur**, implemented and calibrated against captures, parked behind
  `PS2X_MODERN_BLUR=1` rather than offered in the menu.
- **Windows**, which builds and runs but has had a fraction of Linux's attention.
  The leads are under "Windows has had the least attention" below.

Two things about this project are not like other recompilation projects, so they are
worth knowing before judging anything: **no game code and no game data ship, ever** -
the player's own disc produces the game - and the project is built by one owner with
heavy AI assistance, which the READMEs state plainly.

## Ground rules

- **Never `git push`** without the owner's explicit OK. Commit locally on
  `main` in `PS2Recomp/`, one logical change per commit, with a message that
  says what changed, why, and how it was verified.
- **Never stage `ps2xRuntime/src/runner/register_functions.cpp`, and never
  `git checkout` it back either.** The tracked file is upstream's 6-line stub and the
  build generates the real 240k-line registry into the same path, so `git status`
  reports it as modified permanently. That is the normal state, not something to
  clean up. Staging it commits 19 MB of generated code; reverting it leaves the
  *next* game build with an empty function table, and nothing complains until the
  game runs. On 2026-10-17 a clean-up pass did exactly that, and the game was
  rebuilt from `build-pipeline/generated/register_functions.cpp`, an identical copy.
- Never commit game code or data: generated `FUN_*.cpp`, `vcs-vu1/`, VU1/GS
  captures, ISO contents, texture packs.
- Test runs must use a **visible window** (no `PS2X_HIDDEN_WINDOW`). The owner
  watches them. Run one game instance at a time, and don't leave runs behind.
- Don't touch the owner's settings: use `XDG_CONFIG_HOME=<tmp dir>` for test
  runs. `PS2X_EXIT_AFTER_FRAME=N` skips the launch menu and quits at frame N.
- Don't ask the owner what to do next. Pick the most important open item.

### A Windows link needs the VU1 programs on the link line

Not for the microprograms themselves: for the link. raylib exports `CloseWindow` and
`ShowCursor`, and Windows' USER32 exports both as well, so the symbol exists twice -
once in `libraylib.a` and once in the import library. Which one wins depends on the
link line. With `ps2_vu1_programs` on it, CMake records the dependency and repeats
`libps2_runtime.a` later in the line, and the resolver takes raylib's. Without it,
the resolver takes USER32's and stops:

    ld.lld: error: duplicate symbol: CloseWindow
    >>> defined at libraylib.a(rcore.c.obj)
    >>> defined at USER32.dll

So `PS2X_VU1_PROGRAMS` has to be set when build-win is configured, and the Windows
build has to come *after* the VU1 stage. `build.sh` had it before, which meant a
fresh tree could not produce a .exe at all; the handoff's own cross-compile command
does pass the VU1 directory, which is why that one was fine.

Found in a clone fetched from GitHub. Diffing the two `build-win/CMakeCache.txt`
files came down to this single setting - everything else, including the toolchain
binaries themselves, was byte-identical.

### Windows has had the least attention

It builds, and it runs - confirmed on real hardware and under Proton. It has not
been optimised the way Linux has.

**What the GPU renderer needed (2026-10-17, the owner's RTX 3050 laptop).** It was
falling all the way to the CPU rasterizer. Creating a 4.6 core context on the
host's DC failed and that was the only attempt made; the fix is a retry unshared on
a private window, plus posting `NvOptimusEnablement` /
`AmdPowerXpressRequestHighPerformance` so a two-GPU laptop picks the discrete GPU
at all. The renderer now runs 4.6 there.

**The window's context cannot be shared on Windows.** The renderer would rather
hand the composed picture over as a texture and skip the readback entirely. The
driver refuses: `wglCreateContextAttribsARB` with a share context returns null
(Windows error 3221688541 = `0xC00710DD`) while the same DC accepts a 4.6 core
context unshared. It is not a version gap - rebuilding raylib's own context at 4.3
instead of 3.3 changed nothing - so **the readback is the present path on Windows
and no amount of context work will remove it.** Worth knowing before anyone tries
again.

**The frame rate that is left is total CPU throughput, not a setting.** Measured on
the laptop at native: 40 fps with *no* thread saturated - VU1 91% of a core, the
game thread 56%, the NVIDIA driver 69%, and an in-process thread the port does not
own at 58%. On this machine the same build runs at 60 with the driver threads at
~6%. So that laptop spends a core and a bit outside the game, and has about three
cores to spend. Resolution changes nothing because none of it is fill: the readback
is capped to the window, and VU1 does not care.

**Measured again with the host timing on (2026-10-17, `play-perfstats.cmd`).** Three
things came out of one run, and the first two retire work that was next in line:

- **The unnamed thread was the NVIDIA driver.** With the module named for every
  thread, `0xdd100001` is `nvoglv64.dll:0xdd100001` (7% of a core) beside
  `nvoglv64.dll:2912` (35%). It was never an overlay or a recorder, and no vendor
  setting is going to remove it. `0xdd100001` is a description several of the
  driver's threads share, which is why it looked like one thread on two ids.
- **The host loop is a rock-steady 60 fps.** `[hosttiming] frameMs avg=16.67
  p50=16.67 p99=16.7 max=16.9` through a busy stretch. The host draws the latched
  frame, so a perfect host does not prove the renderer is comfortable - but the
  renderer's thread was 13% of wall and the driver ~25%, so **the present path is
  not the critical path.** That deprioritises both the asynchronous readback and the
  window-present path below: neither is what the frame rate is waiting for.
- **What it is waiting for is VU1, on one thread.** `Vu1Worker=144%` of its budget
  with `vu1Ms=14043` in a 16.9 s interval: **83% of a full core on a single thread**,
  while the frame itself is 28 ms. Per call, 126 us here against 47.8 us on this
  machine - and the busy samples read 18.0, 22.6, 19.6, 26.2 fps, i.e. ~185 VU1
  calls a frame at 126 us, which is the whole frame. The game is VU1-bound and VU1
  is serial.

**PGO does not exist on Windows, and cannot.** The CMake gate is
`CMAKE_CXX_COMPILER_ID STREQUAL "GNU"`, so `PS2X_PGO=use` applies to the Linux build
(GCC) and never to the Windows one (clang/llvm-mingw) - confirmed in the caches:
`build/CMakeCache.txt: PS2X_PGO=use`, `build-win/CMakeCache.txt: PS2X_PGO=off`, and
`-fprofile-use=...` on the Linux runtime target only. So every Windows player runs an
unprofiled build while Linux does not. clang can do PGO; the gate is simply written
for GCC, and collecting a profile needs an instrumented run on the target.

Two smaller finds from the same run, neither urgent:

- **The texture-replacement worker is enormously expensive.** A callgrind profile
  puts 34% of its instructions in `stbi_zlib_compress` alone (it writes PNG caches).
  It runs even with the HD pack disabled, and its own decode of what it wrote is
  already failing on Windows (`cannot decode PNG`).
- **`PS2X_GS_GPU_STATS` prints nothing on Windows.** Its totals come from the
  renderer's destructor, which is evidently not reached on the normal exit path, so
  "time: readback= draw= present=" never appears. The numbers would be useful for
  settling the present cost; the host timing answered it instead.

### The VU1 harness exists - use it, and watch the build basis

`ps2xRuntime/tools/vu1_replay` replays a `PS2X_VU1_CAPTURE` of MSCALs offline and
checks the final VU1 state, VU1 data memory and the XGKICK output **bit for bit**
(`mismatches=0`), and it carries its own RIP profiler (`VU1_PROFILE=<file>`, host RIP
samples relative to `profileHandler`). Record a capture with
`PS2X_VU1_CAPTURE=<file>,<startFrame>,<count>`; 900 MSCALs is a few game frames' worth
and replays in under a second. The tool now prints cycles, calls and **ns per emulated
VU cycle** beside us/record, because per-call figures are not comparable across scenes
(a call is 1600-2400 cycles and varies by more than an order of magnitude).

**Its figures are only comparable within one build basis, and there is a trap.**
`build/` is configured `PS2X_PGO=use` with a profile collected 2026-10-07, and GCC
consumes a *stale* profile silently - no warning. Recompiling the VU core against it
(which any change after that date does) made the very same capture replay **five times
slower in the same tool**. So before trusting any before/after: regenerate the profile
(`PS2X_PGO=generate`, run `vu1_replay` plus a gameplay session, then `PS2X_PGO=use` -
the workflow the CMake comment describes), or build `PS2X_PGO=off` and treat the
figure as conservative. Regenerating is the right answer for anything shipped.

Solid regardless, because these are from single builds:

- **The cost is in the generated per-instruction code**, not in the interpreter choice:
  compiled VU1 is only 1.35x faster than interpreted, so whatever dominates is shared
  by both paths rather than being instruction dispatch.
- `VU1Interpreter::calculateFmacExactResult` (an 80-bit `long double` path) is ~3% of
  replay samples - small, but it is inside the hot loop and worth a look.
- **Two dead A/Bs, both measured:** `PS2X_SYNC_GS=1` is worse (46.9 us a call against
  37.8, because the GS work then happens inside `run()`), and the CPU rasterizer is
  worse again (63.0 us a call, 46.6 fps against 60) - so the GPU renderer is not
  costing VU1 anything, it is paying for itself.
- **Contention is not the explanation.** Eight spinning threads and seven concurrent
  replays moved the replay's per-record time by under 3% (7.5 to 7.7 us). Note this was
  measured before the build-basis problem above was found, so re-take it on a clean
  baseline before relying on it.
- **`vu1Ms` is wall time inside `run()`, not pure emulation time** - it includes
  handshake waiting, and with a synchronous GS it includes the GS work. The replay
  figure is the honest measure of the emulation itself.

### What the VU1 cost is not (measured by ablation)

Seven variants, each with one piece switched off, min of six interleaved runs, one build
basis (PGO off, so these are conservative), against a gameplay capture whose baseline is
38.3 us a call and 2359 emulated cycles a call:

| switched off | effect | usable? |
|---|---|---|
| the per-instruction pipeline commit | 1.6% faster | yes |
| the register marks (VF/VI/ACC ready) | 0.7% | yes, and mismatches=0 |
| the per-instruction budget check | 1.1% | yes, mismatches=0 |
| the pipeline stall on a read | 1.9% | no, the run diverged |
| the FMAC 80-bit exact path | 1.4% | yes, mismatches=0 |
| the flag commit (`syncFlags`) | 15% faster | **no** - it ran 45% fewer cycles |
| the branch-backup clear | 8.5% slower | no, the run diverged |

Every identifiable per-instruction component is worth about 1%; all of them together are
roughly 6%. **There is no hot line to remove.** Note the first row: ablating the *entire*
pipeline commit moves 1.6% while the RIP profiler attributed 39% of VU1 to it. That is
the second time the sampler's line attribution pointed somewhere wrong - the first cost
a day chasing the present path - so **trust the ablations over the profile.** Two of the
clean results are clean because of the file's own design (the tool hands the VU a huge
budget; normal values take the plain float route), which also says the 80-bit
recomputation is dead weight on normal data - worth 1.4% at most, not the prize.

**The shape is what matters.** Two generated programs are 99.5% of VU1 time on gameplay
(62.7% and 36.8%), at 15.0 and 19.2 ns per emulated cycle; the other ten are rounding.
The cost is spread evenly over the instructions of those two rather than concentrated -
~17 ns per emulated VU cycle, about 50-60 host cycles per emulated instruction, and about
42% of one core here at 60 fps. On the owner's laptop, roughly twice as slow per core,
that is the 83%-of-a-core that makes the frame VU1-shaped. **A 1.5x VU1 speedup would put
that laptop at 60** - and since no single component is worth more than 2%, it needs a
systemic change (VU1 across threads, or a generator that keeps VU state in registers
instead of in memory), not another pass of micro-optimisation.

**And there is an unexplained factor of five.** The tool binary that was sitting in
`build/` before any of this replayed each of these captures at **7.5-8.2 us a call**;
every build made from the current source since replays the *same files* at **42-49 us**
(`mismatches=0` either way, and 329 `vu1p_` symbols present, so both really run the
compiled programs - 42.1 us with them against 75.1 us with `PS2X_VU1_NO_COMPILED`).
Removing my own instrumentation changed it by 2%, and switching PGO off by another
little, so neither explains it. The likeliest reading is that the binary was older than
the tree it sat in - which would mean the VU1 path got several times slower over the
days of changes since, and that is worth much more than any micro-optimisation.

**Next step, running: a bisect.** `git worktree add --detach <dir> 65ebb08` plus a
`vu1_replay` build, configured with the same VU1 programs directory, then the same
captures. If the old commit is fast, bisect the range; if it is also ~42 us, the fast
baseline was a build artefact and the absolute figures here are not trustworthy at all
until a clean, regenerated profile exists.

One admission for whoever reads this next: the fast baseline binary was overwritten by
these rebuilds before its timestamp or build flags were recorded, so the fast number
cannot be re-taken. Capture `stat` output before rebuilding anything that is being used
as a baseline.

Two leads this file used to recommend are **dead**, measured on this machine by
running the same scripted scene twice:

- **VU1 program coverage buys almost nothing.** The learnt set covers only the boot
  sequence and the lead looked strong, but interpreted VU1 costs 62.9 us a call
  against 46.7 compiled - 1.35x (`PS2X_VU1_NO_COMPILED=1` for the all-interpreted
  run). The VU stack is simply expensive: ~72% of a core at 60 fps here, on one
  thread (`Vu1Worker`, the async VIF1 worker - `ps2_memory.cpp`), and it is that
  thread the laptop runs out of. Learning more programs is not the fix.
- **Running VIF1/VU1 inline is worse.** `PS2X_SYNC_VIF1=1` puts the work on the game
  thread: 51 fps against 60, game thread pinned at 104%.

Still open, in order of what they are worth:

- **VU1 throughput, and it is the only thing that moves the laptop's ceiling.** It is
  the critical path - 83% of a core, on one thread, 126 us a call - and the frame is
  VU1-shaped. Two ways in: find what the per-call cost is actually spent on, or spread
  it over threads. The compiled/interpreted ratio being only 1.35x says the cost is
  *shared* between the two paths rather than in the instruction dispatch, which is
  worth knowing before optimising either. Cycles per call (`m_cycle` delta per run,
  reported in the perf log) would say whether the emulation spends a sane number of
  host cycles on a VU cycle or a silly number, and that is cheap to add.
- **PGO (and LTO) for the Windows build.** The gate is GCC-only, so this is a small
  CMake change plus a way to collect a clang profile on the target. It is a tenth-ish
  on every Windows machine rather than a fix for this one, and the pipeline already
  runs the game twice for the VU1 passes, which is where a profile could be collected.
- **The Windows present path** - the asynchronous readback, and the window-present
  design written up above. Both are real; both are now *below* the two items above,
  because the host is already a steady 60 and the renderer's thread is 13% of wall.
  This buys headroom, not frame rate.
- **The texture-replacement worker**, which is expensive and writes PNGs it then
  cannot decode.

### The window-present path, and why it is not just a blit

The share refusal looks like it ought to have an easy answer. A window's back buffer
belongs to its DC, not to a context, and on Windows the renderer *already* creates
its context on the host's DC - `publishHostGlContext` publishes `GetDC(hwnd)` for
exactly that reason, and the private-window fallback is only for when the DC itself
refuses. So a renderer context current on that DC can draw into the window's back
buffer with no share at all: blit the composed frame to framebuffer 0, let the host
draw its overlay on top, swap once. The diagnostic already proved the half that
looked doubtful - "4.6 core works on this DC when unshared".

Working the sequencing through, two things make it more than a blit, and both come
from the same place: **the swap rotates the back buffer, and only the renderer can
redraw the game frame into the new one.**

1. **Every host frame needs its own blit.** The renderer composes at the game's rate
   (60); the host presents at up to 240. On the readback path the host redraws the
   same latched texture into each new back buffer, which is fine because a texture
   can be drawn any number of times. A blit cannot: after a swap the new back buffer
   holds what was there two frames ago, so a frame blitted once is shown once and
   then replaced by stale content. The renderer therefore has to blit once per *host*
   frame, which means the host drives it.
2. **Which means a handshake, not a flag.** The host has to ask and *wait* for the
   blit before drawing its own overlay, or a blit landing between the overlay draw
   and the swap wipes the overlay. A "do not blit while I draw" flag does not close
   it: a blit already issued still completes.

So the shape is: at the point where the host draws the frame today, publish a request
and wait - bounded, with the frame from the readback path as the fallback if it is not
answered; the renderer's thread blits the newest composed frame to framebuffer 0 and
acknowledges; the host draws its overlay with no background clear and swaps. Worst
case the host ends up running at the game's rate, which on Windows is 60 anyway
(interpolation needs the share and cannot run there), so there is nothing to lose at
the top.

That is a change to the threading contract of the present path rather than a change
of destination, which is why it wants hardware and a second pair of eyes in front of
it - and why the asynchronous readback is worth doing first: nothing can block it, and
it removes the stall, which is half the measured cost.

## Layout (project root `/mnt/2TBSSD/projects/Vice City Stories Recomp`)

| Path | What |
|---|---|
| `PS2Recomp/` | git repo (fork of ran-j/PS2Recomp, GPLv3): recompiler + runtime |
| `PS2Recomp/ps2xRuntime/src/lib/game_overrides.cpp` | VCS-specific fixes and features (bottom of the file) |
| `iso/extracted/` | the disc (boot ELF `SLUS_215.90`) |
| `vcs-configs/vcs_clean.toml`, `vcs_clean.csv` | recompiler config + function map (copy in git: `PS2Recomp/games/vcs/config/`, keep in sync) |
| `tools/` | map generator (`rebuild_function_map.py`), disassemblers, profilers (copy in git: `PS2Recomp/games/vcs/tools/`) |
| `vcs-vu1/` | AOT-compiled VU1 microprograms (generated, linked when configured) |
| `scratch/` | captures, logs, screenshots, `ext/` (llvm-mingw, PCSX2 source) |
| `GTA Vice City Stories Texture Pack/` | owner's HD pack, symlinked as `build/ps2xRuntime/mods/HD Textures for VCS` |

## Build and run

Linux (main dev build; PGO profile in `build/pgo`):

    cd PS2Recomp/build && cmake --build . --target ps2EntryRunner -j 14
    cd scratch/shots && env PS2X_GS_GPU=1 ../../PS2Recomp/build/ps2xRuntime/ps2EntryRunner ../../iso/extracted/SLUS_215.90

Windows (cross-compiled, tested under Wine 11 / Proton):

    cmake -B build-win -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake \
      -DLLVM_MINGW_ROOT=<root>/scratch/ext/llvm-mingw-dl/llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64 \
      -DPS2X_VU1_PROGRAMS=<root>/vcs-vu1
    make -C build-win -j14 ps2EntryRunner
    wine build-win/ps2xRuntime/ps2EntryRunner.exe   # boots ./game/SLUS_215.90 next to the exe

   - **Wine vs Proton (2026-10-13):** the system wine (11.19) has jittery frame
     pacing (`PS2X_PERF_LOG` over the fast-boot run: late=2..57/600,
     lostMs up to 554 ms) and the radio music audibly ran slow, while the
     emulator itself was never the bottleneck (eeMcyc constant, VU1 fully
     compiled, 0 interpreted calls). Running the same exe with the bundled
     wine from `proton-cachyos-native` (any Proton install works) fixed it:
     late=0..22/600, steady-state lostMs 0-29 ms --- the win is **ntsync**
     (the prefix boot printed "ntsync: up and running"; needs the ntsync
     kernel driver, checked with CachyOS's ntsync-enabled kernels).
       WINEPREFIX=/some/proton-pfx \
       /usr/share/steam/compatibilitytools.d/proton-cachyos-native/files/lib/wine/x86_64-unix/wine \
         ps2EntryRunner.exe
     The runner is OpenGL (GL 4.6 core passthrough to native Mesa), so DXVK
     does nothing here; Proton helps only through sync primitives.
     **Owner-confirmed (2026-10-13):** the radio music is at full speed
     under Proton, and all of yesterday's radio work (custom station,
     D-pad hold Radio Off toggle, unmute on station change) works on the
     Windows build.

Regenerating game code (only needed after recompiler/map changes, about 25 min
full build): see the `vcs-recomp-function-map` notes. `ps2recomp
vcs-configs/vcs_clean.toml` writes `vcs-port/generated_clean/`, which is copied
into `ps2xRuntime/src/runner/` + `include/`.

## Testing recipes

- **Fast boot to gameplay** (skips intro/cutscene, gameplay at ~frame 2400, the
  bike is next to the player; Triangle at ~2800 to mount):
  `PS2X_PAD_SCRIPT="1000:cross:10;1200:cross:10;1400:cross:10;1600:cross:10;1800:cross:10;2000:cross:10;2200:cross:10"`.
  Don't use it when testing the intro, cutscenes or logos.
- **Intro cutscene ~frames 6000-9000** (no script) is a good streaming-heavy
  scene for renderer perf.
- **Screenshots:** `PS2X_SCREENSHOT_EVERY=N` (PNG in cwd). Check them, don't
  assume.
- **Per-thread load:** `PS2X_PERF_LOG=<file>` (every 600 frames: late frames,
  audio, % per thread, VU1 compiled/interpreted counts).
- **Profiling:** no `perf` on this box. Run with `PS2X_ALLOW_PTRACE=1`, then
  `eu-stack -p <tid>` repeatedly (it dumps all threads; split on `TID n:`).
  The `ScopeTimer` numbers in the GPU renderer proved unreliable. Trust
  sampled stacks and A/B runs instead.
- **Pitfall:** `pgrep -f` / `pkill -f` match their own shell command. Use
  `pgrep -x ps2EntryRunner`.
- More env vars: `PS2X_PAD_DEBUG`, `PS2X_TEXREPL_STATS`,
  `PS2X_TEXREPL_LOG_MISSES`, `PS2X_MPEG_END_TRACE`, `PS2X_MPEG_AUDIO_TRACE`,
  `PS2X_GS_SCALE`, `PS2X_OPEN_SETTINGS`, `PS2X_MODS_DIR`,
  `PS2X_DUMP_RAM_FRAME`, `PS2X_WATCH_WRITE`, F9 = GS capture, F7 = log marker.

## What works (owner-confirmed)

Full speed in gameplay (GPU renderer + compiled VU1), audio, intro/logo
movies with full audio, cutscene lip sync, F2 settings menu (display,
resolution scale 1-4x, aspect, FOV, draw distance, controls/remapping,
classic/modern gamepad layouts, mods), first-launch menu, controller support
(Select+Start opens the menu; last-used controller, or one chosen in the
menu), mouse look on foot and in vehicles (wall/ground collision), PCSX2
texture packs from `mods/`, Windows build under Wine.

### The launcher round trip: one env var is what makes "return" honest

The launcher (its own repo, `thestoriescollection-launcher`) starts a game with
`PS2X_LAUNCHED_FROM_LAUNCHER=1` in its environment, and the settings menu asks
`ps2HostLaunchedFromLauncher()` before it labels the way out: started by the
launcher it says "Return to game select", started on its own it still says "Quit
to desktop". The action is identical either way (`runtime.requestStop()`); it is
the destination that differs, so the wording has to follow who started the
process. Run the game straight from a shell and nothing sets the variable, which
is exactly when the old wording is right.

The launcher waits for the game in a thread instead of blocking its loop, so the
window keeps answering the window manager while the game runs - a window blocked
for minutes gets flagged "not responding" (and offered up for killing) by some
window managers. It records that it is away from the menu at the moment it starts
the game rather than when a frame first observes it running: a game that exits
faster than one frame would otherwise never hand the menu back, which leaves the
screen black and the theme silenced for good. Found by pointing the launcher at a
script that exits immediately, which no real game does.

### Vehicle mouse look: the wall raycast must run after the game's own body

Owner-reported world flicker (2026-10-14, "geometry flickering in and out of
existence") in vehicles only, gone when the look offset decays. Captures
proved the *game* was not sending the world (the CPU rasterizer replay of the
same capture has the same empty frames), and the bad state persists in
stretches (up to ~50 frames) - its culling follows a broken camera state.

Cause: the "camera stops at walls" raycast (644407a) called the guest
`CWorld::ProcessLineOfSight` (0x1908C0) from inside the 0x2A2ED0 hook, i.e.
*before* the game's own 0x2A2ED0 body (which builds TheCamera). A guest call
that yields throws `EeDispatcherTransfer` (see `EeScheduler`), which unwinds
the host frames - so the game's body for that frame was skipped and the
camera state was left culled, until a frame whose offset was zero (driving).

Fix: the rotation stays in the hook (it must precede the camera build), the
*measurement* moved into `vcsMeasureCarWall` **after** `g_vcsOrigAfterCamProcess`,
and its result (pull position, or the overhead pose with Front/Up) is applied
by the next frame's rotation - a one-frame lag, invisible in practice. A
yield in the raycast now costs only that frame's clamp. `PS2X_CARLOOK_WALLS=0`
disables the wall stop; `PS2X_CARLOOK_DEBUG=<file>` logs the whole path
(rotate/restore/skip/measure/LOS).

## Open work, in rough priority

0. **DONE (2026-10-14): presentation shares the window's GL context.**
   The renderer creates its context in the window context's share group (GLX
   on Linux - raylib's GLFW here is X11-only, so the renderer moved off EGL;
   WGL on Windows), composes the display into a grow-only double-buffered
   texture pair and hands the id to the host, which draws it with raylib. No
   readback, no upload, no CPU copies. `PS2X_GS_PRESENT_SHARE=0` forces the old
   path (own surfaceless EGL context + capped readback + upload), which is also
   what tools get (gs_replay has no window). The renderer logs
   `[shares the window's context]` / `[own context]`.
   Lessons: (1) the pair must be grow-only - recreating it on any display-size
   change churned two full-size textures through the logos/FMV/intro and made
   the first cutscene stutter (owner-reported); (2) a texture shared between
   contexts is synchronised between them, so the host's draw waits on the
   renderer's frame - measure before assuming that is free.

1. **Release packaging (source-only).** A prebuilt exe would distribute
   Rockstar's (translated) code, and Take-Two DMCA'd re3/reVC. Plan: publish
   the runtime + VCS overrides + configs + scripts; users point a script at
   their own disc, which extracts it, runs `ps2recomp` with
   `vcs_clean.toml/.csv` and builds (Linux + Windows). Blocker: `vcs-vu1`
   comes from runtime captures (game microcode), so users need a way to
   generate it themselves (e.g. extract VU1 microprograms statically from the
   ELF for `vu1_recomp`). Without it VU1 is interpreted, about a third of the
   speed. Owner decides on publishing.

   **The owner wants a guided builder app** (no commands for players):
   - Wizard flow: pick disc image (.iso, later .chd), verify it's
     SLUS_215.90 by hash, pick an install folder, Build (progress bar + ETA),
     done. The result is a ready folder (exe, FFmpeg DLLs, `mods/`) plus a
     shortcut.
   - UI with raylib + ImGui like the game's settings menu. Linux + Windows.
   - Bundled pieces:
     - an ISO9660 reader (in-app; see `tools/bin2iso.py`);
     - the `ps2recomp` binary + `vcs_clean.toml/.csv` (addresses only);
     - a C++ compiler: llvm-mingw Windows-hosted build on Windows, a portable
       clang (or Zig's bundled clang) on Linux;
     - the runtime precompiled as a static library, so only the generated
       game code is compiled on the player's machine (~10-30 min).
   - VU1: generate `vcs-vu1` from the player's ELF (needs the static
     microcode extraction above). Feasibility check (2026-10-12): of 14
     distinct program starts in `scratch/vu1_capture.bin` (record = 16-byte
     header 'CVU1' + VU1State (664 B) + 16 KB code + 16 KB data + VU1State +
     16 KB data), 13 appear verbatim in the ELF (e.g. 0x1ad2b0, 0x2acaa0,
     0x382470). Check what `ps2Vu1ProgramKey` hashes: the program alone, or
     the whole micro-memory layout. If the latter, the extractor must also
     reproduce which programs are co-resident (follow the VIF MPG uploads in
     the game's DMA chains). Find the 14th (runtime-patched, or loaded from
     another disc file).

     Findings, later on 2026-10-12:
     - The key (`ps2_vu1_programs.cpp`) covers only the program itself: its
       startPC plus the address + words of every pair reachable from it
       (JR/JALR not followed). Co-resident programs don't matter.
     - The ELF has no inline VIF MPG headers before these blobs. RenderWare
       builds the uploads at runtime from pointer tables, e.g. at file offset
       0x382888: pointers 0x2ac240, 0x2ac228, ... stepping 0x18. Fully
       static extraction therefore means decoding RW's PS2 pipeline
       structures to recover load address + entry points per blob.
     - **Recommended alternative: self-learning VU1.** When the game meets
       an uncompiled program, it writes {code image, startPC, traced JR
       targets} to `vu1_cache/`. The builder (or an "optimize" step at next
       launch, with the bundled compiler) runs `vu1_recomp` on the cache into
       a plugin DLL/.so that the game loads at startup (registry merged with
       the built-in table). The builder can seed it with an automated run:
       intro + PS2X_PAD_SCRIPT fast boot + some frames of gameplay. This
       works for any game, ships no game code, and coverage grows as the
       player plays.
     - **Implemented (e6027eb): `PS2X_VU1_LEARN=<dir>`** records up to 4 MSCAL
       runs per program that has no compiled version
       (`<dir>/vu1_learn_<time>.bin`, capture format). Then
       `vu1_recomp <outdir> <dir>/*.bin`, and build with
       `-DPS2X_VU1_PROGRAMS=<outdir>`.
       Measured on the fast-boot run, starting from nothing
       (`PS2X_VU1_NO_COMPILED=1` for pass 1):

       | Learned set | Recorded | VU1 calls compiled | Late frames / 600 |
       |---|---|---|---|
       | Pass 1 | 13 programs, 2.6 MB | ~75% | – |
       | Pass 1 + pass 2 (built with the pass-1 set) | 15 variants | ~88% | 0-2 |
       | Full `vcs-vu1` (hand-made captures) | – | 100% | 0 |

       More passes keep closing the gap. Programs whose JR targets depend on
       co-resident code need runs under each layout.
     - **Plugin implemented:** `-DPS2X_VU1_PLUGIN_PROGRAMS=<vu1_recomp outdir>`
       builds `vu1_programs.so`/`.dll` next to the runner (target
       `ps2_vu1_plugin`). The game loads it at startup (or from
       `PS2X_VU1_PLUGIN`) and merges its programs with the built-in table.
       The plugin links nothing from the game: the 6 interpreter methods
       compiled code calls go through `Ps2xVu1HostApi`
       (`ps2_vu1_plugin.h`, shim in `src/lib/vu/ps2_vu1_plugin_shim.cpp`).
       Version + `sizeof(VU1Interpreter)` are checked, so the plugin must
       come from the same build/headers.
       Tested on Linux: a runner with no built-in programs + the 2-pass
       learned plugin ran ~90% of VU1 calls compiled, 0 late frames. The
       Windows DLL also loads under Wine (15 programs; the table is built on the
       first VU1 call, after the intro movie).
     - Next steps:
       - merging learned files across sessions;
       - possibly more runs per key;
       - the builder running 2-3 automated learning passes (the intro plus
         the fast-boot pad script) before handing over.
   - Suggested order:
     1. ISO reader + hash check, in a standalone library with a CLI test.
        **Done:** `ps2xBuilder/` (`ps2x_builder` library: `ps2x::IsoImage`,
        `isoBootFile`) and the `ps2x_iso info|list|hash|extract` CLI. It
        reads 2048-byte .iso and raw 2352-byte .bin images. A full
        extraction of `iso/vcs.iso` (341 files, 2.16 GB) is byte-identical
        to `iso/extracted` (empty dirs included). It cross-builds for Windows
        and runs under Wine. Known-good boot ELF hash (XXH3-128):
        `/SLUS_215.90 = 7f69b961cc427e3dcfd6582e25362be8`. The manifest
        should check this.
        **Requested: read from a physical DVD drive** (owner's idea, like
        PCSX2). PS2 DVDs are plain ISO9660 to a PC drive, so `IsoImage` can
        read the device instead of a file:
        - Linux: `/dev/sr*` works with fopen (2048-byte sectors).
        - Windows: use `CreateFileW(L"\\.\D:")` + `ReadFile` with
          sector-aligned offsets/lengths, not fopen.
        - Enumerate drives: Linux `/dev/sr*`; Windows `GetLogicalDrives` +
          `GetDriveTypeW == DRIVE_CDROM`.
        - Offer "extract straight from the disc" and "also save an .iso
          backup".
        - Blue CD-ROM PS2 games would need raw mode-2 reads (ioctl /
          `IOCTL_CDROM_RAW_READ`). Not needed for VCS/LCS (DVDs).
        - Real-drive findings (owner's external Blu-ray, Ultimate
          Spider-Man disc, 2026-10-12):
          - The drive needs ~8 s after inserting before reads work ("No
            medium found" until then), and can drop back to that state.
            The builder must poll and retry ("Waiting for disc...").
          - On a USB hub port it hit I/O errors and USB resets. A direct
            port fixed it. Show a hint when reads fail repeatedly.
          - It read at ~4 MB/s (a 4 GB disc takes ~17 min). Show progress +
            ETA, and extract straight from the disc rather than
            image-then-extract.
          - **Verified:** `ps2x_iso extract /dev/sr0` (559 s, 17 files,
            3.9 GB) is byte-identical to extracting a `dd` image of the same
            disc (dd: 532 s, 7.8 MB/s). Disc reading works on Linux.
          - **Windows drive reading VERIFIED on hardware** (owner, 2026-10-16,
            via the test build below): the drive was found and a real PS2 DVD
            read correctly, and a disc that is not one of the two games is
            reported as exactly that rather than as a failure. So the raw
            `\\.\D:` path is good, needs no elevation, and the disc feature is
            viable. The read failed only once, and that was a 0-byte copy on the
            USB stick, not the code - the test tool now publishes a checksum for
            that reason.
        - **Implemented:** `ps2x::opticalDrives()` and
          `IsoImage::open("/dev/sr0" | "D:")`, plus `ps2x_iso drives`. The
          owner has an external Blu-ray drive: test with `ps2x_iso drives`,
          then `ps2x_iso info|hash /dev/sr0` (expect the boot hash above).
        - Regeneration check (2026-10-12): `ps2_recomp` with
          `games/vcs/config/vcs_clean.toml` (paths made absolute) takes ~2.4 s
          and reproduces `ps2xRuntime/src/runner` (10,225 files) and both
          headers byte-for-byte. The builder's recompile step is
          deterministic.
     2. the wizard UI;
     3. drive `ps2recomp` + the compile with progress parsing;
     4. packaging the bundle per OS.

   Test on a clean Wine prefix / Proton, and on real Windows before
   release.

   **Future: "GTA Stories Collection" launcher.** The owner wants the
   builder to grow into one frontend that builds and launches both VCS and
   LCS (an LCS port is in progress separately). Design the builder around a
   **per-game manifest** from the start:
   - disc ID + hash;
   - recompiler TOML + function map;
   - the game's runtime overrides;
   - display name and art;
   - the default boot ELF.

   UI: a library screen showing each game as Build (no disc yet) or Play,
   with per-game mods and settings. Both games use the PS2Recomp runtime;
   keep game-specific code behind `PS2_REGISTER_GAME_OVERRIDE`.
2. **Custom radio station + Radio Off: mostly DONE (owner-confirmed).** A
   `music` folder (MP3/OGG/FLAC/WAV) plays as the in-vehicle "Custom
   Soundtracks" station on the game's own silent slot, and "Radio Off" is now a
   runtime toggle (hold D-pad Down ~0.75 s, or `M`), not a station.
   - **Station variable FOUND (2026-10-12, owner's F8 session in a car):**
     u8 at **0x4E4755** (static data; same values at 0x4E475B and in heap
     copies 0x1D859BA / 0x1FB5834). Values: 0 Flash FM, 1 V Rock, 2
     Paradise, 3 VCPR, 4 VCFL Radio, 5 The Wave 103, 6 Fresh 105, 7
     Espantoso, 8 Emotion 98.3, **9 Radio Off**. 0x4E4756 lags one step at
     one point (probably the station actually playing vs the one selected).
   - **Custom station = slot 9 (e6689ee):** the game has no spare station
     value (the field clamps >= 9 to 9), so the custom stop *is* the game's
     own silent slot 9; every transition is one the game expects. Slot 9's
     GXT name is written per frame: **"Custom" while the radio is on,
     "Radio Off" while the runtime radio is off** (vcsKeepCustomName; GXT is
     UTF-16LE, located by scanning for the strings). Natural tuning into 9
     shows "Custom"; the toggle's re-tune shows "Radio Off".
   - **Radio Off toggle (af935e3, a13b850, 83a69dc, f3d6ca3):** D-pad Down
     (host button, both layouts) or `M` held ~0.75 s toggles the runtime
     radio off/on; the game's own station pop-up shows "Radio Off" (slot 9's
     name while the runtime radio is off). Off = push the
     game to slot 9 **once** (desired/playing/last bytes + the station
     field), then leave it alone - the game's own update holds the silent
     slot, so no station audio plays while the engine/atmosphere keep going,
     and our music folder pauses. A station press (field departs 9) switches
     the radio back on. A per-frame push was tried and removed: it fought
     the game, never silenced the current station, and dragged every station
     change back to the silent slot.
     - Verified under Wine/Proton (2026-10-13): station cycle to slot 9,
       `[music] playing ...`, cycle continues past it (headless); the owner
       then confirmed the D-pad hold toggle and the radio feature set on the
       controller under Proton, at full speed.
   - Key addresses: the radio singleton is at **0x4E41E0** (+0x575 playing
     [*overloaded, see below*], +0x576 desired, +0x577/+0x578 lock flags,
     +0x57B last, +0x564/+0x568 owning vehicle); the station field is +0x2BA
     of the entity from `FUN_1B5768(0x514A90)`; the pending change is
     +1/+9 at **0x48613C**; the cycle is **mod 10**, applied by
     `FUN_0018b8d8`, and `FUN_0018aad8` (hooked as vcsRadioUpdate) clamps
     values >= 9 to 9. 0x4E4755 is not a reliable station value (radio
     *modes* 9..30 live there when the slot is unset); match the field.
   - Slot 9 is the leftover PC/PSP **"custom tracks"** station, disabled on
     PS2: GXT `FEA_CSS` = "CUSTOM SOUNDTRACKS:"; the frontend audio page
     `FRONTEND/DEFAULT/AUDPG.PSP` defines the selector (RADIO_STATION_MI,
     states S0..S9) plus CUSTOM_TRACKS_EDIT_MI and CUSTOM_RADIO_ICONS_MI;
     the ELF option list has HOOK_USE_CUSTOM_TRACKS.
   - HUD: the station logo pop-up (HUD message `0xB2`) still shows slot 9's
     "Radio Off" logo on the custom stop (the owner's logo overlay was
     removed earlier at the owner's request).
   - `ps2_music_station.cpp` runs the folder (raylib streams; F10 skips a
     track, Mods tab has on/off, volume, shuffle, open folder, rescan).
   - Done (2026-10-13, commit 19ad92f): track skip on D-pad Right/Left single
     press (F10 = keyboard next) with real previous-track support; the music
     pauses with the game's own pause menu and resumes on the same track
     (pause flag **0x4CD2AC**, set 1/0 by the open/close handlers at
     0x2F4D64 / 0x2F4D70 on the START press, gates the station query); a
     Mods tab settings toggle for the Radio Off hold (`radio_off_enabled`);
     input device prompts (Xbox / Steam / PlayStation / keyboard wording in
     the Mods music help, now-playing overlay, settings footer).
   - Remaining (one item): the **frontend audio page selector** for the
     custom stop. `FRONTEND/DEFAULT/AUDPG.PSP` declares the radio selector as
     `<9c ad="10">` (= state count) with `S0..S9 = FEA_FM0..8 + FEA_NON`, and
     c81bf35 renames the 10th key `FEA_NON -> FEA_CSS` in the read buffer
     (CD.cpp `readRegisteredCdSectors`). Owner check (2026-10-13): the
     in-game audio menu still cycles only the nine real stations - no 10th
     entry, nothing "Radio Off"/custom. So the page's 10th state is not what
     the selector enumerates; the option list is built ELF-side.
     - New findings (2026-10-13): the hook registry is `FUN_003e2530`
       (0x3E2530, dispatcher over a jump table at vaddr **0x4C1D80**, 20
       entries, each case returning the hook's name string from the table at
       0x4C1BF8+): idx 12 = `HOOK_USE_CUSTOM_TRACKS` (name at 0x4C1CC8),
       idx 13 = `HOOK_RADIO_STATION`. Next probes: find who builds the audio
       page's option list (the option -> hook mapping) and whether the
       custom-tracks availability gate hides the 10th station entry; when
       found, the runtime can satisfy the gate (a music folder exists).
       ELF note: single PT_LOAD, file offset + 0x1000 = vaddr (text/data).
     - When tuned via the menu, it must set the station the way the L1/R1
       path does (slot 9 + `g_vcsCustomStation`), else the vehicle tunes to
       a normal station on entry.
3. **Texture-pack overhead - measured and located (2026-10-13).** Owner
   re-tested the airport: slight frame drops persist with the HD pack. A/B
   on the fast-boot drive (Linux native, GPU renderer, `PS2X_PERF_LOG`):
   the render thread runs **GsGpu ~44-57% with the pack vs ~24-30% without**
   (~+20 pts), which tips the airport into late frames. New
   `PS2X_TEXREPL_STATS` fields (added today) split it:
   - **Compatibility/perf sweep (2026-10-14, after "runs on as many machines
     as possible").** `gs_replay` on the airport capture, texture pack on,
     from the same start: **scale 1 348 -> 151 ms, scale 4 360 -> 272 ms**,
     CPU rasterizer 6.20 -> 6.08 s (54 presents). Four changes:
     - **The upscaled frame was round-tripped through the CPU twice**: a full
       readback of the composed frame, then a zero-fill + row copy + upload
       back on the main thread. The readback is now capped to the host window
       (published by the main thread, `PS2X_GS_PRESENT_SIZE` overrides) with a
       GPU downscale, `copyLatchedHostPresentationFrame` hands the latch over
       (swap) instead of copying, and the host uploads it straight when it
       fills the texture. Present 5.13 -> 2.68 ms/frame at scale 4; the capped
       frame is an exact box filter of the 4x compose (mean diff 0.19/255).
     - **The display is composed on the GPU at scale 1 too** (it only did so
       above scale 1, so the default configuration paid a full 4 MB readback
       plus a CPU merge every frame). Equivalent output (0.26/255 average,
       <=9/255 single pixel), identical replacement counters and vram hash.
       `PS2X_GS_GPU_COMPOSE=0` restores the CPU compose.
     - **Repeat presents are skipped.** The game renders ~30 Hz and presents
       60 Hz, so half the presents carry no GS input at all: the GS frontend
       tracks an input sequence and skips the recompose, and the frame id lets
       the host keep the texture it has instead of re-uploading (a latch with
       nothing new no longer clears the screen either).
     - `vcsKeepCustomName` searched all of RAM **every frame** until the GXT
       names appeared (twice over: the station keys and "Radio Off"); now at
       most every 60 frames, like the button-text search.
   - **Fix 1 landed (2026-10-14): T4 uploads are mirrored whole blocks.**
     Every VCS T4 upload is aligned to 32x16 tiles (= whole 256-byte GS
     blocks: 100% of 2883 T4 transfers on the airport capture, 10.25 MB of
     the 11.8 MB mirrored), so `GSCpuBackend::UploadImage` now writes those
     tiles in one pass from the row-major source, applying the in-block
     layout through a compile-time nibble permutation
     (`GSMem::WriteT4Tile`, inverse of ColumnTable4), instead of walking the
     page/block/column tables per pixel.
     - `gs_replay` on the airport capture (pack active, GPU renderer):
       **mirrorMs 42 -> 16-17**, hash counters identical
       (draws=3944 hits=3408 misses=536 hashed=3092); the CPU-backend replay
       is bit-identical (`vramHash=9e8e70dc2688b4a1`), and a standalone
       check writes 8192 geometries with both writers - all identical.
     - Not covered yet (previous path, unchanged): non-tile-aligned rects,
       T8 (0.75 MB, 99.8% tile-aligned so it could use the same trick), C32
       (0.77 MB), split uploads (none seen for T4).
     - Measurement aid: `PS2X_TEXREPL_STATS=<interval>` (or `=1` for 300)
       now also flushes on teardown, so 54-frame captures report.
   - **Owner reported general gameplay slowdown again (2026-10-13 late)** -
     mostly gone before the prompt work; visible while loading texture
     packs. Two causes found and fixed in 81346e7 (see the prompt section):
     (a) the inline prompt load was keyed by file size, so small pack files
     also decoded on the render thread mid-frame while textures streamed in
     (the camera-movement flicker the owner saw), and (b) every prompt
     rewrite bumped the index version and the renderer deleted + reloaded
     every replacement texture, the whole pack included. The index now
     carries per-file stamps and only changed entries reload; the inline
     load is limited to entries flagged `inlineLoad` (the prompt art).
     Re-measure after this: `PS2X_PERF_LOG=1 PS2X_TEXREPL_STATS=1`.
   - **mirrorMs ~590-950 ms per 5 s period (~16-19% of the render thread)**
     = `GSCpuBackend::UploadImage` decoding uploads into the CPU copy.
     (Before Fix 1 above; the T4 block path cut what gs_replay sees to ~40%.)
   - **hashMs ~16-30 ms** = texture/CLUT hashing - negligible.
   - Per-format bytes: **PSM 0x14 (T4, 4-bit palettized) is ~95%** of both
     mirrored and hashed bytes (~140 MB per period; T8 ~2-9 MB, C32 ~6-9 MB
     mirrored but never hashed). The hash reads the same bytes ~30x faster
     (block-linear 256-byte reads + XXH3 vs the per-pixel swizzle
     `WriteRowT` -> `PixelStorageTraits::Write` with nibble RMW).
   - Design direction (the doc's "lazy mirror keyed by upload content"):
     keep raw uploads in a side overlay (stream + transfer rect) and, for a
     texture region fully covered by one upload, **synthesize the 256-byte
     hash blocks from the stream via the fixed in-block column table**
     (`ColumnTable4` etc.) instead of maintaining `m_vram` per pixel;
     overlapping/mixed regions fall back to the current "CPU-stale =
     hash miss" policy. In-block layouts are the GS column interleaves (the
     column tables in `ps2_gs_memory.cpp`), NOT row-major - a naive block
     memcpy would corrupt data. Estimated 5-10x on the mirror cost.
   - Verification harness: `tools/gs_replay` frame diffs + comparing
     `[texrepl]` hit/hash counts before/after (identical hits = identical
     mirrored bytes).
   - **Where the pack's +15% actually is (2026-10-16, measured).** Not the
     per-draw replacement lookup: caching the resolved replacement per texture
     (keyed by its page generation, cleared with the index) changed the airport
     replay by nothing. The +15% is the **replacement textures' loading and
     upload** - 555 HD textures per pass, roughly 280 MB pushed to the GPU,
     ~0.2 s at the observed rate, which matches the 0.211 s difference exactly.
     It is a one-time cost per texture and is already capped at 6 MB/frame to
     avoid the hitch on entering a new area. The per-draw caching is kept: it is
     correct and cheaper, just not the bottleneck.
     - **ANISOTROPY WAS THE AIRPORT SLOWDOWN (owner-measured 2026-10-16).** Every
       replacement texture was created with `GL_TEXTURE_MAX_ANISOTROPY = 16` - 16
       taps per sample at grazing angles, on textures far larger than the texture
       cache, which is where the airport's surfaces sit. Owner A/B at the airport
       with the pack: **16 drops into the 30 fps range, 4 shows no slowdown at
       all.** The default is now **4** (`PS2X_TEXREPL_ANISO` overrides; 1
       disables). This is why it never showed in the replay: it is GPU sampling
       cost and the GPU is asynchronous, so the wall time is blind to it - a
       useful lesson for the next GPU-bound hunt.
   - **T4 mirror tables (2026-10-16).** `WriteT4Tile` ran 512 iterations per
     256-byte block, each doing two `(x, y)` table lookups and two
     `y * srcRowBytes` multiplies. The destination byte's two nibbles come from
     fixed places in the source, so those offsets and shifts are now
     precomputed once per upload (`GSMem::T4TileTables`). On the airport capture
     (594 MB, 228 presents): **mirrorMs 231 -> 137** (and 234 -> 137 with the
     pack on), replay 6.11 -> 5.12 ms/present, with `vramHash` bit-identical in
     both configurations (`bc5111eb0bb24b61` without the pack,
     `6bcdb83ff62b0f44` with it).
     - Measured split (airport capture, pack on): the mirror is **~12%** of the
       replay and runs whether or not the pack is active; the pack's own **+15%**
       (62,017 replacement hits + 555 loaded textures) is the draw-side
       replacement path, a separate piece of work. T8/C32 are 2 MB of 159 MB, so
       extending the block path to them is not worth it.
   - **F9 capture stalls fixed (2026-10-16).** Pressing F9 lagged the game
     badly. `gsCaptureRecord` wrote every GIF packet and register with
     `std::fwrite` as it arrived, on the game thread with the GS state mutex
     held, so the GS serialized against the I/O for every record. A busy
     three-second airport capture is **594 MB** (measured; a second one was
     465 MB) - tens of thousands of small writes. It was not the disk (the owner
     ran from the SSD): it was the per-record I/O under the mutex. Records are
     now collected in memory and written once when the capture ends.
4. Mods: file overrides (replace disc files from a mod folder). `.pnach`
   memory patches are now applied (be626b9): PCSX2 `patch=1,EE,ADDR,TYPE,VALUE`
   byte/short/word lines from enabled mod folders are written to EE RAM each
   frame. New setting "Apply .pnach memory patches" (`pnach_cheats`, default
   on; Mods tab). `extended` writes and conditional/pointer code words are
   skipped. Unit tests in `ps2xTest/src/ps2_mods_tests.cpp`.
5. Controllers: PlayStation button names in the menu (done), rumble (done),
   analog trigger pressure (done, needs an owner feel-test - see below).
   - **Rumble.** The game drives the two DualShock motors through
     `scePadSetActDirect`, which used to be a no-op. Nothing in the input stack
     could pass it on: raylib's GLFW backend stubs `SetGamepadVibration` out
     ("not available on target platform") and GLFW 3.4 exposes no rumble at all,
     so each platform drives its own device layer - `src/lib/ps2_pad_rumble.cpp`
     (its own translation unit so `<windows.h>` and raylib.h never meet, which is
     what made the first Windows build fail). Linux: kernel force feedback on the
     pad's evdev node (`EVIOCSFF`, the same `/dev/input` tree
     `ps2BuildEvdevMapping` already walks), one `FF_RUMBLE` effect with
     `replay.length = 0` updated in place; works for DualSense, Xbox and Switch
     pads. Windows: XInput, loaded by hand, applied to every slot - Xbox-family
     pads only; a DualSense/DS4/Switch pad on Windows needs HID output reports,
     which this does not do. `PS2X_PAD_RUMBLE=0` disables it. Verified: the game
     calls the stub with `port=0 slot=0` and a data pointer (bytes `00,e6` - the
     large/left motor), and the host's effect upload+play is accepted by the
     kernel (`effect=0 rc=0`).
   - **Analog trigger pressure.** The DualShock 2's face buttons were
     pressure-sensitive and the game reads that pressure (`scePadEnterPressMode`
     is called; `buttonMask` stays 0xFFFF). In a vehicle the port maps RT to
     Cross (accelerate) and LT to Square (brake), but collapsed the trigger to a
     boolean at `GetGamepadAxisMovement(...) > 0.0f`. The pressure now flows:
     `readState` turns the trigger axis into a 0..255 pressure for whichever PS2
     button the trigger is mapped to (`data[8 + bit]`), `readPadPortData` copies
     it into `PadInputState::pressure`, and `pressureValue` prefers it over the
     digital answer. The rest point is detected (lowest axis value seen) rather
     than assumed, so a pad whose mapping has no trigger axis cannot become a
     throttle that creeps. A pull past ~10% sets the button bit too, so the game
     gets both bit and pressure across the whole travel. Verified the value
     reaches the game's read buffer (`[pad] ... pressure: 14:153` with
     `PS2X_PAD_DEBUG=1`).
     **The game's side is now traced, and the offset is right.** The pad poll is
     `FUN_002843d8` (called from `FUN_00284390`, which gets the per-player pad
     state from `func_2852A8(i) = 0x005147A8 + i*0xD4`); it passes the read
     buffer as `$sp` and then reads every pressure byte: `0x8`..`0x19` -> right,
     left, up, down, triangle, circle, cross, square, L1, L2, R1, R2, i.e. the
     same `data[8..19]` layout the port writes, with Cross at `0xE` and its
     field at `+0x2A` of the pad state.
     **But it scales each one by 4 and clamps at 255** (`func_284370`:
     `dest = min(dest << 2, 255)`, the store happening in the `jal` delay slot).
     So a raw 0..255 saturates the game's value at raw 64 - only the first
     quarter of the trigger's travel would be analog, which is exactly why the
     owner reported it still feeling on/off. Full trigger travel now maps to a
     raw maximum of 64 (`PS2X_PAD_PRESSURE_MAX`, default 64) so the game's whole
     range lands across the whole travel. Needs an owner feel-test; if the car
     feels underpowered instead, the game's range really is 0..255 and the max
     should go back up.

    - **Proportional throttle - PARKED (owner's call, after five attempts).**
      The owner's ask: a car that accelerates with the trigger instead of being
      flat out or stopped. It was built and did work - the car was proportional -
      but at a partial pull it **surges**, and that was never solved. The owner
      called it: **the option is removed from the menu and the settings, and the
      hooks are gone**, because a broken setting in the menu is worse than no
      setting. The knowledge below is kept in case anyone wants to try again.

      What the game actually does (all in the vehicle):
      - `+0x25C` - the **throttle request**, signed: +1 accelerating, -1
        braking/reversing. The strongest trigger correlation of any vehicle
        field.
      - `+0x66C` the **gas pedal**, `+0x670` the **brake pedal**, floats.
      - `FUN_001b9360` is the **ramp**: it returns `min(abs(request), pedal +
        0.09)`, i.e. the pedal eases toward the request. `FUN_001b8a68` is the
        control update (writes the pedals, 0/1, from the button), `FUN_001b5d68`
        the vehicle update.
      So the game *has* a small analog-capable model; only the input is a
      button.

      What the port did (now removed, `game_overrides.cpp`): wrote the request
      and both pedals from the triggers (`RT - LT`, low-passed) at the entry of
      the vehicle update (`FUN_001b5d68`), at the control's tail, and at the
      entry of both pedal readers (`FUN_001b9360`, `FUN_001ba600`). The four
      `replaceFunction` calls are gone, so the game is untouched again.

      Why it still surges (from `PS2X_THROTTLE_TRACE=1`, now removed): the game
      writes **+1.0 into the request in the *player* update, once per presented
      frame, before the vehicle update runs**, and its own player-side logic
      consumes it there. The trace shows `upd in_was=1.000` then `upd
      in_was=0.398` - our write lands, and the whole vehicle update sees it, but
      the earlier write has already been used. That matches the owner's
      observation exactly: **the stutter is fast and close together at a light
      pull and longer and more spread out deeper in**, which is what gear logic
      acting on a full-throttle signal while the engine only gets a partial one
      would do.

      The one lever not tried: **suppress the Cross/Square buttons in a vehicle**
      (so the game's own request write produces 0 and its player-side logic sees
      "no input") while the port keeps driving the request. Untested, and it
      risks the player-side logic behaving oddly, but it is the only way left to
      reach that window.

      Debug tool kept: `PS2X_THROTTLE_SCAN=1` dumps the vehicle's words to
      `throttle_scan.txt` (config folder and working directory).
6. VU1 compiled-program coverage (captures in heavy areas, e.g. the airport;
   regenerate `vcs-vu1` with `vu1_recomp vcs-vu1 <captures...>`).
7. **Player-selectable render API (Vulkan; D3D11 for Windows).** Owner's ask:
   a settings choice of rendering API --- Vulkan on Linux, Vulkan + DirectX on
   Windows. Feasible by design: the GS frontend is decoupled from
   rasterization behind the 42-line `GSRasterBackend` interface
   (`runtime/gs/gs_backend.h`), with two living backends (`GSCpuBackend`,
   the bit-exact reference, and `GSGpuBackend`, the GL 4.6 surfaceless
   renderer in `src/lib/gs/gpu/gs_gpu_renderer.cpp`, ~2.4k lines; selected
   with `PS2X_GS_GPU=1`, reading `ps2HostSettingsGpuRenderer()`). A Vulkan
   (or D3D11/12) renderer is a new backend in the same slot: same frontend,
   same local-memory model, same GS-semantics knowledge (blend
   `(A-B)*C>>7`, swizzles, DATE, FBMSK), same frame-diff verification via
   `tools/gs_replay` and `PS2X_GS_CAPTURE`.
   - **Effort (honest):** ~3-6 weeks each for Vulkan and ~the same for D3D11
     (D3D12 more), for someone fluent in Vulkan and this GS pipeline. The
     bulk is backend boilerplate (surfaceless/headless present, sync,
     barriers, descriptor/memory management, SPIR-V/HLSL re-expression of
     the uber shader) plus present-path wiring (today the GL backend reads
     the DISPLAY back into a `PresentationFrame`; the host window side is
     rlgl/GL and would need a shared-texture/DMA path for Vulkan/D3D).
   - **First cheap step:** stop gating the backend on the env var; add a
     "Renderer: CPU / OpenGL" dropdown in the F2 settings menu (the menu
     already has internal resolution), so the selector UX exists before the
     new backends do. Then each new backend is one more entry.
   - **What it buys:** on Linux native, OpenGL passthrough is already full
     speed --- Vulkan there is player choice and future-proofing, not
     perf. D3D11 pays off on real (non-Wine) Windows and under Wine via
     DXVK. Today's actual Windows pacing problem was sync, not graphics:
     fixed by running under Proton (ntsync), see the Build and run note.
   - Don't start until the radio feature, its owner test under Proton, and
     the remaining radio niceties are closed out.
8. **Button prompts: per-device replacement of the game's on-screen button
   glyphs. Runtime DONE + verified (2026-10-13); asset keys pending.**
   - Mechanism: the game's prompt glyphs are ordinary uploaded textures, so
     PCSX2-named replacement PNGs swap them, exactly like texture packs.
   - **`PS2X_TEXREPL_DUMP=<dir>`** (new): writes every texture the
     replacement matcher sees as `<tex0Hash>-<clutHash>-<bits>.png`, the
     PCSX2 naming - i.e. what a pack file for that texture must be called.
     RGBA expansion is best effort (CLUT/16-bit via the same rules as the
     hashing). With **`PS2X_TEXREPL_DUMP_ALL=1`** it also captures textures
     the matcher refuses (unsupported formats, region clamp): use it when
     hunting glyphs that may be drawn differently. If a glyph never appears
     even then, it is baked into other art (e.g. a shared sheet) rather than
     being a separate texture.
   - **Prompt set layer** (new): PCSX2-named PNGs in `prompts/<set>/` next to
     the executable (or `PS2X_PROMPTS_DIR`), sets `xbox`, `steam`,
     `keyboard`, `dualsense`, `dualshock`, `switch`. Loaded into the
     replacement index
     AFTER the mods on purpose (last file wins), so a user texture pack can
     never override the prompt glyphs - verified: 791 test textures, 511
     overriding the HD pack's keys.
   - **Setting `button_prompts`** (Mods tab): Auto (follows the active
     device via `ps2InputDeviceKind()`, per-frame re-resolve) / Xbox /
     Steam / Keyboard / PlayStation (DualShock + DualSense) / Switch / Off.
   - **Pipeline: `tools/make_prompt_packs.py`** (also in
     `games/vcs/tools/`): manifest (`packs.json`) driven compositor; renders
     the AL2009man prompt SVGs (honest queue: scrape
     https://github.com/AL2009man/Gamepad-Asset-Pack, submodule
     Gamepad-Prompt-Asset-Pack, MIT with attribution) via `rsvg-convert`,
     single-glyph or atlas-cell placements, Switch face buttons come from
     the Pro Controller FaceButton sheet (2x2, A=bottom), Steam falls back
     to the Xbox glyphs (Deck layout; swap art later), keyboard set renders
     keycap text labels (defaults per the port's default bindings; adjust
     per action).
   - **Identification progress (owner, 2026-10-13 evening):** the pause-screen
     controller diagram is `955f573d239ed3c1-ed1f7a64aaa8fafc-00002213.png`
     (256x256); face buttons + d-pad are the 16x16 tiles:
     `60a3a644...-00001113` = Cross, `d0d9f396...-00001113` = Triangle,
     `a1b7cc92...-00001113` = D-pad Up, `3c60f292...-00001113` = D-pad Right
     (owner-corrected), `784c33b4...-00001113` = D-pad Left and
     `47e8e408...-00001113` = D-pad Down (by mirror/elimination; owner to
     double-check). **Done already:** the five sets (xbox, steam, dualsense,
     dualshock, switch - PS split into DualSense/DualShock 4 at the owner's
     ask, detection by name) each carry the controller-page art (from the
     Controller Asset Pack bases) + the 2 faces + 4 d-pad arrows at 4x;
     verified: `[prompts] xbox: 7 textures (7 overriding mod packs)`.
     Still to find on the contact sheet: Circle, Square, Start/Select, and
     L1/R1 if they exist as glyphs (keyboard set = text keycaps via the
     script; no controller art for it - intentional).
   - **Controller portraits (owner picks, 2026-10-13 late):** all five use
     the pack's **full** layout exports (the `*_base.png` files are the
     "No Thumbstick and Triggers" exports, so the sticks looked missing):
     Xbox Series X **Black** (`Xbox Series X Controller Overlay.png`),
     Switch Pro (`Switch Pro Controller Overlay.png`), DualShock 4
     **Jet Black** (`DualShock 4 Controller V2 Model Overlay.png`),
     DualSense **White** (`DualSense Controller Layout (White-Black).png`),
     Steam Deck (`Steam Deck VSCView Overlay.png`). The Steam Deck
     templates paint the screen flat green (every variant, including the
     "Black" theme SVG), which looked wrong on the game's dark page - the
     `screen_black` option in `packs.json` flattens green-dominant pixels
     to black inside `compose_controller()`. The white halo outline was
     dropped after the owner test.
   - **Keyboard set (2026-10-13 late):** now ships the six menu glyph
     textures too (cross/triangle + the 4 d-pads), so menus stop showing
     the PS2 icons for keyboard users; the keyboard art is a keycap square
     (dark rounded square + gray rim + white label): W/LMB/S/E for the
     face buttons (port defaults) and the arrow keys for the d-pad (the
     Menu directions are bound to the arrow keys). The font-atlas cells
     use the same keycaps. `keyboard/` = 6 textures + sheet + previews +
     manifest; no controller diagram (intentional).
   - **Button-name text follows the live mappings (2026-10-13 late).** The
     GXT button words in menus (the map screen's "L1 and R1 buttons",
     Empire menus' "CROSS - ...", the controls lists) now resolve at
     display time: gamepad sets ask `ps2controls::hostButtonForPs2(ps2)` -
     the host button that currently reaches that PS2 button in the current
     context, preferring Modern/custom remaps over the classic identity
     (so "Modern" shows RT for fire, not B); keyboard sets use the action
     that presses it (`vcsKeyboardAction`: menu actions first - the map
     options line reads ENTER - then the on-foot / in-vehicle action of the
     pad context). Text is re-applied whenever the prompt set, a binding,
     the gamepad map/layout or the pad context changes
     (`vcsMappingSignature`; the pad backend publishes
     `ps2controls::setCurrentContext`). Keyboard menu strings that embed
     the symbol tokens ("Hold ~X~ to display the map options.") are
     rewritten to the bound key name with a fit-check ("Hold ENTER to
     display map options."); console sets keep the tokens because their
     glyphs already follow the set. Limitation: the *glyph* textures stay
     positional (baked per set), so with Modern/custom layouts an icon can
     still show the classic button - runtime art generation would be the
     follow-up. `PS2X_PROMPT_TRACE=1` logs the site counts.
   - **Menu layout decoupled from gameplay (2026-10-13 late).** The game's
     pause menu / map is a third pad context (`ps2controls::Menus`), probed
     from the game's own pause flag (`0x004CD2AC`, the same flag the Custom
     Soundtracks query reads) and wired like the vehicle probe. The menu map
     stays classic PS2 in every preset, so Modern / custom remaps never
     change how the menus navigate, and menu-only edits never leak into
     gameplay. The Gamepad buttons table gained the **In menus** column; the
     runtime prompt art resolves the keyboard cells to the menu actions
     (Enter / Backspace / arrows) while a menu is open, and the controller
     art falls back to the classic glyphs (identical to the shipped art).
     **B is the back button** (modern convention): the game's own menus read
     Triangle as back, so the menu map sends Triangle from B, while X and Y
     keep the base game's Square (target) and Triangle (legend) roles on the
     map. Circle is no longer sent in menus (nothing needs it so far); the
     prompt text/art follow via hostButtonForPs2. The keyboard's Backspace
     already sends Triangle. Keyboard in menus: WASD presses the D-pad
     (discrete steps, like the arrow keys - no analog stick), the mouse wheel
     zooms the map (up = R1 in, down = L1 out) instead of cycling weapons,
     and Q / RMB stay the L1/R1 map zoom keys. Each wheel notch holds the
     zoom button ~6 frames and fast scrolling queues steps, so the wheel
     keeps up; the pad's L1/R1 feel is unchanged. The Button prompts
     dropdown sits at the top of the Controls tab, under the controller
     selection.
     - **The keyboard's menu keys are their own set** (Controls -> Menu
       keys): accept = Enter, back = Backspace, the directions = arrows +
       WASD, Map: target = Space, and Skip cutscene = Enter. Only these work
       in the game's menus - the gameplay bindings no longer leak in (and
       the menu keys never fire in gameplay), except Skip cutscene, which
       sends Cross while the cutscene probe reports a cutscene.
     - **Cutscene flag (2026-10-13 late).** F8 RAM dumps in two cutscenes
       (B/C) and two gameplay states (A/D) diff to 33 word-level boolean
       candidates; `vcsInCutscene` reads **0x006F4D14** (heavily referenced
       by the game code, 15 functions across modules). The pad logs
       `[pad] cutscene=0/1` on every change so the flag can be verified
       live; `PS2X_CUTSCENE_FLAG=<hex>` overrides the address for testing.
       Other candidates if it is wrong: 0x6F4DA0, 0x6F4D64, 0x6F4D10,
       0x74D3F0, 0x486E28, 0x4874F4, 0x4CDA40, 0x711E94.
       The **FMV movies** (opening logos / credits) are not covered by the
       flag: `isMpegMovieActive` (MPEG stubs) remembers the presented frame
       of the last picture served and reports a movie for 45 frames after
       it; the probe returns true for either. Owner-confirmed the in-engine
       skip works; the movie path is new.
     - **The map zoom on the keyboard is the mouse wheel alone**: the wheel
       prompt replaces the L1/R1 icon pair (one "MOUSE WHEEL" keycap, the
       second icon blank; the map's "L1 and R1 buttons" hint reads "Mouse
       Wheel"), and Q / RMB no longer press L1/R1.
     - **The game's Widescreen option follows the port's aspect setting**
       (2026-10-13 late). The option state itself is the only thing that
       needs writing: a 32-bit flag at ***(0x48F3D0) + 0x30** (0 = 4:3,
       1 = 16:9). It was pinned with a guest-store trace (`PS2X_WATCH_BOOL_WRITES`,
       now filtered to the static data range + F7 markers in `boolwrites.log`):
       the in-game toggle stores it via the menu setter at 0x2AF660
       (`sw a1, 0x30(a0)`), and the game re-derives everything else from it
       every frame, so the port must not write the derived values:
       - 0x487488 = the 2D/HUD aspect (1.33333 / 1.77778, 1.66667 during
         cutscenes) by 0x2653F0;
       - the camera half-FOV pair at CCam(*(0x51E490)) + 0x60 / + 0x64
         (tan(FOV/2) and tan(FOV/2)/aspect) and 0x742AC4, applied by the
         renderer every frame via 0x1AB9E0;
       - the HUD layout scale (0.85 / 0.68) read live from the getter
         0x2AF658 by 0x2AF7C8 / 0x31DB20 and friends;
       - the frontend's "Widescreen On/Off" text (the option page value
         dispatcher at 0x2AEC9C reads the same getter).
       Earlier attempts wrote the derived values (0x487488, 0x742AC4,
       0x486E28) and were overwritten every frame - and would have broken
       the cutscene aspect. 0x486E28 is also derived per frame now, so the
       port no longer writes it; only the display-config bit 0x20 of
       0x6F2889 / 0x6F288D / 0x6F2A49 / 0x6F2A4D is still replicated
       (the game only writes it at toggle time). `PS2X_WIDESCREEN_FLAG`
       overrides those addresses; `PS2X_WIDESCREEN_LOG=1` prints each
       option write. **Owner-verified working** (2026-10-14): 16:9 turns
       the in-game option On with the correct HUD, 4:3 turns it Off.
       Toggling the option in the game menu flips back within a frame -
       the port's aspect setting is the master by design.
   - **Map-screen icons covered (2026-10-13 late).** The map legend draws
     its button icons as their own textures; miss logging + single-key
     colour tests on the owner's capture 6209 found them:
     `c075546801b74bf1-6b7c470bee19b607-00001113` = square (Target),
     `1c274bfc4808cd11-21fc390fe03ad8c0-00001113` = L1 and
     `23ece69c64b71407-f6ce098a91a7ac7b-00001113` = R1 (the Zoom pair).
     xbox/steam/switch get face-button discs (X / Y) and shoulder pills
     (LB/RB, L/R); the keyboard set gets keycaps with the keys that work on
     the map (SPACE = Jump sends Square, Q = Interact sends L1, RMB = Aim
     sends R1). The PlayStation sets use the pack's own face art so the
     icons match the tutorial HUD cells (DualSense = the white-style
     square, DualShock = the Color square); both use the same dark L1/R1
     pill (dark fill + light print, like the black bumpers on the real
     controllers). The cross/d-pad legend icons were already covered.
   - **Runtime prompt art (2026-10-13, both milestones).**
     `ps2_prompt_art.cpp` regenerates the prompt textures from the live
     bindings and writes them to
     `<config>/ps2recomp/prompts_rt/<set>/`; the prompt loader reads that
     folder after the shipped `prompts/<set>/` files (last wins).
     - Keyboard: keycaps labelled with the bound keys (same action labels
       as the shipped art, arrow glyphs included). Long labels ("ENTER",
       "BKSP", "RMB") draw as wide keys - like a real Enter key - with the
       text across the full tile, instead of squeezed into a square; the
       shipped keycap generator (`tools/prompt_glyphs.py`) matches.
     - Controller sets: the glyph of the host button that currently reaches
       each PS2 button (`hostButtonForPs2`), so Classic / Modern / Custom
       and the on-foot vs in-vehicle contexts show in the art too (Modern
       on foot: the Circle cell becomes an RT pill, L1 a B disc, ...).
       Styles: Xbox/Steam/Switch letter discs (brand rings, official
       colours), PlayStation symbol discs (DualSense white style with gray
       symbols, DualShock Color style with the pack's sampled colours),
       pills for shoulders / sticks / menu buttons, arrows for host D-pad
       inputs.
     - Rewrites are debounced (~45 stable frames) and keyed by an
       effective-content signature, so Classic layouts hash the same in
       both pad contexts (riding a vehicle rewrites nothing) while Modern /
       Custom rewrite when the context changes; stale files are removed so
       dropped mappings fall back to the shipped art.
     - Validated with a standalone hidden-window raylib harness for all six
       sets plus a Modern-layout case; both executables relinked.
   - **Tutorial cells filled (2026-10-13 late).** The HUD tutorials draw the
     button cells small (a ~32px atlas block at about 0.7x on screen), so
     the flat shoulder pill (44% of the cell height) left the L2/R2 hints
     (shoulders bound to face buttons, e.g. Modern driving: LT/RT) only a
     few pixels tall. Pills drawn into the sheet cells now fill 80% of the
     cell; the map icons keep the flat shoulder look. Verified by
     replaying the owner's two vehicle tutorials (1996 = "Press X to
     accelerate", 2520 = "Press Square to use the vehicle's brakes") with
     the owner's settings (DualSense + Modern): the prompts now read as
     large L2/R2 pills.
   - **Menu flips rebuild only the prompt layers (2026-10-13 late).** The
     runtime art rewrites on every pad-context change (menu open, vehicle
     entry/exit with remapped layouts) and each rewrite ran a full
     `ps2ModsRefresh`, rescanning every mod folder (~1 s with a big
     disabled pack). The mods' replacement files are now cached in
     `g_modsIndex` and `ps2IndexWithPrompts()` rebuilds the prompt set +
     runtime art on top of it; the full scan runs only at startup, on
     Rescan and on mod toggles.
   - **Prompt flips are now same-frame (2026-10-13 late).** The wrong keys
     for a moment when opening the map (owner: "E for back" then Backspace)
     had three sources: the menu debounce still waited two frames (now one),
     the replacement textures reloaded through the async worker (files up to
     256 KB - all the prompt art - now load inline on the render thread at
     the first draw that needs them), and the pause flag is set after the
     port reads the pad, so the art was always one frame late. A fresh Start
     press now predicts the toggle for the art only
     (`ps2PromptArtSetContext`; the pad mapping keeps the real context until
     the flag is seen) and the prompt layers rebuild at the pad read, early
     in the frame, so the new art lands before that frame's draw. Big pack
     textures still use the worker. This also removed the reload latency on
     vehicle entry/exit (relevant to the slowdown report).
   - Also done: the dropdown now has **"Original (PS2 art / texture packs)"**
     (= Off, shows the game's own art and any texture-pack art), it lives in
     the **Controls tab** (next to the gamepad layouts) with a glyph preview,
     and the prompt sets load independently of the texture packs - the old
     "Use texture packs" master toggle is gone (per-pack toggles only; the
     master switch silently disabled the prompt sets).
   - Layout-adaptive labels (Classic/Modern/custom bindings changing the
     controller-page text) are still open: the page's labels are page text,
     not part of the diagram texture, so that needs a runtime-generated
     diagram (compose per (device, layout) at index build or procedurally) -
     design noted, not started.
   - **Font-embedded button symbols solved (2026-10-13).** The tutorial prompts
     (Circle = melee, Square = brake/reverse, ...) are FONT GLYPHS in the
     256x256 T8 UI atlas at tbp0=13592 (key
     `3ece7b57667e958b-f663bcea3bca5023-00002213.png`) - found after the F9
     captures proved every prompt screen samples only that sheet family and
     the replayed captures surfaced the full 256px window. Cell coordinates
     (owner-located, shape-verified): square (0,230), cross (24,230), circle
     (46,230), triangle (212,206), ~22px each.
     `tools/make_font_sheet.py` redraws just those cells per set (button art
     from the AL2009man pack; keyboard = the port's default bindings,
     W/LMB/S/E) and writes the whole atlas under
     `prompts/<set>/<atlas key>.png`; every other pixel is byte-identical so
     the rest of the text is untouched. The v2 note about shared-sheet
     replacement now applies only for extending this (e.g. adding L1/R1 or
     Start/Select symbols in the font if they exist).
   - **GS-scale alpha (fixed 2026-10-13 late) - the "see-through prompts"
     root cause.** Replacement textures are sampled through the GS blend
     emulation, whose factor is `k = alpha/128` (`0x80` = opaque; the dumper
     writes that scale and the DDS converter uses it too). Art authored in
     `0..255` alpha makes `k ~ 2`: the source is over-brightened and the
     destination factor goes negative, so prompt glyphs rendered washed out
     and "showing the game world through" on the HUD (menus hid it; white
     art saturated worst). `prompt_glyphs.to_gs_alpha()` now scales every
     generated replacement (glyph tiles, atlas cells, controller art) to
     `0..128`; `preview_*.png` keep `0..255` for the settings UI and the
     controller preview is its own full-alpha file (`preview_controller.png`,
     referenced from `manifest.txt`). Art is dark again (the approved look):
     Xbox letters in official colours, Switch/Steam white + brand ring,
     DualShock = the pack's Color SVGs (coloured symbols, like the real
     DS4), DualSense = the pack's **white style** (white disc, gray
     `#808080` symbol - the "Style - Transparent" layer group; the SVG's
     three style groups are selected explicitly in `_svg_with_style()`),
     keyboard keycaps; controller portraits are the plain dark art (the
     white halo was removed after the owner test). Verified by replaying
     capture 1661: the cross prompt renders exactly as authored
     (`(28,28,32)` disc, green `A`); DualSense renders as the white button.
     Backlog: the "Black" style (`Style - Outline (Black)`: black disc,
     white symbol) is available as an alternative if a prompt set ever
     needs a dark-button look.
   - The owner has the self-contained contact sheet at
     `texdump/_contact_sheet.html` (open with the full file:// path).

9. **DONE (2026-10-16, owner-confirmed): Xbox One controller connected but
   unresponsive.** Fixed; the original report and leads are kept below for
   reference. **Xbox One controller connected but unresponsive (owner report,
   2026-10-13 late).** The DualSense works; the Xbox One pad is seen by the
   game but no input registers. Leads:
   - The pad selection logs `[pad] using controller <i>: <name> (connected: ...)`
     on every change - check which slot is actually active. The settings'
     `controller=` pin (`ps2SetPreferredGamepad`) wins while that exact name
     is connected, so a still-connected DualSense keeps the Xbox pad ignored.
   - If the Xbox pad IS the active slot and dead: the buttons are likely
     unmapped. The port reads pads through raylib (GLFW + SDL's
     gamecontrollerdb); a joystick that is present but has no mapping reads
     as all-zero. raylib's `SetGamepadMappings` can load an SDL mapping
     string at runtime - we need the device's GUID (raymath doesn't expose
     it; read `/proc/bus/input/devices` or log `glfwGetJoystickGUID`).
   - Also check the transport: Bluetooth Xbox One pads need `xpadneo` (the
     stock `xpad` only handles USB); an unmapped hid-generic node would
     explain "seen but dead".

10. **DONE (2026-10-16, owner-tested): Steam Controller works through Steam.**
    Adding `ps2EntryRunner` to Steam as a non-Steam game and using the Steam
    Controller makes the game recognize it as a Steam controller and show the
    correct prompts. The original goal and leads are kept below for reference.
    Steam Controller (2026, Puck dongle) / Steam Input testing. Goal:
    exercise the Steam prompt set + the Steam device path without the Steam
    client. The owner has the **2026 Steam Controller** (Puck dongle
    `28de:1304`):
    - `benashby/steam-puck-bridge` - the Linux userspace driver for that
      dongle: disables lizard mode and exposes a virtual Xbox 360 gamepad via
      uinput, explicitly without Steam running. Main option for this pad.
    - Name the virtual pad "Steam Controller" so `ps2InputDeviceKind()`
      matches "steam" and Auto picks the Steam prompt set.
    - The older tools target the 2015 pad only - fallback references:
      `rodrigorc/inputmap` (virtual devices, by the hid-steam author) and
      `kozec/sc-controller` (GUI, own config format); the kernel `hid-steam`
      driver is likewise the 2015 driver.
    - Note: real Steam Input (action sets/layouts) still needs the Steam
      client; the bridge just makes the device usable as a normal gamepad,
      which is what the port needs for its Steam prompt set.

11. **Motion blur: a setting to turn the PS2 effect off, and possibly a
    modern replacement (owner ask, 2026-10-15).** The PS2 effect is a
    full-screen smear the owner wants gone, with a better one as a
    possible later extra. Two parts:
    - **Off switch, game side first.** If the effect is gated by a config
      byte or a branch on a settings value, patching that gate is the right
      fix - it removes the cost as well as the look. The port already has
      both mechanisms: the `.pnach`-style write path
      (`ps2ModsApplyPatches`, re-applied every presented frame) and the
      `game_overrides` function hooks. Find the gate by watching which
      variable the blur code reads (RAM dump + F8 diffing, as the radio
      station variable was found).
    - **Off switch, renderer side as the fallback.** The effect is a
      framebuffer feedback - a screen-sized primitive that samples the
      colour buffer and blends it back over itself, probably once per frame
      near presentation - which is also why it is expensive as well as ugly.
      Verify that first: `PS2X_GS_GPU_STATS`/`PS2X_GS_TRACE_FRAME`, or dump
      the draw list for a frame, and look for a full-target quad with the
      frame's own colour buffer as its texture. If it is that, the GPU
      renderer can skip the pass by primitive size/target/texture.
      Expose it in **Display** as `Motion blur: PS2 / Off / Modern` rather
      than an env var, and default to PS2 so nothing changes silently.
    - **Modern replacement (optional, after the off switch).** The port
      already composes the display and keeps the previous frame, so a
      smoother accumulation is cheap there - camera-relative, or per-object
      from transforms the port already has for the vehicle-look work. Opt-in
      only; this is a taste change, not a fix.

    **Findings (2026-10-15).** The off switch is implemented; the guess above
    was wrong in a way worth recording. The effect is *not* one
    framebuffer-feedback quad. It is a **multi-pass ping-pong accumulation**:
    the game accumulates previous frames into two 256x256 buffers (adjacent in
    VRAM, TEX0 base **13440** and **14464**) in a ~9-pass cycle per frame, then
    blends the result over the display (fbp=140). Each pass lerps toward its
    source by destination-alpha/128, which is the trails decay.
    - Consequence for the fix: skipping only the accumulation leaves the
      buffers stale and shows a **frozen ghost**; skipping only the display
      blend keeps the cost. Both have to go.
    - Every pass - the textured ones and the texture-less ones, which still
      carry the buffer in TEX0 - names one of the two buffers as its texture
      base, so a single test in `flushBatch` drops the whole effect. Measured
      on `scratch/gs_capture.bin`: draws 4,969 -> 4,537 (~8 a frame), resolves
      8,349 -> 7,716, loads 4,965 -> 4,461, i.e. **~8.5% of the GPU passes as
      well as the look**. The scene renders straight into the display buffer
      and the blur is merely blended over it, so skipping leaves the scene
      sharp rather than frozen.
    - Shipped as **Display > Motion blur: PS2 / Off**, default PS2.
      `PS2X_MOTIONBLUR_OFF=1` forces it off without the menu;
      `PS2X_MOTIONBLUR_BUFS=a,b` overrides the buffer addresses;
      `PS2X_GS_TRACE_FULLSCREEN=1` logs every draw covering most of its target,
      which is how this was identified.
    - **Still open:** the two buffer addresses come from one capture. A level
      that allocates VRAM differently would need them re-derived (the trace
      above is the tool), or the rule made structural. A *game-side* gate
      would be cleaner still: the blur strength is the destination alpha the
      game writes, so if the game keeps that in a variable, zeroing it removes
      the effect at the source - findable with the RAM-dump/F8 diffing used for
      the radio station variable. Not needed now that the renderer-side switch
      works, but it would survive VRAM layout changes.
    - Not yet verified by eye in gameplay (the owner should confirm the
      picture looks right and the blur is gone). `tools/gs_replay` cannot check
      it: its presented-frame dump returns nothing, and the vramHash is
      *expected* to change when the blur stops writing VRAM.

12. **VSync + VRR - DONE (2026-10-15).** Tearing on both platforms came from
    `SetTargetFPS(60)`: a software cap, not a sync, so the buffer swap never
    waited for the display. **Display > VSync** now sets the swap interval
    (`FLAG_VSYNC_HINT`), default on. That is also all a VRR display needs - the
    driver and panel handle the rest - but note the emulation is fixed at 60
    VBlanks/s, so a VRR panel will sit at ~60 Hz until frame interpolation
    exists. The 60 fps cap is kept deliberately: with vsync on it is redundant,
    and removing it would present duplicate frames faster than the emulation
    produces them.

13. **A modern motion blur (owner ask, 2026-10-15).** Effort depends entirely
    on which kind, and it is worth being precise because the cheap version and
    the good version are different projects:
    - **Camera-relative reprojection - moderate (~a day).** The port already
      composes the display and owns the previous frame, and it already has the
      camera matrix (from the vehicle-look work). Warp the previous frame by
      the camera delta and blend: cheap, plausible, and it fixes the *look*.
      Limits: only camera motion is known, so moving objects do not smear
      correctly, and disocclusion (turning to reveal new geometry) smears.
    - **Per-object - hard, and fragile.** Needs per-object motion vectors,
      which the GS does not provide. They would have to be synthesised from the
      transform stream the port already sees, per drawn primitive. That is real
      work with a long tail of edge cases (skinning, particles, billboards).
    - Recommendation: do the reprojection version first as an opt-in
      `Motion blur: Modern`, and only consider per-object if it proves too
      smeary to be worth having.

    **Progress (2026-10-15):** the camera offsets are **confirmed against a real
    camera**, and the first attempt at them was wrong - which is exactly what
    the log was for. Published state is `g_ps2Camera` in
    `runtime/ps2_camera.h`: `front`/`up`/`pos` read from **TheCamera.Cams[0]**
    at the fixed address `0x6F4540` (Front +0x10, Source +0x20, Up +0x60), the
    same camera the vehicle mouse look rotates. The log showed `front` and `up`
    unit length and mutually perpendicular, `pos` world-scale (~1700) and moving
    with the camera, `front` swinging on turns, `up` tilting on look up/down,
    and the view window tracking the game's own swappable FOVs
    (tan(35)/tan(22.5)/tan(60) all appear).

    Two things that first attempt got wrong, both caught by the log:
    - `+0x78/+0x7C` off the *RenderWare* camera are not alpha/beta. They read
      2000/1000 - a mode/state value - and the look code only uses them as a
      *change detector*, which is a different job from an orientation.
    - `+0x20` there is not a world position (|pos| ~ 1).

    The view window comes from `RwCameraSetViewWindow`, but **that hook only
    runs when the game sets the window**, so a camera published only from there
    goes stale in between (the log showed publishes at frames 1554, 1581, 1582,
    1633 - not consecutive). `publishCameraFrame` is therefore called from the
    present path in `PS2Runtime::run()`, once per presented frame: the CCam is
    updated every frame, so that gives one fresh sample per picture, and the
    view window is remembered from the last request. Verified locally: the intro
    (no camera at all) now logs *consecutive* frames with `valid=0`, so the
    plumbing is right even where there is nothing to reproject.

    `valid` requires a unit `front`/`up`, a position off the world origin, and a
    real view window - before the game builds a camera the CCam is all zeros
    (a unit -Z front, zero position), which is exactly the state the intro
    presents.

    **`PS2X_CAMERA_LOG=1`** writes cameralog.txt next to the executable (the
    Linux launcher sets it; `windows/cameralog.cmd` does the same on Windows).

    **Progress (2026-10-15): a "Modern" mode was built and then withdrawn.** It
    is not in the menu; the commits are `f491ff4` (first version) and `2bba4be`
    (rework), and the camera work it was built on is still in the tree. The
    reasoning is worth keeping, because most of it applies to item 14 as well.

    **What the owner said, and what was actually wrong.** The first version
    blended the previous frame into the current one - a *temporal* accumulation.
    That reads as a sharp double image (ghosting), not a blur, and the trail
    compounds frame over frame. The rework made it *spatial* instead: the
    composed frame is smeared along the camera's own per-pixel motion, six taps
    from `uv` back along `uv - warp(uv)`, no history at all. That is stable and
    is what motion blur actually is. The owner's verdict on the rework: "the blur
    itself looks better, it still doesn't look great" - and the HUD and minimap
    were still being smeared.

    **The blocker is the 3D/overlay split, not the blur.** The HUD and minimap
    are drawn in screen space, so they do not move with the camera and must not
    be warped. The attempt here was a heuristic: the first draw that stops
    testing depth after one that did marks the end of the 3D scene, and the
    target is copied aside there; the compose compares the finished frame against
    that snapshot and puts the answer in the alpha channel. It never fired on the
    intro (nothing to find there) and **was never confirmed on real gameplay**,
    which is exactly the weakness - `PS2X_BLUR_MASK_DEBUG=1` existed to check it
    by eye, and the owner's report says it was still wrong.

    The honest read: a heuristic on the GS draw stream is the wrong tool. The
    right signal is on the game side - a hook on the HUD's own draw, or on the
    function where the 3D render ends. That needs a *capture* to find, not a
    guess: `F9` in gameplay writes a GS capture, and the HUD's draws can be read
    out of it (the port already skips the game's blur buffers by TBP, so
    identifying draws by their state has precedent).

    **What is still good and still in the tree:**
    - `runtime/ps2_camera.h` + `src/lib/ps2_camera.cpp`: the camera published
      once per presented frame, from `TheCamera.Cams[0]` at `0x6F4540`
      (`Front +0x10`, `Source +0x20`, `Up +0x60`). The offsets were **confirmed
      against a real camera** with `PS2X_CAMERA_LOG=1`: unit `front`/`up`,
      mutually perpendicular, world-scale `pos` (~1700), swinging on turns.
      (The first attempt read `+0x78/+0x7C` off the RenderWare camera, which is a
      change detector reading 2000, not an angle; the log caught it.)
    - `ps2CameraHomography`: the camera-to-camera homography, checked numerically
      against hand-computed cases (identity, yaw, forward translation at two
      depths, pitch for both vertical signs). Applying it to the owner's real
      cameralog gives an *exact* identity for all 34 static camera pairs, with
      the game's FOV ramp showing up as a ~0.0002 scale.
    - The screen convention, which is the thing most likely to be silently
      wrong: the draw shader maps GS y straight to GL clip y with no flip, so
      `gl_FragCoord.y` increases *down* the picture - the opposite of the
      camera's y-up basis, so the intrinsics carry the sign. Confirmed against
      the port's own readback path.

    **Two limits that are inherent, not bugs**, and that item 14 inherits:
    only camera motion is known, so objects moving through a still camera do not
    smear (and would not interpolate); and the GS receives vertices the game
    already projected, so there is no usable depth buffer - translation is
    reprojected against a single depth plane (`PS2X_BLUR_DEPTH`, default 100
    world units ~ 10 m), which only scales the translation part.

    **Parked (2026-10-15):** Modern is out of the settings menu; only
    `PS2X_MODERN_BLUR=1` turns it on. It is parked because of the mask, not the
    blur: the HUD is blurred outright on frames where the mask is missing.

    **Why the mask is intermittent - measured, and the thing to fix next.**
    The display target's mask is empty on some frames and populated on others.
    Two findings from dumping every target's mask across consecutive frames:

    - The copies that carry the mask to the display happen on only *some* frames
      (blocks 0, 12288, 13440, 14464 each appear on a handful of the seven frames
      examined). Propagating through them therefore cannot be the whole answer:
      on a frame with no copy, the display's mask is stale or absent.
    - The game draws the HUD into the *scene* framebuffer (block 4480), and the
      scene's own mask is the complete one - it holds the HUD directly and the
      minimap through the composite that draws it in. The compose reads the
      display, so it reads the wrong target.

    So the fix is to have the compose read the mask from the target the overlays
    were actually drawn into (the widest such target, which is the scene - the
    minimap's own target is only 128 wide), and to make the per-frame clear
    deterministic rather than lazy. Both were attempted and are *not* in the
    tree: the measurement loop needed the mask dump to be frame-numbered, and the
    trace and the dump turned out to be off by one frame from each other, so the
    results could not be trusted. Start by making those two agree.

    A caution for whoever picks this up: several of the earlier "verified" claims
    in this item were taken from a replay that was silently not applying the
    renderer settings (`g_gsModernBlur` stayed false), and one live check was
    defeated by the owner's saved `motion_blur_strength=0.05`, which is a
    sub-pixel smear and reads as "the blur does nothing". Check the one-shot
    `[gs] motion blur:` line says what you expect before believing a comparison.

    Next: the same motion vectors are what item 14 needs, so the overlay split
    has to be solved once, properly, before either is worth shipping.

    **Owner's note (2026-10-15): per-object motion blur is a task for later.**
    The camera-only blur cannot know that a car or a ped is moving, so anything
    crossing a still camera stays sharp, and anything moving while the camera
    moves is smeared by the camera's motion rather than its own. Per-object would
    need motion vectors per object, which the GS stream does not carry: the port
    would have to derive them from the game's own transform data (the same way
    the camera is read from `TheCamera.Cams[0]`), or from a depth+reprojection
    pair across frames. Worth revisiting once item 14 is settled, since frame
    interpolation wants the same per-object motion.

    **Progress (2026-10-15): the overlay split is solved and verified.** A
    gameplay capture (F9, `gs_capture_<frame>.bin` in the working directory)
    replayed through `tools/gs_replay` gave the answer, which is *measured* now
    rather than guessed:

    - **The signal is the perspective divisor.** Every world draw has STQ
      `q` between 0.006 and 0.11; every screen-space overlay draw has `q`
      **exactly 1.0**. That is what a 2D sprite is - no perspective - and it
      separates the two sets cleanly across a whole frame.
    - **A full-frame pass is also `q == 1`** (the game's own blur display blend,
      the display copies), and those are the frame itself, not an overlay. Width
      tells them apart: the frame is full width, and the widest overlay draw
      measured is 557 of 640.
    - `PS2X_GS_TRACE_DRAWS=<presentIndex>` prints one frame's draws in order with
      the texture, depth state, bounds, `q` range and Z range - that is how the
      above was measured.

    The renderer now builds the mask: `flushBatch` marks every primitive that is
    `q == 1` and not full width into a per-target R8 texture (one extra draw per
    overlay batch, created lazily, cleared once per frame). The compose can show
    it with `PS2X_OVERLAY_MASK_DEBUG=1`, and `PS2X_OVERLAY_MASK_DUMP=<path>`
    writes it out as a PGM. **Verified against the capture**: the mask is exactly
    the HUD - clock, weapon icon, health and armour bars, minimap, subtitle - with
    the world untouched, 2.1% of the frame. Default vs mask-on is byte-identical
    over 31 frames, so it costs nothing until something reads it.

    **The catch, and it is why the blur smeared the HUD.** The mask lands on the
    *scene* target (`fbp 140`), but by compose time the frame has been **copied
    through other buffers to the display target** - and those copies are
    full-screen `q == 1` quads themselves, so the display's own mask is useless.
    The compose reads the display, so it cannot see the mask yet. Two ways
    forward, both now grounded: propagate the mask along with the pixels through
    the copies, or apply the blur on the scene target at the display copy. This
    is also the answer to "why did the HUD smear" - it is baked into the pixels
    before the port ever composes.

    **A second correction, from the same reading.** `key.zte` is `(test >> 16) &
    1`, the ZTE *enable*, while `(test >> 17) & 3` is the ZTST *mode* (0=NEVER,
    1=ALWAYS, 2=GEQUAL, 3=GREATER). The HUD draws with ZTE=1 and ZTST=ALWAYS, so
    a ZTE-based test counts them as depth-tested and never fires - which is
    exactly what the withdrawn exclusion did. `q` is the reliable signal.

14. **Frame rates above 60 (owner ask).** The emulation is locked to 60
    VBlanks/s - the game's logic runs per VBlank - so anything above 60 must be
    *interpolated*, not emulated. Three shapes, cheapest first:

    **Step 1 DONE (2026-10-15): the host loop runs at 120.** `SetTargetFPS(120)`
    in `ps2_runtime.cpp`. The emulation is unaffected and that is verified, not
    assumed: the game runs on its own thread (`gameThread` -> `m_eeScheduler->run()`)
    paced by the VBlank clock, and the host loop is only the *present* loop.

    **A false alarm worth recording.** The first attempt was reported as "host and
    game both 120", which reads as the emulation doubling. It was not: the "game
    fps" readout counted `g_ps2PresentedFrameCounter`, which advances once per
    *host* frame - at 60 Hz the two coincided, so it had always looked right. The
    owner's own eyes were the correct instrument ("the game doesn't appear sped
    up"), and the perf log's `fps=`, which is derived from VBlank ticks, had been
    reading 60 all along. The readout now uses the VBlank clock, and the camera
    publish is gated the same way, once per *picture* rather than per host frame -
    the reprojection deltas are between pictures, and publishing twice per picture
    would halve them.

    Verified headless with VSync forced off (a real display would block the swap
    at its own rate, so this is the only way to reach the cap): host 120 fps
    (8.33 ms/frame) while the game read 60 by both measures. `PS2X_PERF_LOG` gets
    `[host] fps= frameMs= gameFps=` every 5 s; its own `fps=` field is the game
    rate from VBlank ticks, so the two can be cross-checked. Do not confuse them -
    this note's author did, twice.

    **Step 2 (in progress): the synthesis, renderer side DONE and verified.**
    The renderer now tracks the camera on every compose (not just when the blur is
    on - the interpolation deltas need it), remembers the texture the last compose
    produced, and has `synthesizeHostFrame()`: it warps that frame forward by half
    the last camera motion (the homography extrapolated as `1.5*I - 0.5*H`,
    normalised by H's bottom-right entry first) into its own texture, taking the
    source as-is wherever the overlay mask says overlay.

    Verified from a capture with the synthetic camera: the world is displaced by
    the half-step while the clock, bars, weapon icon, minimap and subtitle are
    unmoved. `PS2X_FRAME_INTERP_TEST=1` plus `PS2X_FRAME_INTERP_DUMP=<prefix>`
    writes the source and the synthesized frame out as PPMs for that check. With
    no env set the renderer is byte-identical over 61 frames, so it is inert.

    **Step 2 DONE (2026-10-15): the host wiring is in.** `SynthesizeFrame` is a
    virtual on `GSRasterBackend` (default: no-op, so the CPU backend is
    unaffected); `GSGpuBackend` implements it as a *blocking* round trip to the
    render thread - the same `std::promise` shape `ReadVram` uses, because unlike
    `Present` the caller has to have the texture before it draws. `GS::synthesizeFrame`
    exposes it, and `ps2_runtime.cpp` calls it on the frames the game produced no
    new picture (the camera-publish gate already knows, since it keys off the
    VBlank tick), wrapping the returned GL id as a raylib Texture2D exactly as the
    shared present texture is.

    Verified: with the synthetic camera and VSync forced off, a live run logs
    `[gs] frame interp: first synthesized frame 2560x960 (host frame, game
    produced none)` - so the whole chain runs. With no env set the renderer is
    byte-identical over 61 frames.

    **Frame rate setting (2026-10-15, owner ask).** Display options has
    **60 (native) / 120 / 180 / 240 (interpolated)**. The rate is the setting that
    implies the interpolation; `PS2X_FRAME_INTERP=1` still forces it on for
    testing and `PS2X_FRAME_RATE` sets the rate from a launcher.

    The synthesis takes a phase: the warp is `(1 + t)*p - t*H*p`, so the frame
    shown is the fraction `t` of the way from the last picture toward the next
    rather than always the midpoint. The host works out `t` from the host frames
    since the last picture over `rate/60`, and drives `SetTargetFPS` from the same
    setting. Verified with VSync forced off: rate 240 gives host 240 fps
    (4.17 ms/frame) with the game still at 60, and the first synthesized frame
    logs phase 0.25.

    **A bug this exposed, and it was the 120 cap.** The owner reported the mouse
    camera pulling behind the vehicle even sitting still, where it should move
    freely and only recentre once moving. The look offset is applied only while
    `g_ps2PresentedFrameCounter` is within one of the vehicle-camera update frame -
    and that counter advanced once per *host* frame, so at 120 the gap was two,
    the gate failed, the offset stopped being applied, and the game's own
    recentring won. The counter now counts *pictures*: it advances where a new
    picture is detected, not in the host present block. Everything that gates on
    it is picture-based again, which is what they all assumed; the host frame count
    is kept separately for the dumps and exit gates. Worth remembering as a class
    of bug: anything keyed on "presented frames" quietly changes meaning when the
    host loop runs faster than the game.

    `linux/play-interp.sh` and `windows/play-interp.cmd` set the rate.

    - **Reprojection (as in item 13).** Generate intermediate display frames by
      warping the previous composed frame along the camera delta. Reuses the
      same warp pass, so items 13 and 14 are one piece of machinery. Quality:
      smooth, with the same camera-only limits - expect edge smear and
      disocclusion artefacts, so it wants to be opt-in and per-frame tunable.
    - **Render the scene twice per emulated frame** - not possible without the
      game's cooperation: the GS stream is game-driven, so there is no
      intermediate state to render.
    - **Optical-flow / model-based frame generation (DLSS-FG style)** - needs
      motion vectors plus a model. Out of scope.
    - This is also what makes VRR above 60 worth having: with reprojection the
      display can run at 120/144 and VRR will follow the *generated* rate.
    - The optimisation work matters here: reprojection is extra GPU work per
      displayed frame, so it is only affordable because the renderer's pass
      count and the game thread came down (state/uniform caches, VU0 compiled,
      the VBlank catch-up fix). Worth re-checking the headroom before starting.

      **The black flicker, and what it actually was (2026-10-15).** Every
      interpolated frame went black on screen, worse the higher the rate (more
      synthesized frames), while the renderer's own readback said its output was
      a faithful warp of the source. Five fixes were tried and none was the
      cause: a deeper ring of synthesized textures, a `glFinish` after the
      synthesis, enabling the overlay mask for interpolation, deriving the phase
      from time, and pinning the phase. `glFinish` changing nothing was the clue
      that it was not a timing or visibility race at all.

      The cause was a *context* mistake. A synthesized frame is handed to the
      host as a GL texture and drawn there, not read back - and that only works
      if the renderer's context is in the window's share group. It never was.
      `GS`'s constructor calls `reset()`, which called `m_backend->Flush()`,
      which reached `execute()` and constructed the `Renderer` - whose
      constructor creates the GL context. `GS` is a `PS2Runtime` member, so that
      all happened at construction time, before `InitWindow()` had even run.
      `GLContext::create` only tries the GLX share when `g_hostSharedGlContext`
      is already published, so it silently fell back to an EGL surfaceless
      context of its own (`[own context]` in the log) - and the host then drew a
      texture *name* that means something else in its own context. Black.

      Two changes fix it, both about ordering rather than GL: `GS::reset()` no
      longer touches the backend until the GS is initialised
      (`m_localMemoryStorage != nullptr`), and the renderer is created on the
      first command instead of when the render thread starts.
      `syncCoreSubsystems()` - which is what actually initialises the GS - then
      moved after `publishHostGlContext()` in `PS2Runtime::initialize()`.

      Verified on the same machine: the log now reads
      `[shares the window's context]`, the present path is `shared texture`, and
      the brightness the host actually puts on screen is 14.28 (synthesized) vs
      13.90 (picture) - it was 4.04 vs 13.3. `PS2X_HOST_FRAMECHECK=1` measures
      exactly that, averaged separately for the two kinds of frame; a readback
      from the renderer's side cannot see this class of bug at all, because the
      renderer's own context is always right.

      **A side effect worth knowing:** sharing working also switches the present
      path to the shared texture (no readback, no re-upload) whenever the
      internal scale is above 1, which it always was on Windows. That path had
      never actually run - sharing never worked - so it is worth a look at the
      edges of the image. `PS2X_GS_PRESENT_SHARE=0` (`play-readback.sh`) forces
      the old readback path for an A/B; measured here the two are visually
      identical (0.56/255 mean difference, filtering only).


      **The warp/jitter - cause and fix (Claude, 2026-10-17; code landed in
      `aa6ede8`). Awaiting the owner's visual check.** The interpolation
      assumed one game frame per VBlank. `PS2X_INTERP_LOG` with a scripted
      turn (`PS2X_MOUSE_FAKE`) shows otherwise: a picture is composed every
      VBlank, but the camera changes only every 2-4 VBlanks. VCS draws a new
      view at 15-30 fps. So 2-3 of every 4 pairs had identical cameras (no
      motion, a hold), and the next pair carried the whole multi-VBlank step,
      swept inside one VBlank (t=0 then ~0.5, then the new picture). That is
      the jitter. Warping the old image across that large step in a single
      frame, with the translation modelled at one depth, is the visible
      warping.

      Two clocks made it worse. The phase came from the host noticing the
      VBlank, while the pair changes when the render thread composes, so a
      reset could show the old pair at t=0: a step back, then forward. And
      the camera was published by the host loop, so a camera could arrive a
      compose early or late relative to its picture.

      The fix:
      1. **Each picture carries its camera.** `GSPresentationRequest::camera`
         is sampled on the game thread at the VBlank latch
         (`g_gsCameraSampler` → `ps2_game_overrides::readCamera`).
      2. **The pair shifts only on a new game frame** (the picture's camera
         changed; `Renderer::trackCamera(request)`). The source picture is
         copied then and held across the repeats.
      3. **The renderer owns the phase:** `(now − pair time) / measured
         game-frame interval`, on its own clock (running average, clamped
         1/60–1/10 s). The host passes −1 unless `PS2X_INTERP_PHASE_FIXED`
         is set.
      4. **Once the sweep reaches 1, or the camera is still,** the synthesis
         returns 0 and the host draws the newest picture as is (current
         NPCs and cars, no warp).

      Verified with the same scripted turn at 120: `t` now climbs 0.19 →
      0.43 → 0.69 → 0.93 → next frame, evenly across each game frame, with
      no resets mid-frame. A still camera shows plain pictures (`synth=0
      t=1`). At 60 the synthesis never runs. `PS2X_INTERP_LOG` now also
      prints the renderer's phase as `t=`. Still possible after this: the
      parallax approximation (`PS2X_BLUR_DEPTH`) on fast sideways movement
      near walls - camera-only interpolation can't fix that, but the steps
      are now small.

      **Status 2026-10-17: the warp is fixed. What remains is frame pacing, and that
      is why it is parked.** The two-way blend with the static-pixel rule (`cc5abea`)
      and pairing each picture with its own camera (`7e5bf2d`) removed it, and the
      owner confirms the picture is right now. But at 120-240 on the owner's machine
      the frame times are still not steady and the result tears, so `d2df272` took the
      option out of the menu and made saved rates above 60 be ignored;
      `PS2X_FRAME_RATE=<hz>` still turns it on for testing. Related: `c16e562` had
      VSync do the pacing and took the blocking round trip out of the synthesis, which
      made host frame times even but did not make them *steady* at 4x on that machine.
      **A reviewer should read the pacing, not the homography** - the homography is
      verified exact against the true camera mapping, and the visible defect is now
      uneven frame delivery rather than a distorted picture.

      **Interpolation cannot work on Windows as it is built, whatever the pacing
      says.** The synthesized frame is handed to the host as a *texture* - the
      renderer draws it into `m_synthTexture` and the host draws that, it is never
      read back (the note in `ps2_runtime.cpp` says so) - so it needs the two GL
      contexts to share. On Windows the driver refuses to share the window's context
      at all, and after the fix above the renderer honestly reports `[own context]`,
      so a host frame asking for a synthesized picture would draw a texture name from
      a context that cannot see it: exactly the corrupt-frame case that the context
      sharing fixed on Linux. So the feature is *doubly* unreachable there - it wants
      a readback variant of the handover as well as the pacing work - and a Windows
      player who sets `PS2X_FRAME_RATE` should expect a broken picture, not an
      interpolated one.

      **Where Claude left it (2026-10-17, commits `7e5bf2d`, `cc5abea`,
      `c16e562`, `d2df272`).** Measured facts, so nobody re-derives them:
      - **VCS draws a new view every 2-4 VBlanks** (15-30 fps), while a picture
        is composed every VBlank. The interpolation must work in game frames.
        `PS2X_INTERP_SEQ=1` shows it per compose: `p` = picture changed, `c` =
        its camera arrived, `B` = both.
      - **The camera that comes with a picture can be a compose early or
        late.** Cameras are now stamped into the VIF1 stream order
        (`ps2CameraStreamPush` after the camera update, looked up by the
        stream position of the latest scene-buffer draw). The renderer pairs
        by *picture change* (the composed source's `drawSeq`) and waits at
        most one compose for the camera.
      - **The overlay mask covers almost none of VCS's HUD** (a dot and some
        text boxes), and `m_overlayMaskOriginX/Y` were never assigned. What
        keeps the HUD and the orbited player still is the static-pixel rule in
        the synthesis shader (a pixel equal in place in both pictures, with
        its four neighbours, is shown unwarped).
      - **Depth 15**: the camera orbits the player at close range.
        `PS2X_INTERP_ALIGN=1` measured the warp removing ~25% of the frame
        difference at depth 8-20 vs ~10% at 200. The FOV scale sweep in the
        same diagnostic puts the best fit at 1.0 and the reversed direction
        2-3x worse, so the projection and direction are right.
      - **Pacing**: VSync now paces alone when interpolating, and the host
        requests frames without blocking (`RequestSynthesis` /
        `LatestSynthesis`). `PS2X_HOST_TIMING=1` with the owner's settings
        (4x, VSync, 180 Hz display): 5.56 ms frames, p99 <= 7 ms through a
        scene that used to spike to 30 ms.
      - **A real interpolated frame looks right:**
        `PS2X_INTERP_ALIGN=1 PS2X_INTERP_DUMP_PAIR=<prefix>` writes
        prev/mid/next PPMs and the two mask PGMs. After `cc5abea` the HUD and
        the player are crisp and single and the world sits between the two
        pictures.
      - **Still unexplained:** the owner still saw judder and tearing after
        `c16e562`, and none of the measurements above reproduces it. Not
        covered yet: driving (an object that moves on screen), the static-pixel
        rule on flat surfaces (seams), the async path showing each result one
        host frame after it was requested for, and motion blur on top. The next
        useful instrument is a recording of the *screen* at the display rate
        (OBS at 180), or an in-memory ring of host frames written out
        afterwards (PNG writes during capture slow the host 6x, so
        `PS2X_HOST_CAPTURE` is not representative).

      **Attribution:** `aa6ede8` ("handoff: bring it up to date...") also
      contains Claude's first interpolation change (game frames, renderer-owned
      phase). OpenCode committed it from the shared working tree. It is
      published (in `vcs-bringup`, `release` and the `stories-v0.1.1` tag), so
      history was not rewritten; this note is the record.

      **(Previous notes, kept for the record.) PARKED 2026-10-16: the
      interpolated picture warps. FOR THE NEXT PAIR OF EYES.** Everything else about interpolation works - no black frames, the
      HUD is protected, cutscenes and FMVs play, the picture stream is correct -
      but with a moving camera the whole picture visibly warps against the
      motion. Six real bugs were found and fixed on the way here, all worth
      knowing because each one alone produced the same symptom:

      1. **Context sharing** - the renderer's GL context was created before the
         window existed, so it never joined the window's share group; a
         synthesized frame is handed over as a texture, so the host drew a name
         that meant something else. That was the original black flicker.
         `GLContext::create` only tries the GLX share when the host context is
         already published, and `GS::reset()` from the GS *constructor* brought
         the renderer up before `InitWindow()`.
      2. **Size** - the synthesized frame was handed over at the composed size
         while the picture is handed over at the readback size, so the two kinds
         of frame were resampled differently every other frame.
      3. **Mask fetch** - the overlay mask texture is the scene target's full
         extent (taller than the used region); the compose fetches it by texel
         offset, the synth fetched it by normalized uv, so the HUD was not
         protected and a phantom frozen band sat mid-screen.
      4. **Cutscenes** - a cutscene's GS input arrives only a couple of times a
         second, so the pair of composed frames stops advancing and the synthesis
         warps a stale pair forever. Interpolation is now skipped while
         `ps2_game_overrides::isInCutscene()` is true.
      5. **The source copy** - `glBlitNamedFramebuffer` is affected by the
         scissor test, and the game leaves its last scissor set from the draw
         that ended the frame. The copy of the outgoing picture into the
         interpolation source covered only that rect, leaving the source mostly
         black - the "frozen black frame from before the fade-in".
      6. **Convention and direction** - `ps2_camera.h` says outright that its K
         matrices use the picture's **v-down** convention while `gl_FragCoord`
         is v-up, and the shader samples the source at `q` for each *output*
         pixel, so the homography must map **output(mid) -> source(prev)**. Both
         were wrong: the warp was vertically mirrored, and it was the inverse of
         the mapping needed. A standalone numeric check confirms the helper now
         gives the identity at t=0 and exactly `ps2CameraHomography(prev, cur)`
         at t=1.

      **The warp survived all six fixes, so it is something else again.** What is
      *known*:

      - The homography math is exact and its direction is now verified.
      - The warp is a no-op when the camera is still (which is why every
        measurement taken standing still looked right).
      - It scales with motion; the owner describes it as the whole screen warping
        against the movement, worst on mouse turns.
      - The display runs one picture behind by design (that is what
        interpolation costs). If the remaining complaint is the *lag* rather
        than a distortion, that is a trade-off, not a bug - worth asking.

      **Next things to try, cheapest first:**
      - `PS2X_INTERP_PHASE_FIXED=0.5` pins the phase: if the warp stops, it is
        the phase sweep, not the warp.
      - `PS2X_FRAME_INTERP_OFF=1` keeps the host rate and drops only the
        synthesis - the reference for "does it need the synthesis at all".
      - `PS2X_BLUR_DEPTH` sets the depth the homography's translation term uses
        (default 200). The camera's *translation* is modelled at a single depth,
        so the parallax is wrong everywhere else - small per frame, but it is the
        one knowingly approximate part of the math, and worth an A/B.
      - `PS2X_SYNTH_CHECK=1` has the renderer report the brightness of the
        source and of the synthesized frame (the three outcomes and what they
        mean are in the code).

      **Diagnostics that exist** (all env-gated, all inert by default):
      `PS2X_INTERP_LOG=1` - one line per host frame with picture/synth, phase,
      drawn texture and size, a `composed=` counter and `camValid=` plus the
      camera; `PS2X_INTERP_IN_CUTSCENE=1` bypasses the cutscene gate so the
      synthesis can be exercised at the boot screen; `PS2X_SYNTH_WHY=1` says
      which guard refused; `PS2X_SYNTH_MASK_DEBUG=1` the mask state;
      `PS2X_HOST_FRAMECHECK=1` reads the presented frame back and reports synth
      vs picture brightness; `PS2X_FRAME_INTERP_TEST=1` +
      `PS2X_FRAME_INTERP_DUMP=<prefix>` + `PS2X_BLUR_FAKE_CAMERA=<deg>` dump a
      source/synth PPM pair from a capture (the replay is deterministic with an
      empty `PS2X_MODS_DIR`). `/tmp/opencode/dirtest/main.cpp` is a standalone
      numeric check of the interpolation homography against the true camera
      mapping - extend it rather than trusting a render.

      The synthesis is `Renderer::synthesizeHostFrame` +
      `interpolateCameraHomography` (`gs_gpu_renderer.cpp`), the pair is kept in
      `upscalePresentation`'s "frame about to be overwritten" block, and the host
      side is the interp block in `ps2_runtime.cpp`.

      **Cleanup and perf (2026-10-16).** The diagnostics from the resolved hunts
      are gone (`PS2X_PRESENT_SIZE_LOG`, `PS2X_SYNTH_FINISH`,
      `PS2X_SYNTH_MASK_DEBUG`, `PS2X_INTERP_NO_MASK`, the per-frame `[cam]` log,
      the host PPM dump), and the `[pad] reads=` line is now behind
      `PS2X_PAD_DEBUG` instead of printing always (the boot log went from
      thousands of lines to ~650). Host frame times with the synthesis on and off
      are identical on this machine (60/16.67 ms at 60, 180/5.56 at 180,
      240/4.17 at 240) and the emulation holds 60 throughout, but the boot scene
      is too light to show the synthesis cost - a gameplay check on the owner's
      machine is still the one that counts.

15. **Other versions of the game - PAL, and other regional builds (owner ask,
    2026-10-18: "how much work would it be to support other versions of the game?
    like pal.").** Parked deliberately: not started, and the owner said not to
    worry about it for now. What follows is the survey, so whoever picks it up
    starts from evidence rather than from the question again.

    **Nothing is measurable here yet** - there is no PAL disc on this machine
    (only the NTSC-U VCS and LCS), so the layout question below is open. Today a
    PAL disc is refused at the boot-hash check, so it cannot even be tried.

    **What is already version-agnostic**, which is most of it:

    - the runtime itself - graphics, VU1, memory card, pads, audio - holds no game
      addresses at all (the ones in `ps2_runtime.cpp` are the guest heap base and
      COP0 constants, not game-specific);
    - the override registry matches a game by **ELF filename + entry point (+
      optional CRC32)**: `PS2_REGISTER_GAME_OVERRIDE("… (SLUS_215.90)",
      "SLUS_215.90", 0x001DD6A0u, 0u, …)`. A second version is another descriptor,
      which is what the mechanism is for;
    - `games/<id>/game.toml` already carries `region`, `boot_file` and
      `boot_hash_xxh3_128`, and `[runtime] override` names the registration;
    - the VU1 program set is learned from the disc at build time, so it needs
      nothing per version;
    - the disc supplies every byte of game data, so a PAL build ships nothing new.

    **What is version-specific, counted (all of it in
    `ps2xRuntime/src/lib/game_overrides.cpp` unless said otherwise):**

    | what | count | notes |
    |---|---|---|
    | hook code addresses | 16 | camera process, follow-ped cam, follow-car cam, after-cam-process, frame update, set view window, set far clip, line of sight, radio entity, radio update, movie finished, movie push-end-code, GIF add-GS-AD, GIF close-GIF-tag, `sceCdSearchFile`, `sceMpegIsEnd` |
    | game data addresses | 13 | camera struct, view window, cutscene flag `0x6F4D14`, pause flag `0x4CD2AC`, radio/station tables, the settings bytes near `0x4E47xx` |
    | struct offsets the hooks read | ~a dozen | camera and ped fields; usually identical if the build is the same engine revision |
    | `config/vcs_clean.csv` | 10,224 functions | **regenerated**: a Ghidra auto-analysis of the PAL ELF, then `games/vcs/tools/rebuild_function_map.py` |
    | `config/vcs_clean.toml` stub bindings | 351 `name@addr` | **regenerated** by ElfAnalyzer |
    | prompt art / font table | 1 address | what `games/vcs/tools/make_prompt_packs.py` reads the game's font from |

    **The cost turns on one question: is PAL's `.text` the same code at the same
    addresses?** Regional builds of the same revision are often identical code with
    different data, and the localisation lives in the data segment - in which case
    most of those 29 addresses carry over and the work is verification rather than
    discovery. If it is a genuinely different build, each of the 16 hooks has to be
    re-found by matching it against the *named* NTSC-U function in the new map -
    which is what `games/vcs/tools/` (`mips_dis.py`, `find_refs.py`,
    `jal_callers.py`) exists for. **So: about a week if the code matches, two to
    three weeks if it does not**, for one regional variant, plus the same again per
    further revision (v2.0, a different pressing).

    **An hour settles which case it is**, given the PAL ELF: compare the entry
    point, the `.text` size, and whether those 16 addresses still point at
    recognisable code. Do that before planning anything.

    **Two things beyond the addresses.** PAL runs at **50 Hz** and the port presents
    at 60, with the audio clock following the game clock (that work is done for
    NTSC-U) - a PAL build's frame-rate-dependent logic needs the same treatment.
    And PAL's internal resolution differs (512x512 against 512x448); the port
    already overrides the game's view window and far clip through the
    `set view window` / `set far clip` hooks, so those need PAL values.

    **The cheap enabling step, worth doing first (~an hour).** Let the builder
    **warn instead of refuse** when the disc's boot file does not match the
    manifest - "this is not the disc this source was built for; building anyway".
    Without it a PAL disc cannot be tried at all. The overrides would *not* apply
    (they match by ELF name and entry point), so expect visible corruption rather
    than a working game - but it is real information, and it is the step that makes
    the measurement above possible for anyone holding the disc.

    **The same shape applies to LCS**, and note that the LCS project *vendors* a
    copy of this runtime (`Liberty City Stories Recomp/vendor/ps2recomp/`, with its
    own `game_overrides.cpp`). So a multi-version mechanism belongs in the runtime,
    with the per-version data kept per game - otherwise every version has to be
    added twice.

16. **RetroAchievements support (owner ask, 2026-10-18: "is retroachievements
    support doable?").** Technically yes, and unusually cheaply - but the answer
    today is **no, and not for a technical reason**. Recorded here so nobody spends
    a week before finding that out.

    **Why it would be cheap, which is worth knowing.** A *recompilation* keeps the
    game's memory layout: the recompiled code runs at the ELF's own addresses and
    the game's globals sit where the retail build put them - the overrides in this
    repo are the proof, reading the cutscene flag at `0x6F4D14`, the pause flag at
    `0x4CD2AC` and the settings bytes near `0x4E47xx`, and the runtime's HLE arena
    (`0x00080000`-`0x00100000`) stays below the game's heap base. So RA's *existing*
    PS2 sets, which are authored against PCSX2's view of retail memory, would
    evaluate against the port's RDRAM **unchanged** - no re-authoring, which is the
    part that makes this normally expensive. Vice City Stories already has a set
    (167 achievements, plus a subset).

    **What it would take:**

    - **rcheevos** (MIT) with its `rc_client` - it parses and evaluates the
      conditions, keeps the session, and reports unlocks. The client supplies only a
      memory-read callback, which for this port is a `memcpy` out of RDRAM.
    - **The game ID**: RA's PS2 hash is the `BOOT2=` path from `SYSTEM.CNF` followed
      by the boot executable's contents, hashed. The port has both (`ps2x_iso`
      already parses `SYSTEM.CNF` for the boot file).
    - **An HTTP client with TLS**, which would be this port's first networked
      feature: libcurl on Linux, and something cross-buildable for the Windows
      target. This is the heaviest piece by far.
    - **UI**: a login field, an achievements list and unlock toasts, in the existing
      ImGui settings menu.

    Roughly one to two weeks, most of it the HTTP/TLS plumbing and testing (which
    needs an RA account). Not blocked by anything in the port.

    **Why it is not allowed.** RA's standalone-support rules say it outright:
    *"Decompilations, recompilations, and other unofficial ports are not eligible
    for standalone sets."* And the port is not an emulator, so it cannot be an
    approved emulator client either. Reporting as PCSX2 to get around it would be
    against the rules and would put players' accounts at risk - not something to
    build, and not something to suggest to players.

    **So the options are:**

    - **watch the policy.** Recompilation is a live subject there - there are forum
      threads asking for it, and the exclusion line is a recent addition to the
      docs, so it is being actively decided rather than long settled. If it changes,
      the work above is a week.
    - **a local set, which is the intended route (owner, 2026-10-18: "park that for
      later incase the rules change. maybe we could make our own set that just the
      port uses").** Parked, but this is what to build when it is picked up - see
      below.
    - **note for whoever does it**: RA's hardcore mode forbids cheats, and this port
      ships a `mods/` folder with cheats in it - an RA client would have to switch
      those off, or restrict them to casual.

    **The local set, in the shape it should take.** Everything above stays, minus
    the HTTP client and the server:

    - **Use rcheevos as the evaluator, and its condition format for the set.** It is
      MIT, it is the format the RA tooling and editor already speak, and it keeps
      the set *portable*: if RA ever accepts recompilations, the same definitions
      can be submitted rather than re-authored. Writing our own evaluator and format
      would be more work for less.
    - **No network at all**, which removes the whole libcurl/TLS problem and the
      need for an account. Definitions ship in the repo as our own content (like the
      prompt packs and `mods/`), and unlocks are a small local file.
    - **The addresses are ours to find, and the tools are already here**: the
      port's F9 RAM dump, the ped dump and the camera trace exist for exactly this
      kind of hunt, and conditions authored against this port's memory are authored
      against retail's, because the layout matches.
    - **Cost**: the integration is small - a few days for rcheevos, a memory
      callback, a list and unlock toasts in the settings menu, and the local file.
      The set itself is the real work, and it is authoring rather than programming:
      a set worth having is weeks of playing with the debugger open, the way the 167
      on RA's set were made.
    - **UI placement** is the one open design question: the settings menu is the
      obvious home, with unlocks as an in-game toast the way the game itself would
      show one.

    **The set itself: what the official trilogy does, so ours feels the same.** The
    reference is *The Trilogy - The Definitive Edition* (2021), which is the only
    official achievement set these games have ever had. Pulled from Steam's public
    achievement data (2026-10-18), complete lists:

    | game | achievements | hidden |
    |---|---|---|
    | GTA III - Definitive Edition | **29** | 4 |
    | GTA Vice City - Definitive Edition | **34** | 4 |
    | GTA San Andreas - Definitive Edition | **35** | 3 |

    **The categories are stable across all three**, which is the useful part - it is
    a house style, not three unrelated sets. Counted across the trilogy:

    | category | III | VC | SA | what it looks like |
    |---|---|---|---|---|
    | story milestones | 5 | 7 | 5 | one per act, named after the mission: *"Complete 'The Exchange'."* |
    | mission challenges | 4 | 4 | 3 | a special way through one mission, almost always hidden: *"Keep the two mafia members alive during 'Triads and Tribulations'."* |
    | side activities | 6 | 9 | 8 | the whole job ladder: Paramedic 12, Firefighter, Vigilante 12, taxi fares, pizza, RC races, import/export, properties, vehicle school |
    | collectibles / sets | 3 | 3 | 0 | *"Collect 100 hidden packages."*, *"Complete 20 unique jumps."*, *"Use every weapon at least once."* - always the whole set in one achievement, never one per item |
    | stat and rating milestones | 5 | 6 | 6 | criminal rating tiers, respect, sex appeal, weight, six-star wanted level, $1,000,000, property damage |
    | quirky one-offs | 3 | 4 | 9 | the flavour: listen to every station, a 30-second wheelie, keepie-uppy, buy a meal 8 times, drown, win at roulette |
    | capstones | 2 | 2 | 3 | 100% completion, unlock everything ("the platinum"), and SA's *"...Here we go again"* for a new game after 100% |

    **The rules that make it feel official**, worth following exactly:

    - **Broad, not deep.** ~30 achievements spanning every system in the game, one to
      three each. There is *no* achievement per story mission (VC has 59 missions and
      7 story achievements), and no grinding spam - the whole of a collectible set is
      one achievement.
    - **Name is a joke or a quote; the description is the instruction.** *"Wheels Up -
      Complete 20 unique jumps."* Consistent to the letter across all three sets.
    - **Hidden means a spoiler or a special method** - never just "secret".
    - **Exactly two capstones** in III and VC (100%, and unlock-all), which is the
      shape a player reads as "the platinum".
    - **Nothing difficulty-based**: these games have no difficulty setting, so no
      achievement references one.

    **Ours, by that recipe - 34 achievements, the same mix** (VCS's own content, all
    of it verified against the game's guides):

    | category | count | proposed |
    |---|---|---|
    | story milestones | 6 | **decided** (owner, 2026-10-18): the first and last mission of each chapter - **Soldier** / **To Victor, the Spoils**, **Jive Drive** / **From Zero to Hero**, **Brawn of the Dead** / **Last Stand** |
    | mission challenges | 4 | hidden, authored per mission: a no-damage delivery, keeping Lance alive, a kill with a named weapon, a stealth route - the same shape as *"Kill the property developer with a Golf Club during 'Four Iron'."* |
    | side activities | 9 | **Vice Sights** (5 levels), **Beach Patrol** (15), **Air Rescue** (15), **Fire Copter** (15), Paramedic 15, Firefighter 15, Vigilante 15, taxi fares, the **Shooting Range** (5 rounds) - plus **Empire Builder: acquire all 30 Empire Building Sites**, which is VCS's own system and deserves its slot |
    | collectibles / sets | 3 | all **99 Red Balloons**, all **36 unique stunt jumps**, every **Rampage** |
    | stat and rating milestones | 6 | $1,000,000, the top criminal rating, a six-star wanted level, maximum empire reputation, 100 gang kills, $1,000,000 in property damage |
    | quirky one-offs | 4 | listen to every radio station at least once, a long wheelie, a round of Golf, **Crash!** |
    | capstones | 2 | 100% completion, and unlock everything |

    That lands at **34**, level with Vice City DE, which is the right sibling to match.
    For contrast, RA's community set for VCS has **167** - a different philosophy
    (exhaustive, plus subsets), and a far bigger authoring job. The official scale is
    both more authentic and much less work.

## Third-party assets (release-page credits)

Both packs are by **AL2009man**, MIT-licensed, (c) 2024 Al. Lopez:

- **Gamepad Asset Pack** - https://github.com/AL2009man/Gamepad-Asset-Pack
  (the assets were originally created for
  [VSCView](https://github.com/Nielk1/VSCView/)). Used for the
  controller-page portrait art: `Controller Asset Pack/` - Xbox Series X,
  DualSense, DualShock 4, Switch Pro and Steam Deck images.
- **Gamepad Prompt Asset Pack** -
  https://github.com/AL2009man/Gamepad-Prompt-Asset-Pack. Used for the
  button-prompt glyphs: PlayStation face glyphs (Colour + white styles),
  Xbox face buttons, Nintendo glyphs, D-pad arrows.
- Minor: the keyboard keycap labels are rendered with **DejaVu Sans Bold**
  (Bitstream Vera / DejaVu licence) and the SVGs with `rsvg-convert`
  (LGPL); neither ships in the release, only the baked PNGs.
- Licence note: the Prompt Asset Pack README carves out platform "Guide"
  buttons from its commercial-use grant; our prompt configs never use
  guide/home glyphs (face buttons, d-pads, start/select only). The
  controller portraits come from the Controller Asset Pack (plain MIT).

## Where past fixes live

Read the `vcs-recomp-progress` memory notes (owner's Claude memory) or `git log`
before re-investigating a bug. Most VCS-specific behaviour is hooked in
`applyGtaVcsUs()` at the end of `game_overrides.cpp`, and movie playback in
`Kernel/Stubs/MPEG.cpp`.
