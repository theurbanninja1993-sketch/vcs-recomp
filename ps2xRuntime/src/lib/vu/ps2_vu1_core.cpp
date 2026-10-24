#include <iostream>
#include <atomic>
#include <chrono>
#include <sstream>
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ps2_log.h>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;

namespace
{
    // Opt-in first-failure inputs. Normal runs neither copy VU memory nor
    // allocate these buffers. Each worker owns its capture and VU instances.
    struct VuFailureInput
    {
        bool captured = false;
        VU1State state{};
        std::vector<uint8_t> code, data;
    };
    thread_local bool s_vuRunIsResume = false;

    // Always-live, timing-neutral ring of the most recent VU runs. It records
    // only the entry state (a small memcpy, no allocation) so it does not
    // perturb timing the way the full failure capture does - that copies 32 KiB+
    // of code and data per run, which suppresses the intermittent VU
    // budget-exhaustion cascade. Enabled by PS2X_VU_RING=1; on a failure the
    // ring is dumped (PS2X_VU_RING_DIR) to show the data-driven loop bounds.
    struct VuRunRecord
    {
        VU1State state{};
        uint64_t enteredCycle = 0;
        uint64_t exitCycle = 0;
        uint32_t exitKind = 0; // 0 = ended/unknown, 1 = cycle budget, 2 = stop request
    };
    constexpr uint32_t kVuRunRing = 32;
    thread_local std::array<VuRunRecord, kVuRunRing> s_vuRunRing;
    thread_local uint32_t s_vuRunRingNext = 0;
    thread_local bool s_vuRunRingDumped = false;

    const std::string &vuRunRingDirectory()
    {
        static const std::string directory = []
        {
            const char *value = std::getenv("PS2X_VU_RING_DIR");
            return value ? std::string(value) : std::string();
        }();
        return directory;
    }

    // Per-entry-PC cycle usage (PS2X_VU_PC_STATS=1): shows which compiled
    // microprograms run near the 65,536-cycle budget, i.e. the cascade's
    // candidates, without needing the rare failure itself. Cheap: two array
    // updates per run, no allocation.
    constexpr uint32_t kVuPcSlots = 2048; // 16 KiB VU1 code, one slot per 8 bytes
    thread_local uint32_t s_vuPcMax[kVuPcSlots];
    thread_local uint32_t s_vuPcCount[kVuPcSlots];
    thread_local uint64_t s_vuPcRuns = 0;

    void reportVuPcStats()
    {
        struct Entry
        {
            uint32_t pc, maxCycles, count;
        };
        std::vector<Entry> entries;
        for (uint32_t slot = 0; slot < kVuPcSlots; ++slot)
            if (s_vuPcCount[slot] != 0u)
                entries.push_back({slot << 3, s_vuPcMax[slot], s_vuPcCount[slot]});
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.maxCycles > b.maxCycles; });
        std::cerr << "[vu:pcstats] runs=" << s_vuPcRuns << " top entry PCs by max cycles:";
        for (size_t i = 0; i < entries.size() && i < 8u; ++i)
            std::cerr << " 0x" << std::hex << entries[i].pc << std::dec << ":" << entries[i].maxCycles << "/"
                      << entries[i].count;
        std::cerr << std::endl;
    }

    void dumpVuRunRing(const std::string &directory, bool vu1, const VU1State &finalState, uint64_t finalCycle,
                       uint32_t maxCycles, bool compiled, bool resumed)
    {
        if (directory.empty() || s_vuRunRingDumped)
            return;
        s_vuRunRingDumped = true; // one dump per session: the first failure
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream out(std::filesystem::path(directory) / (vu1 ? "vu1-ring.txt" : "vu0-ring.txt"));
        if (!out)
            return;
        out << "# recent VU runs, oldest first; entry state per run\n";
        out << "# reason=" << (finalCycle >= maxCycles ? "cycle-budget" : "stop")
            << " compiled=" << compiled << " resumed=" << resumed << " finalCycle=" << finalCycle
            << " maxCycles=" << maxCycles << " frame=" << g_ps2PresentedFrameCounter.load(std::memory_order_relaxed)
            << "\n";
        out << "# final pc=0x" << std::hex << finalState.pc << std::dec << " top=" << finalState.top
            << " itop=" << finalState.itop << " cycles=" << finalState.cycles << "\n";
        out << "# final vi:";
        for (int v = 0; v < 16; ++v)
            out << " " << finalState.vi[v];
        out << "\n";
        for (uint32_t i = 0; i < kVuRunRing; ++i)
        {
            const VuRunRecord &r = s_vuRunRing[(s_vuRunRingNext + i) % kVuRunRing];
            if (r.enteredCycle == 0u && r.state.pc == 0u)
                continue;
            out << "run " << i << " pc=0x" << std::hex << r.state.pc << std::dec << " top=" << r.state.top
                << " itop=" << r.state.itop << " enter=" << r.enteredCycle << " exit=" << r.exitCycle
                << " exitKind=" << r.exitKind << "\n";
            out << "  vi:";
            for (int v = 0; v < 16; ++v)
                out << " " << r.state.vi[v];
            out << "\n";
        }
    }

    bool writeVuFailureFile(const std::filesystem::path &path, const void *data, size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        file.close();
        return !file.fail();
    }

    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

