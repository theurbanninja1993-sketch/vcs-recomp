#ifndef PS2_PLATFORM_H
#define PS2_PLATFORM_H

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Small OS-specific helpers (ps2_platform.cpp keeps <windows.h> out of the
// files that use raylib).

// Folder of the running executable (falls back to the current directory).
std::filesystem::path ps2ExecutableDirectory();

// Where the port keeps settings, generated prompt art and logs: normally the
// user's config folder. In portable mode - a "portable.txt" next to the
// executable, or PS2X_PORTABLE=1 - a "config" folder next to the executable, so
// a build on a USB stick keeps its settings with it and touches nothing else.
// The folder is created if missing.
std::filesystem::path ps2ConfigDirectory();

// Marks the calling thread as the host frame loop (raylib's). Some work is far
// too slow to run from the emulated game thread - a stall there is a dropped
// frame and a gap in the audio - so it is deferred to the host thread.
void ps2SetHostThread();
bool ps2IsHostThread();

// Per-thread CPU time of this process: thread id, a label (the thread's name,
// or its id when unnamed) and ticks of 1/100 s. Keyed per *thread*, not per
// name - the main thread and the IOP worker both used to report as
// "ps2EntryRunner" and their sum hid which one was actually busy.
struct Ps2ThreadCpu
{
    uint64_t id = 0;
    std::string label;
    uint64_t ticks = 0;
};
std::vector<Ps2ThreadCpu> ps2ThreadCpuTicks();

// Ask the OS to schedule this thread favourably. The emulated game thread both
// runs the game and feeds the audio ring, and when it is scheduled in lumps the
// audio arrives in lumps and the ring runs dry between them. Best effort:
// failures (no privileges) are ignored.
void ps2RaiseCurrentThreadPriority();

// PS2X_HOST_PROFILE=<file> (Windows): a sampling profiler for the host. A
// background thread suspends the game, VU1 and GS threads ~1000 times
// a second, records a short stack (RtlVirtualUnwind) and writes the counts,
// as module+offset per frame, to <file> every 10 s. Symbolize offline with
// llvm-symbolizer against the same executable. No-op elsewhere.
void ps2StartHostProfiler();

// Resident memory of this process in MB (0 when unavailable). Reported in the
// perf log: a footprint that keeps climbing over a session is a leak.
size_t ps2ProcessResidentMb();

// Opens a folder in the desktop's file manager.
void ps2OpenFolder(const std::string &path);

// Reports a fatal startup error: stderr, plus a message box on Windows
// (where the game has no console window).
void ps2ShowFatalError(const std::string &message);

#endif
