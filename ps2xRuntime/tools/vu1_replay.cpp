// Offline VU1 replay benchmark. Replays MSCALs captured with
// PS2X_VU1_CAPTURE=<file>,<startFrame>,<count> and checks that the final VU1
// state, VU1 data memory and XGKICK output match the capture bit for bit.
//
// usage: vu1_replay <capture.bin> [passes]
#define private public // markVU1CodeModified() is a private PS2Memory helper
#include "runtime/ps2_memory.h"
#undef private
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "ps2_runtime.h"
#include "vu_unit_config.h"

#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The runtime library references the generated function table; the replay
// tool runs no guest code.
const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    // VU1_PROFILE=<file>: SIGPROF samples of the host RIP, written as
    // "offset-from-profileHandler" u32s (map with tools/rip_hist.py).
    std::vector<uint32_t> g_samples;
    void profileHandler(int, siginfo_t *, void *contextPtr)
    {
        const auto *context = static_cast<const ucontext_t *>(contextPtr);
        if (g_samples.size() < g_samples.capacity())
            g_samples.push_back(static_cast<uint32_t>(context->uc_mcontext.gregs[REG_RIP] -
                                                      reinterpret_cast<uint64_t>(&profileHandler)));
    }

    struct Record
    {
        uint32_t startPC = 0, top = 0, itop = 0;
        bool vu0 = false; // 'CVU0' record: 4 KB code/data and the VU0 unit
        uint32_t codeSize = PS2_VU1_CODE_SIZE;
        uint32_t dataSize = PS2_VU1_DATA_SIZE;
        VU1State before{};
        std::vector<uint8_t> code, dataBefore, dataAfter;
        VU1State after{};
    };

    // flagless: a compiled program that maintains no MAC/status flags ran.
    bool sameState(const VU1State &a, const VU1State &b, bool flagless)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 4) == 0 &&
               std::memcmp(&a.p, &b.p, 4) == 0 && std::memcmp(&a.i, &b.i, 4) == 0 && a.r == b.r &&
               (flagless || (a.mac == b.mac && a.status == b.status)) && a.clip == b.clip && a.pc == b.pc &&
               (!kVu0 || (a.cycles == b.cycles && a.ebit == b.ebit &&
                          a.haltAfterDelaySlot == b.haltAfterDelaySlot && a.dBitEnabled == b.dBitEnabled &&
                          a.tBitEnabled == b.tBitEnabled && a.stoppedByD == b.stoppedByD &&
                          a.stoppedByT == b.stoppedByT && a.top == b.top && a.itop == b.itop &&
                          a.branchPending == b.branchPending &&
                          (!a.branchPending || (a.branchTarget == b.branchTarget && a.branchDelay == b.branchDelay))));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <capture.bin> [passes]\n", argv[0]);
        return 2;
    }
    const int passes = argc > 2 ? std::atoi(argv[2]) : 1;
    if (passes <= 0)
    {
        std::fprintf(stderr, "passes must be positive\n");
        return 2;
    }

    std::vector<Record> records;
    if (FILE *f = std::fopen(argv[1], "rb"))
    {
        for (;;)
        {
            uint32_t header[4];
            const size_t headerBytes = std::fread(header, 1, sizeof(header), f);
            if (headerBytes == 0u && std::feof(f))
                break;
            Record r;
            if (header[0] == 0x30555643u) // 'CVU0'
            {
                r.vu0 = true;
                r.codeSize = PS2_VU0_CODE_SIZE;
                r.dataSize = PS2_VU0_DATA_SIZE;
            }
            else if (header[0] == 0x31555643u) // 'CVU1'
            {
                r.vu0 = false;
                r.codeSize = PS2_VU1_CODE_SIZE;
                r.dataSize = PS2_VU1_DATA_SIZE;
            }
            else
            {
                std::fprintf(stderr, "bad record magic\n");
                return 1;
            }
            r.startPC = header[1];
            r.top = header[2];
            r.itop = header[3];
            r.code.resize(r.codeSize);
            r.dataBefore.resize(r.dataSize);
            r.dataAfter.resize(r.dataSize);
            if (std::fread(&r.before, sizeof(VU1State), 1, f) != 1 ||
                std::fread(r.code.data(), 1, r.code.size(), f) != r.code.size() ||
                std::fread(r.dataBefore.data(), 1, r.dataBefore.size(), f) != r.dataBefore.size() ||
                std::fread(&r.after, sizeof(VU1State), 1, f) != 1 ||
                std::fread(r.dataAfter.data(), 1, r.dataAfter.size(), f) != r.dataAfter.size())
            {
                std::fprintf(stderr, "truncated record payload\n");
                std::fclose(f);
                return 1;
            }
            records.push_back(std::move(r));
        }
        std::fclose(f);
    }
    else
    {
        std::perror(argv[1]);
        return 1;
    }
    if (records.empty())
    {
        std::fprintf(stderr, "capture contains no records\n");
        return 1;
    }
    std::printf("%zu records\n", records.size());

    PS2Memory memory;
    if (!memory.initialize())
        return 1;
    GS gs;
    uint64_t gifHash = 1469598103934665603ull;
    uint64_t gifBytes = 0;
    memory.setGifPacketCallback([&](const uint8_t *data, uint32_t size)
                                {
                                    gifBytes += size;
                                    for (uint32_t i = 0; i < size; ++i)
                                        gifHash = (gifHash ^ data[i]) * 1099511628211ull; });
    VU1Interpreter vu1(VU1Interpreter::Unit::VU1);
    VU1Interpreter vu0(VU1Interpreter::Unit::VU0);

    const char *profilePath = std::getenv("VU1_PROFILE");
    if (profilePath)
    {
        g_samples.reserve(1u << 22);
        struct sigaction action{};
        action.sa_sigaction = profileHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, nullptr);
        itimerval timer{{0, 100}, {0, 100}};
        setitimer(ITIMER_PROF, &timer, nullptr);
    }

    // PS2X_VU1_FOOTPRINT=1: report which bytes of VU1 data memory each program
    // writes, by diffing the captured before/after images. Two programs can only be
    // run on different threads if their writes do not overlap - and if the dominant
    // pair turns out to be double-buffered (each writing its own half), that is the
    // evidence for doing it.
    if (std::getenv("PS2X_VU1_FOOTPRINT") != nullptr)
    {
        struct Range
        {
            uint32_t lo = 0xFFFFFFFFu, hi = 0u, touches = 0u, records = 0u;
        };
        std::map<uint32_t, Range> byProgram;
        for (const Record &r : records)
        {
            if (r.vu0 || r.dataBefore.size() != r.dataAfter.size())
                continue;
            Range &range = byProgram[r.startPC];
            ++range.records;
            for (size_t b = 0; b < r.dataBefore.size(); ++b)
            {
                if (r.dataBefore[b] == r.dataAfter[b])
                    continue;
                ++range.touches;
                range.lo = std::min<uint32_t>(range.lo, static_cast<uint32_t>(b & ~0x0Fu));
                range.hi = std::max<uint32_t>(range.hi, static_cast<uint32_t>(b | 0x0Fu));
            }
        }
        std::printf("\nVU1 data memory written, per program (bytes, 16-aligned):\n");
        std::printf("   %-10s %8s %10s %14s %14s\n", "pc", "records", "changed", "writes from", "writes to");
        std::vector<std::pair<uint32_t, Range>> ranked(byProgram.begin(), byProgram.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) { return a.second.touches > b.second.touches; });
        for (const auto &entry : ranked)
            if (entry.second.touches != 0u)
                std::printf("   0x%-8x %8u %10u      0x%06x      0x%06x\n", entry.first, entry.second.records,
                            entry.second.touches, entry.second.lo, entry.second.hi);
        // What decides whether two programs can run on different threads is not
        // whether their writes overlap over a whole capture, but whether the ones
        // that run next to each other do. A double-buffered pair shows up here as
        // adjacent records touching disjoint bytes.
        std::vector<std::vector<uint8_t>> written;
        written.reserve(records.size());
        for (const Record &r : records)
        {
            std::vector<uint8_t> touched(r.vu0 ? 0u : r.dataBefore.size(), 0u);
            if (!r.vu0 && r.dataBefore.size() == r.dataAfter.size())
                for (size_t b = 0; b < touched.size(); ++b)
                    touched[b] = r.dataBefore[b] != r.dataAfter[b] ? 1u : 0u;
            written.push_back(std::move(touched));
        }
        size_t pairs = 0, disjoint = 0, overlapPairs = 0;
        size_t overlapBytesTotal = 0, overlapWorst = 0;
        for (size_t i = 0; i + 1 < written.size(); ++i)
        {
            if (written[i].empty() || written[i + 1].empty())
                continue;
            ++pairs;
            size_t overlap = 0;
            const size_t n = std::min(written[i].size(), written[i + 1].size());
            for (size_t b = 0; b < n; ++b)
                overlap += (written[i][b] & written[i + 1][b]);
            if (overlap == 0u)
                ++disjoint;
            else
            {
                ++overlapPairs;
                overlapBytesTotal += overlap;
                overlapWorst = std::max(overlapWorst, overlap);
            }
        }
        std::printf("\nadjacent pairs: %zu checked, %zu disjoint (%.1f%%), %zu overlapping, median overlap %zu bytes, worst %zu\n",
                    pairs, disjoint, pairs ? 100.0 * static_cast<double>(disjoint) / static_cast<double>(pairs) : 0.0,
                    overlapPairs, overlapPairs ? overlapBytesTotal / overlapPairs : 0u, overlapWorst);
        // Where the overlaps actually are. A handful of fixed words is a handshake
        // that a parallel runner can order or own; a wide scatter is a shared buffer
        // and means these two programs genuinely interact.
        std::map<uint32_t, size_t> hotBytes;
        for (size_t i = 0; i + 1 < written.size(); ++i)
        {
            if (written[i].empty() || written[i + 1].empty())
                continue;
            const size_t n = std::min(written[i].size(), written[i + 1].size());
            for (size_t b = 0; b < n; ++b)
                if (written[i][b] & written[i + 1][b])
                    ++hotBytes[static_cast<uint32_t>(b & ~0x0Fu)];
        }
        std::vector<std::pair<size_t, uint32_t>> hot(hotBytes.begin(), hotBytes.end());
        std::sort(hot.begin(), hot.end(), std::greater<>());
        std::printf("\nwhere the overlaps are (16-byte blocks, most frequent first):\n");
        for (size_t i = 0; i < hot.size() && i < 12u; ++i)
            std::printf("    0x%06x  overlaps in %zu of the %zu overlapping pairs\n", hot[i].second, hot[i].first,
                        overlapPairs);
        const Range *a = ranked.size() > 0 ? &ranked[0].second : nullptr;
        const Range *b = ranked.size() > 1 ? &ranked[1].second : nullptr;
        if (a != nullptr && b != nullptr)
            std::printf("\nthe two busiest programs %s\n",
                        (a->hi < b->lo || b->hi < a->lo) ? "write disjoint ranges - plausible to run in parallel"
                                                        : "OVERLAP - not independent as they stand");
        return 0;
    }

    // PS2X_VU1_SCHEDULE=1: simulate an in-order, out-of-order-completion parallel
    // runner over the captured workload, using each record's real write set and its
    // real VU1 duration, and report the speedup against running them serially.
    //
    // This is an upper bound and worth knowing it: it enforces write-after-write only.
    // A consumer that *reads* what an earlier program wrote is invisible to it, and
    // that handshake is exactly what a double-buffered VU1 stream is full of, so the
    // real design will do worse. If the bound is not comfortably above the ~1.45x
    // this machine needs, there is no point building it.
    if (std::getenv("PS2X_VU1_SCHEDULE") != nullptr)
    {
        struct Job
        {
            std::vector<uint8_t> writes;
            double seconds = 0.0;
        };
        extern std::atomic<uint64_t> g_vuRunMicros[2];
        std::vector<Job> jobs(records.size());
        for (size_t i = 0; i < records.size(); ++i)
        {
            const Record &r = records[i];
            Job &job = jobs[i];
            job.writes.assign(r.vu0 ? 0u : r.dataBefore.size(), 0u);
            if (!r.vu0 && r.dataBefore.size() == r.dataAfter.size())
                for (size_t b = 0; b < job.writes.size(); ++b)
                    job.writes[b] = r.dataBefore[b] != r.dataAfter[b] ? 1u : 0u;
            uint8_t *const code = r.vu0 ? memory.getVU0Code() : memory.getVU1Code();
            uint8_t *const data = r.vu0 ? memory.getVU0Data() : memory.getVU1Data();
            if (std::memcmp(code, r.code.data(), r.codeSize) != 0)
            {
                std::memcpy(code, r.code.data(), r.codeSize);
                if (r.vu0)
                    memory.markVU0CodeModified();
                else
                    memory.markVU1CodeModified();
            }
            std::memcpy(data, r.dataBefore.data(), r.dataSize);
            VU1Interpreter &vu = r.vu0 ? vu0 : vu1;
            vu.state() = r.before;
            vu.clearRanFlagless();
            const uint64_t before = g_vuRunMicros[r.vu0 ? 0 : 1].load(std::memory_order_relaxed);
            vu.execute(code, r.codeSize, data, r.dataSize, gs, &memory, r.startPC, r.top, r.itop, 65536);
            job.seconds = static_cast<double>(g_vuRunMicros[r.vu0 ? 0 : 1].load(std::memory_order_relaxed) - before) / 1e6;
        }
        double serial = 0.0;
        for (const Job &job : jobs)
            serial += job.seconds;
        const auto conflict = [&](size_t a, size_t b)
        {
            const size_t n = std::min(jobs[a].writes.size(), jobs[b].writes.size());
            for (size_t i = 0; i < n; ++i)
                if (jobs[a].writes[i] & jobs[b].writes[i])
                    return true;
            return false;
        };
        std::printf("\nparallel schedule over %zu records, serial VU1 time %.4f s\n", jobs.size(), serial);
        for (const int workers : {2, 3, 4, 6})
        {
            std::vector<char> started(jobs.size(), 0);
            std::vector<double> remaining(jobs.size(), 0.0);
            std::vector<size_t> running;
            double now = 0.0;
            for (;;)
            {
                bool progress = true;
                while (progress && static_cast<int>(running.size()) < workers)
                {
                    progress = false;
                    for (size_t i = 0; i < jobs.size(); ++i)
                    {
                        if (started[i] != 0)
                            continue;
                        bool blocked = false;
                        for (const size_t j : running)
                            if (conflict(j, i))
                                blocked = true;
                        if (blocked)
                            break; // in order: this one must go first
                        started[i] = 1;
                        remaining[i] = jobs[i].seconds;
                        running.push_back(i);
                        progress = true;
                        break;
                    }
                }
                if (running.empty())
                    break;
                double step = 1e300;
                for (const size_t j : running)
                    step = std::min(step, remaining[j]);
                now += step;
                for (size_t k = 0; k < running.size();)
                {
                    remaining[running[k]] -= step;
                    if (remaining[running[k]] <= 1e-12)
                        running.erase(running.begin() + static_cast<std::ptrdiff_t>(k));
                    else
                        ++k;
                }
            }
            std::printf("    %d workers: %.4f s  ->  %.2fx (bound, write-after-write only)\n", workers, now,
                        now > 0.0 ? serial / now : 0.0);
        }
        return 0;
    }

    size_t mismatches = 0;
    double seconds = 0.0;
    // Per-program VU1 time, so the cost can be read by *what* is being run rather
    // than as one average over a capture. The average hides the thing that decides
    // where to work: a program can be expensive because it is long, or because it is
    // expensive per cycle, and those are different problems.
    struct ProgramStat
    {
        uint64_t calls = 0;
        uint64_t cycles = 0;
        uint64_t micros = 0;
    };
    std::map<uint32_t, ProgramStat> byProgram;
    // Counters for the per-cycle comparison with the game's perf log.
    extern std::atomic<uint64_t> g_vuRunCycles[2], g_vuRunCalls[2];
    const uint64_t cyclesBefore = g_vuRunCycles[1].load();
    extern std::atomic<uint64_t> g_vuRunMicros[2];
    const uint64_t microsBefore = g_vuRunMicros[1].load();
    const uint64_t callsBefore = g_vuRunCalls[1].load();
    for (int pass = 0; pass < passes; ++pass)
    {
        for (size_t index = 0; index < records.size(); ++index)
        {
            const Record &r = records[index];
            uint8_t *const code = r.vu0 ? memory.getVU0Code() : memory.getVU1Code();
            uint8_t *const data = r.vu0 ? memory.getVU0Data() : memory.getVU1Data();
            if (std::memcmp(code, r.code.data(), r.codeSize) != 0)
            {
                std::memcpy(code, r.code.data(), r.codeSize);
                if (r.vu0)
                    memory.markVU0CodeModified();
                else
                    memory.markVU1CodeModified();
            }
            std::memcpy(data, r.dataBefore.data(), r.dataSize);
            VU1Interpreter &vu = r.vu0 ? vu0 : vu1;
            vu.state() = r.before;
            vu.clearRanFlagless();

            const uint64_t recordCycles0 = g_vuRunCycles[1].load(std::memory_order_relaxed);
            const uint64_t recordMicros0 = g_vuRunMicros[1].load(std::memory_order_relaxed);
            const auto t0 = std::chrono::steady_clock::now();
            vu.execute(code, r.codeSize, data, r.dataSize, gs, &memory, r.startPC, r.top, r.itop, 65536);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!r.vu0)
            {
                ProgramStat &stat = byProgram[r.startPC];
                ++stat.calls;
                stat.cycles += g_vuRunCycles[1].load(std::memory_order_relaxed) - recordCycles0;
                stat.micros += g_vuRunMicros[1].load(std::memory_order_relaxed) - recordMicros0;
            }

            if (pass == 0 && (!sameState(vu.state(), r.after, vu.ranFlaglessProgram()) ||
                              std::memcmp(data, r.dataAfter.data(), r.dataSize) != 0))
            {
                if (mismatches < 10)
                {
                    std::printf("mismatch: record %zu pc=0x%x\n", index, r.startPC);
                    if constexpr (kVu0)
                    {
                        const auto &a = vu1.state();
                        const auto &b = r.after;
                        std::printf("  vf=%d vi=%d acc=%d data=%d cycles=%llu/%llu mac=%x/%x status=%x/%x clip=%x/%x pc=%x/%x\n",
                                    std::memcmp(a.vf, b.vf, sizeof(a.vf)), std::memcmp(a.vi, b.vi, sizeof(a.vi)),
                                    std::memcmp(a.acc, b.acc, sizeof(a.acc)), std::memcmp(vuData(memory), r.dataAfter.data(), kVuDataSize),
                                    static_cast<unsigned long long>(a.cycles), static_cast<unsigned long long>(b.cycles),
                                    a.mac, b.mac, a.status, b.status, a.clip, b.clip, a.pc, b.pc);
                    }
                }
                ++mismatches;
            }
        }
    }
    std::printf("mismatches=%zu time=%.3fs (%.1f us/record) gifBytes=%llu gifHash=%016llx\n", mismatches, seconds,
                seconds * 1e6 / static_cast<double>(records.size() * passes), static_cast<unsigned long long>(gifBytes / passes),
                static_cast<unsigned long long>(gifHash));
    {
        // The VU's own cycle count over the same work, so this can be compared with
        // the game's perf log per *cycle* rather than per call: a call's length
        // varies by more than an order of magnitude between scenes, so per-call
        // figures taken from different scenes are not comparable.
        const uint64_t cycles = g_vuRunCycles[1].load() - cyclesBefore;
        const uint64_t vuCalls = g_vuRunCalls[1].load() - callsBefore;
        if (cycles != 0u)
            std::printf("cycles=%llu vuCalls=%llu cyc/call=%.1f us/call=%.2f ns/cycle=%.3f\n",
                        static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(vuCalls),
                        static_cast<double>(cycles) / static_cast<double>(vuCalls), seconds * 1e6 / static_cast<double>(vuCalls),
                        seconds * 1e9 / static_cast<double>(cycles));
    }
    {
        // VU1's own host time, which is *not* what `seconds` above measures.
        //
        // `execute()` returns only after the XGKICK output has been handed to a
        // real GS backend, so `seconds` is the cost of the VU1 work *and* the GS
        // work that consumes its output: a profile of this tool is dominated by
        // gsgpu::Renderer::* rather than by the generated VU1 programs. Timing the
        // VU1 emulation alone has to come from the runtime's own counter, and this
        // is the figure that lines up with the game's perf log, whose vu1Ms is the
        // same quantity measured inside the running game.
        extern std::atomic<uint64_t> g_vuRunMicros[2];
        const uint64_t micros = g_vuRunMicros[1].load() - microsBefore;
        const uint64_t vuCalls = g_vuRunCalls[1].load() - callsBefore;
        if (vuCalls != 0u)
            std::printf("vu1Only: us/call=%.2f total=%.3fs  (execute()=%.1f us/record includes the GS)\n",
                        static_cast<double>(micros) / static_cast<double>(vuCalls), micros / 1e6, seconds * 1e6 / static_cast<double>(records.size() * passes));
    }
    {
        // Ranked by total VU1 time: which programs pay for the capture, and whether
        // they pay because they run a lot or because each cycle costs more.
        std::vector<std::pair<double, uint32_t>> ranked;
        for (const auto &entry : byProgram)
            ranked.emplace_back(static_cast<double>(entry.second.micros) / passes, entry.first);
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        std::printf("\nper program (VU1 only, top %d of %zu):\n", std::min<size_t>(ranked.size(), 12u), ranked.size());
        std::printf("   %-10s %8s %9s %9s %9s %8s\n", "pc", "share", "calls", "cyc/call", "us/call", "ns/cyc");
        double total = 0.0;
        for (const auto &entry : ranked)
            total += entry.first;
        for (size_t i = 0; i < ranked.size() && i < 12u; ++i)
        {
            const ProgramStat &stat = byProgram[ranked[i].second];
            const uint64_t calls = stat.calls / static_cast<uint64_t>(passes);
            const uint64_t cycles = stat.cycles / static_cast<uint64_t>(passes);
            const double micros = static_cast<double>(stat.micros) / passes;
            std::printf("   0x%-8x %7.1f%% %9llu %9.1f %9.2f %8.2f\n", ranked[i].second,
                        total > 0.0 ? 100.0 * micros / total : 0.0, static_cast<unsigned long long>(calls),
                        calls != 0u ? static_cast<double>(cycles) / static_cast<double>(calls) : 0.0,
                        calls != 0u ? micros / static_cast<double>(calls) : 0.0,
                        cycles != 0u ? micros * 1000.0 / static_cast<double>(cycles) : 0.0);
        }
    }
    if (profilePath)
    {
        itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        if (FILE *f = std::fopen(profilePath, "wb"))
        {
            std::fwrite(g_samples.data(), sizeof(uint32_t), g_samples.size(), f);
            std::fclose(f);
        }
        // <file>.maps for tools/prof_analyze.py (samples in shared libraries).
        if (FILE *f = std::fopen((std::string(profilePath) + ".maps").c_str(), "w"))
        {
            std::fprintf(f, "handler %llx\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&profileHandler)));
            if (FILE *maps = std::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), maps))
                    std::fputs(line, f);
                std::fclose(maps);
            }
            std::fclose(f);
        }
    }
    return mismatches == 0 ? 0 : 1;
}