namespace
{
    // Resets XGKICK bookkeeping without zero-filling the 64 KiB packet buffer
    // (only bytes below copiedBytes are ever read); clearing it per MSCAL
    // cost ~10% of the game thread.
    template <typename Xgkick>
    void resetXgkickState(Xgkick &xgkick)
    {
        xgkick.sourceAddress = 0;
        xgkick.totalBytes = 0;
        xgkick.copiedBytes = 0;
        xgkick.currentTagEnd = 0;
        xgkick.cycleCredit = 0;
        xgkick.issueCycle = 0;
        xgkick.active = false;
        xgkick.currentTagEop = false;
    }
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_efuMask = m_storeMask = m_vfMask = m_viMask = m_accMask = 0;
    m_flagHead = m_flagCount = 0;
    resetXgkickState(m_xgkick);
    m_vfReady = {};
    m_vfRegReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    m_nextCommitCycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    // PS2X_VU1_SKIP_MACFLAGS: diagnostic only. Skips the per-lane MAC flag
    // computation but keeps the result normalisation, so the arithmetic is
    // unchanged - only the flags are wrong. If a capture reports the same
    // mismatches and gifHash with this on, nothing in it reads MAC flags, and
    // the timing difference is the flag path's cost.
    static const bool s_skipFlags = std::getenv("PS2X_VU1_SKIP_MACFLAGS") != nullptr;
    if (s_skipFlags)
    {
        for (uint32_t component = 0; component < 4u; ++component)
        {
            laneFlags[component] = 0u;
            if ((dest & laneForComponent(component)) == 0u)
                continue;
            uint32_t bits = 0u;
            std::memcpy(&bits, &result[component], 4);
            const uint32_t exponent = (bits >> 23) & 0xFFu;
            if (exponent >= 2u && exponent <= 253u)
                continue;
            uint32_t flags = 0u;
            result[component] = normalizeResult(result[component], flags);
        }
        return;
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        // A comfortably normal result can't be zero, overflow or underflow, so
        // the (costly, x87) exact recomputation would only reproduce its sign.
        uint32_t resultBits = 0u;
        std::memcpy(&resultBits, &result[component], sizeof(resultBits));
        const uint32_t resultExponent = (resultBits >> 23) & 0xFFu;
        if (resultExponent >= 2u && resultExponent <= 253u)
        {
            laneFlags[component] = (resultBits & 0x80000000u) != 0u ? 0x2u : 0u;
            continue;
        }

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        // The product of two floats is exact in a double (48-bit significand,
        // exponent within range), so this matches the long double evaluation
        // of normalizeFmacExactResult without x87 arithmetic.
        const double exactProduct = static_cast<double>(left) * static_cast<double>(right);
        const double magnitude = std::fabs(exactProduct);
        uint32_t productFlags = std::signbit(exactProduct) ? 0x2u : 0u;
        if (magnitude == 0.0)
            productFlags |= 0x1u;
        else if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
            productFlags |= 0x8u;
        else if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
            productFlags |= 0x5u;
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = allocFlagEntry();
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (uint32_t i = 0; i < m_flagCount; ++i)
        if (flagEntry(i).issueCycle == m_cycle)
            flagEntry(i).writesStatus = false;
    if (FlagPipelineEntry *entry = allocFlagEntry())
    {
        entry->status = static_cast<uint32_t>(immediate) & 0xFC0u;
        entry->writesSticky = true;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    if (FlagPipelineEntry *entry = allocFlagEntry())
    {
        entry->clip = m_workingClip;
        entry->writesClip = true;
        return;
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (uint32_t i = 0; i < m_flagCount; ++i)
        if (flagEntry(i).issueCycle == m_cycle)
            flagEntry(i).writesClip = false;
    if (FlagPipelineEntry *entry = allocFlagEntry())
    {
        entry->clip = m_workingClip;
        entry->writesClip = true;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_nextCommitCycle = std::min(m_nextCommitCycle, m_fdiv.readyCycle);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            m_efuMask |= 1u << static_cast<uint32_t>(&entry - m_efu.data());
            entry.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min(m_nextCommitCycle, entry.readyCycle);
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            m_storeMask |= 1u << static_cast<uint32_t>(&store - m_storePipeline.data());
            store.readyCycle = m_cycle + 1u;
            m_nextCommitCycle = std::min(m_nextCommitCycle, store.readyCycle);
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_vfMask |= 1u << static_cast<uint32_t>(&write - m_vfWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_viMask |= 1u << static_cast<uint32_t>(&write - m_viWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            m_accMask |= 1u << static_cast<uint32_t>(&write - m_accWritePipeline.data());
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

namespace
{
    // PS2X_VU1_EXACT_PIPELINE restores queued (latency-delayed) VF/ACC
    // writes. The default is verified bit-exact with tools/vu1_replay: a read
    // of an in-flight VF/ACC register stalls until the write lands, so it
    // always observes the new value and the write can land immediately.
    bool fastCommitEnabled()
    {
        static const bool s_exact = std::getenv("PS2X_VU1_EXACT_PIPELINE") != nullptr;
        return !s_exact;
    }
}

void VU1Interpreter::commitReadyPipelines()
{
    if (m_cycle < m_nextCommitCycle)
        return;

    // Single pass per pipeline over live slots (ascending index, as before):
    // commit what is ready and track the earliest pending readyCycle.
    uint64_t next = UINT64_MAX;
    const uint64_t cycle = m_cycle;

    commitFlags(cycle, next);

    if (m_fdiv.valid)
    {
        if (m_fdiv.readyCycle <= cycle)
        {
            m_state.q = m_fdiv.value;
            const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
            m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            m_fdiv = {};
        }
        else
        {
            next = std::min(next, m_fdiv.readyCycle);
        }
    }

    for (uint32_t live = m_efuMask; live != 0u; live &= live - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
        ScalarPipelineEntry &entry = m_efu[index];
        if (entry.readyCycle <= cycle)
        {
            m_state.p = entry.value;
            entry.valid = false;
            m_efuMask &= ~(1u << index);
        }
        else
        {
            next = std::min(next, entry.readyCycle);
        }
    }

    for (uint32_t live = m_storeMask; live != 0u; live &= live - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
        PendingStore &store = m_storePipeline[index];
        if (store.readyCycle > cycle)
        {
            next = std::min(next, store.readyCycle);
            continue;
        }
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store.valid = false;
        m_storeMask &= ~(1u << index);
    }

    for (uint32_t live = m_vfMask; live != 0u; live &= live - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
        PendingVfWrite &write = m_vfWritePipeline[index];
        if (write.readyCycle > cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write.valid = false;
        m_vfMask &= ~(1u << index);
    }

    for (uint32_t live = m_viMask; live != 0u; live &= live - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
        PendingViWrite &write = m_viWritePipeline[index];
        if (write.readyCycle > cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write.valid = false;
        m_viMask &= ~(1u << index);
    }

    for (uint32_t live = m_accMask; live != 0u; live &= live - 1u)
    {
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
        PendingAccWrite &write = m_accWritePipeline[index];
        if (write.readyCycle > cycle)
        {
            next = std::min(next, write.readyCycle);
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write.valid = false;
        m_accMask &= ~(1u << index);
    }

    m_nextCommitCycle = next;
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t first = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (first + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + first, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

extern std::atomic<int32_t> g_ps2Vu1TraceRemaining;
extern std::atomic<int32_t> g_ps2Vu1TraceKicksRemaining;

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    if (g_ps2Vu1TraceKicksRemaining.load(std::memory_order_relaxed) > 0)
    {
        if (g_ps2Vu1TraceKicksRemaining.fetch_sub(1, std::memory_order_relaxed) > 0)
        {
            std::ostringstream out;
            out << "[vu1:xgkick] src=0x" << std::hex << m_xgkick.sourceAddress << " bytes=" << std::dec << m_xgkick.totalBytes << std::hex << " :";
            for (uint32_t q = 0; q < 6u && q * 16u < m_xgkick.totalBytes; ++q)
            {
                uint32_t w[4];
                std::memcpy(w, m_xgkick.packet.data() + q * 16u, 16u);
                out << " [" << w[0] << " " << w[1] << " " << w[2] << " " << w[3] << "]";
            }
            std::cerr << out.str() << std::dec << std::endl;
        }
    }
    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    resetXgkickState(m_xgkick);
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    return m_fdiv.valid || m_xgkick.active ||
           (m_flagCount | m_efuMask | m_storeMask | m_vfMask | m_viMask | m_accMask) != 0u;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            if (m_vfRegReady[access.reg] <= ready)
                continue;
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t mask = usage->viRead & 0xFFFEu; mask != 0u; mask &= mask - 1u)
        {
            const uint32_t reg = static_cast<uint32_t>(__builtin_ctz(mask));
            if (reg < m_viReady.size())
                ready = std::max(ready, m_viReady[reg]);
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
        m_vfRegReady[lowerWrite.reg] = std::max(m_vfRegReady[lowerWrite.reg], m_cycle + latency);
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
        m_vfRegReady[upperWrite.reg] = std::max(m_vfRegReady[upperWrite.reg], m_cycle + latency);
    }

    for (uint32_t mask = decoded.lowerUsage.viWrite & 0xFFFEu; mask != 0u; mask &= mask - 1u)
    {
        const uint32_t reg = static_cast<uint32_t>(__builtin_ctz(mask));
        if (reg < m_viReady.size())
            m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    // Rate-limited: a corrupted microprogram can raise this tens of thousands of
    // times per second, and each message costs an allocation plus a synchronous
    // stderr write - enough to collapse the frame rate on its own (the logged
    // cascade went from 68 % to 31 %). The stop still happens every time; only
    // the reporting is throttled.
    static std::atomic<uint32_t> s_reports{0};
    const uint32_t report = s_reports.fetch_add(1u, std::memory_order_relaxed);
    if (report < 32u)
    {
        RUNTIME_ERROR(
            "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
                  << " reserved " << (upper ? "upper" : "lower")
                  << "] cycle=" << m_cycle
                  << " pc=0x" << std::hex << m_state.pc
                  << " instruction=0x" << instruction
                  << std::dec << '\n');
    }
    else if (report == 32u || (report % 65536u) == 0u)
    {
        RUNTIME_ERROR("[VU" << (m_unit == Unit::VU1 ? "1" : "0") << "] reserved-instruction reports suppressed ("
                            << report << " so far, last pc=0x" << std::hex << m_state.pc << " instruction=0x"
                            << instruction << std::dec << ")\n");
    }
    m_stopRequested = true;
}

// Accumulated in execute()/resume() below and defined further down; this file
// declares it here so those two can reach it.
extern std::atomic<uint64_t> g_vuRunCycles[2];

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    // Recompiled programs assume the default (fast-commit) pipeline model.
    m_compiledEntry = fastCommitEnabled() ? lookupCompiled(vuCode, codeSize, memory, m_state.pc) : nullptr;
    if (m_unit == Unit::VU1)
        (m_compiledEntry ? g_ps2Vu1CompiledCalls : g_ps2Vu1InterpretedCalls).fetch_add(1, std::memory_order_relaxed);
    // The VU's own cycles over this run, accumulated once per call. Read here
    // rather than from inside run(): a pointer to m_cycle there would force the
    // interpreter's hot counter out of a register, which measured five times
    // slower on a replay benchmark. Saturating, because a mid-call reset() would
    // otherwise wrap the difference.
    const uint64_t cycleBefore = m_cycle;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (m_cycle > cycleBefore)
        g_vuRunCycles[m_unit == Unit::VU0 ? 0u : 1u].fetch_add(m_cycle - cycleBefore, std::memory_order_relaxed);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    const uint64_t cycleBefore = m_cycle;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (m_cycle > cycleBefore)
        g_vuRunCycles[m_unit == Unit::VU0 ? 0u : 1u].fetch_add(m_cycle - cycleBefore, std::memory_order_relaxed);
}

// Defined here, read by the perf log.
std::atomic<uint64_t> g_ps2Vu1ExitEnded{0}, g_ps2Vu1ExitBudget{0}, g_ps2Vu1ExitContinue{0};

// Host CPU time spent inside run(), split by unit. VU0 and VU1 share this
// interpreter class but only VU1 has a compiled path (lookupCompiled requires
// Unit::VU1), so a gameplay profile full of VU1Interpreter::* could be either.
// This says which, in host time - the profile cannot, because the hot methods
// are the same code for both.
std::atomic<uint64_t> g_vuRunMicros[2];
std::atomic<uint64_t> g_vuRunCalls[2];
// The VU's own cycle counter, accumulated per run the same way as the host time.
// Two numbers together say what the emulation costs per unit of *emulated* work
// rather than per call, which is what decides whether the VU path is wasteful or
// simply doing a lot: host cycles per VU cycle. The pipeline model issues one
// instruction per cycle, so this is also instructions issued.
std::atomic<uint64_t> g_vuRunCycles[2];

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    const auto vuRunStart = std::chrono::steady_clock::now();
    struct VuRunScope
    {
        std::chrono::steady_clock::time_point start;
        uint32_t index;
        ~VuRunScope()
        {
            const uint64_t micros = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            g_vuRunMicros[index].fetch_add(micros, std::memory_order_relaxed);
            g_vuRunCalls[index].fetch_add(1u, std::memory_order_relaxed);
        }
    } vuRunScope{vuRunStart, static_cast<uint32_t>(m_unit == Unit::VU0 ? 0u : 1u)};
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    static const std::string failureDirectory = []
    {
        const char *value = std::getenv("PS2X_VU_FAILURE_DIR");
        return value ? std::string(value) : std::string();
    }();
    thread_local std::array<VuFailureInput, 2> failureInputs;
    VuFailureInput &failureInput = failureInputs[m_unit == Unit::VU1 ? 1 : 0];
    const bool captureFailure = !failureDirectory.empty() && !failureInput.captured;
    const bool resumed = s_vuRunIsResume;
    const bool compiledInput = m_compiledEntry != nullptr;
    const uint64_t inputCycle = m_cycle;
    if (captureFailure)
    {
        failureInput.state = m_state;
        failureInput.code.assign(vuCode, vuCode + codeSize);
        failureInput.data.assign(vuData, vuData + dataSize);
    }

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    // Timing-neutral first-failure ring (PS2X_VU_RING): one state memcpy per
    // run, no allocation, so it does not suppress the intermittent cascade.
    static const bool s_vuRing = std::getenv("PS2X_VU_RING") != nullptr;
    const bool ringEnabled = s_vuRing && !s_vuRunRingDumped;
    const uint32_t entryPc = m_state.pc;
    VuRunRecord *ringSlot = nullptr;
    if (ringEnabled)
    {
        ringSlot = &s_vuRunRing[s_vuRunRingNext];
        s_vuRunRingNext = (s_vuRunRingNext + 1u) % kVuRunRing;
        ringSlot->state = m_state;
        ringSlot->enteredCycle = inputCycle;
        ringSlot->exitCycle = 0u;
        ringSlot->exitKind = 0u;
    }
    bool programEnded = false;
    // Microcode cannot change while a program runs (MPG needs the VIF, which
    // waits for us), so validate the decode cache once instead of per pair.
    DecodedInstructionPair uncachedDecoded{};
    if (m_compiledEntry)
    {
        const VU1CompiledFn compiled = m_compiledEntry;
        m_compiledEntry = nullptr;
        const uint64_t before = m_cycle;
        const uint32_t entryPcForStats = m_state.pc;
        static const bool s_timeStats = std::getenv("PS2X_VU1_COMPILED_STATS") != nullptr;
        const uint64_t tscStart = s_timeStats ? __builtin_ia32_rdtsc() : 0u;
        const int exit = compiled(*this, budgetEnd);
        static uint64_t s_tsc = 0;
        if (s_timeStats)
            s_tsc += __builtin_ia32_rdtsc() - tscStart;
        // Which way the compiled program left. If `continue`/`budget` dominate,
        // the interpreter loop below runs the rest of the program - at ~10x the
        // cost of the compiled path (measured with tools/vu1_replay) - which is
        // what a gameplay profile showing VU1Interpreter::* but no vu1p_* means.
        (exit == VU1CompiledEnded ? g_ps2Vu1ExitEnded : exit == VU1CompiledBudget ? g_ps2Vu1ExitBudget
                                                                                   : g_ps2Vu1ExitContinue)
            .fetch_add(1u, std::memory_order_relaxed);
        if (exit == VU1CompiledEnded)
            programEnded = true;
        static const bool s_stats = std::getenv("PS2X_VU1_COMPILED_STATS") != nullptr;
        if (s_stats)
        {
            static uint64_t s_calls = 0, s_cycles = 0, s_exits[3] = {0, 0, 0};
            ++s_calls;
            s_cycles += m_cycle - before;
            ++s_exits[exit & 3];
            static int s_logged = 0;
            if (exit == VU1CompiledContinue && s_logged++ < 12)
                std::cerr << "[vu1:continue] entry=0x" << std::hex << entryPcForStats << " exitPc=0x" << m_state.pc << std::dec << std::endl;
            if ((s_calls & 1023u) == 0u)
                std::cerr << "[vu1:compiled] calls=" << s_calls << " tsc/call=" << s_tsc / s_calls << " cycles/call=" << s_cycles / s_calls << " ended=" << s_exits[0]
                          << " budget=" << s_exits[1] << " continue=" << s_exits[2] << " lastExitPc=0x" << std::hex << m_state.pc
                          << std::dec << std::endl;
        }
    }
    // Decode cache for the interpreter loop (skipped when a compiled program
    // ran to its end).
    const DecodedInstructionPair *decodedCache = nullptr;
    if (!programEnded && memory != nullptr &&
        ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
         (m_unit == Unit::VU0 && vuCode == memory->getVU0Code())))
    {
        (void)getDecodedInstructionPairForPc(vuCode, codeSize, memory, 0u);
        decodedCache = m_decodedCodeCache.data();
    }
    while (!programEnded && m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
            break;

        const uint32_t pairIndex = m_state.pc / 8u;
        const bool cachedPair = decodedCache != nullptr && (m_state.pc & 7u) == 0u && pairIndex < kMaxDecodedPairs;
        if (!cachedPair)
            uncachedDecoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        const DecodedInstructionPair &decoded = cachedPair ? decodedCache[pairIndex] : uncachedDecoded;
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        if (const uint32_t viMask = decoded.lowerUsage.viWrite & 0xFFFEu; viMask != 0u)
        {
            writtenVi = static_cast<uint8_t>(__builtin_ctz(viMask));
            oldVi = m_state.vi[writtenVi];
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        const bool queueWrites = !fastCommitEnabled();
        if (queueWrites && hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (queueWrites && hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (queueWrites && decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        if (fastCommitEnabled())
        {
            // Readers stall on m_vfReady/m_accReady until a write lands, so they
            // always observe the new value: keep it in place and retire older
            // in-flight writes to the same lanes.
            auto retireVf = [&](const VfAccess &write)
            {
                const uint64_t sequence = ++m_nextWriteSequence;
                for (uint32_t component = 0; component < 4u; ++component)
                    if ((write.lanes & laneForComponent(component)) != 0u)
                        m_vfLatestWrite[write.reg][component] = sequence;
            };
            if (hasUpperWrite)
                retireVf(upperWrite);
            if (hasDistinctLowerWrite)
                retireVf(lowerWrite);
            if (decoded.upperUsage.accWrite != 0u)
            {
                const uint64_t sequence = ++m_nextWriteSequence;
                for (uint32_t component = 0; component < 4u; ++component)
                    if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
                        m_accLatestWrite[component] = sequence;
            }
        }
        else
        {
            if (hasUpperWrite)
            {
                std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
                std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
                const uint32_t latency =
                    decoded.upperUsage.vfLatency != 0u
                        ? decoded.upperUsage.vfLatency
                        : decoded.upperUsage.latency;
                queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
            }
            if (hasDistinctLowerWrite)
            {
                std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
                std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
                const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                             ? decoded.lowerUsage.vfLatency
                                             : decoded.lowerUsage.latency;
                queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
            }
            if (decoded.upperUsage.accWrite != 0u)
            {
                std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
                std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
                // ACC is forwarded to the next upper instruction. Its arithmetic
                // flags still use the normal four-cycle FMAC timeline.
                queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                              kAccForwardLatency);
            }
        }
        if (writtenVi != 0u && fastCommitEnabled())
        {
            // Same reasoning as VF/ACC: integer readers stall until the write
            // lands. Branch reads of the previous value keep their own backup.
            m_state.vi[writtenVi] = static_cast<int16_t>(m_state.vi[writtenVi]);
            m_viLatestWrite[writtenVi] = ++m_nextWriteSequence;
        }
        else if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (!programEnded && m_cycle >= budgetEnd)
    {
        static std::atomic<uint32_t> s_budgetLogs{0};
        if (s_budgetLogs.fetch_add(1, std::memory_order_relaxed) < 16u)
            std::cerr << "[vu:budget] unit=" << (m_unit == Unit::VU1 ? 1 : 0) << " pc=0x" << std::hex << m_state.pc
                      << std::dec << " maxCycles=" << maxCycles << " exhausted" << std::endl;
    }
    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_state.cycles = m_cycle;
    if (ringEnabled && (m_stopRequested || (!programEnded && m_cycle >= budgetEnd)))
    {
        // First failure: record how this run left and dump the recent runs.
        if (ringSlot)
        {
            ringSlot->exitCycle = m_cycle;
            ringSlot->exitKind = m_stopRequested ? 2u : 1u;
        }
        dumpVuRunRing(vuRunRingDirectory(), m_unit == Unit::VU1, m_state, m_cycle, maxCycles, compiledInput, resumed);
        std::cerr << "[vu:ring] dumped " << (m_unit == Unit::VU1 ? "vu1" : "vu0") << " ring, pc=0x" << std::hex
                  << m_state.pc << std::dec << " cycle=" << m_cycle << std::endl;
    }

    if (captureFailure && (m_stopRequested || (!programEnded && m_cycle >= budgetEnd)))
    {
        // Capture the earliest budget exhaustion too: later malformed GIFtags
        // can be consequences of an already interrupted microprogram.
        failureInput.captured = true;
        const auto directory = std::filesystem::path(failureDirectory) /
            (m_unit == Unit::VU1 ? "vu1" : "vu0");
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        bool saved = !error;
        if (saved)
        {
            saved &= writeVuFailureFile(directory / "state-before.bin", &failureInput.state, sizeof(VU1State));
            saved &= writeVuFailureFile(directory / "code.bin", failureInput.code.data(), codeSize);
            saved &= writeVuFailureFile(directory / "data-before.bin", failureInput.data.data(), dataSize);
            saved &= writeVuFailureFile(directory / "state-after.bin", &m_state, sizeof(VU1State));
            saved &= writeVuFailureFile(directory / "data-after.bin", vuData, dataSize);
            saved &= writeVuFailureFile(directory / "xgkick.bin", m_xgkick.packet.data(), m_xgkick.copiedBytes);
            if (!resumed && codeSize == PS2_VU1_CODE_SIZE && dataSize == PS2_VU1_DATA_SIZE)
            {
                // Same native ABI as PS2X_VU1_CAPTURE; usable by vu1_replay.
                // Matching a failed capture is reproduction, not correctness.
                std::ofstream replay(directory / "replay.bin", std::ios::binary);
                const uint32_t header[4] = {0x31555643u, failureInput.state.pc,
                                           failureInput.state.top, failureInput.state.itop};
                replay.write(reinterpret_cast<const char *>(header), sizeof(header));
                replay.write(reinterpret_cast<const char *>(&failureInput.state), sizeof(VU1State));
                replay.write(reinterpret_cast<const char *>(failureInput.code.data()), codeSize);
                replay.write(reinterpret_cast<const char *>(failureInput.data.data()), dataSize);
                replay.write(reinterpret_cast<const char *>(&m_state), sizeof(VU1State));
                replay.write(reinterpret_cast<const char *>(vuData), dataSize);
                replay.close();
                saved &= !replay.fail();
            }
            if (memory && memory->getRDRAM())
                saved &= writeVuFailureFile(directory / "ram.bin", memory->getRDRAM(), PS2_RAM_SIZE);
            std::ofstream metadata(directory / "failure.txt");
            metadata << "reason=" << (m_stopRequested ? "reserved-or-pipeline" : "cycle-budget")
                     << " unit=" << (m_unit == Unit::VU1 ? 1 : 0)
                     << " resumed=" << resumed << " compiled=" << compiledInput
                     << " frame=" << g_ps2PresentedFrameCounter.load(std::memory_order_relaxed)
                     << " inputCycle=" << inputCycle << " outputCycle=" << m_cycle
                     << " maxCycles=" << maxCycles << " stateBytes=" << sizeof(VU1State)
                     << " codeBytes=" << codeSize << " dataBytes=" << dataSize << '\n'
                     << std::hex << "inputPc=0x" << failureInput.state.pc << " outputPc=0x" << m_state.pc
                     << " xgkickSource=0x" << m_xgkick.sourceAddress
                     << " xgkickCopied=0x" << m_xgkick.copiedBytes
                     << " xgkickTagEnd=0x" << m_xgkick.currentTagEnd << std::dec
                     << " xgkickActive=" << m_xgkick.active << " xgkickEop=" << m_xgkick.currentTagEop << '\n';
            metadata.close();
            saved &= !metadata.fail();
        }
        std::cerr << "[vu:failure-capture] saved=" << saved << " directory=" << directory << std::endl;
        // This is diagnostic input, not a hardware savestate: a resumed run's
        // pending pipelines and concurrent EE/device state are not serialized.
        failureInput.code.clear();
        failureInput.data.clear();
    }
    static const bool s_vuPcStats = std::getenv("PS2X_VU_PC_STATS") != nullptr;
    if (s_vuPcStats)
    {
        const uint32_t slot = (entryPc >> 3) & (kVuPcSlots - 1u);
        const uint64_t used64 = m_cycle > inputCycle ? m_cycle - inputCycle : 0u;
        const uint32_t used = static_cast<uint32_t>(std::min<uint64_t>(used64, 0xFFFFFFFFull));
        if (used > s_vuPcMax[slot])
            s_vuPcMax[slot] = used;
        ++s_vuPcCount[slot];
        if (++s_vuPcRuns % 200000u == 0u)
            reportVuPcStats();
    }
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
