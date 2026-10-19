// VU1 microprogram recompiler: translates the programs found in VU1 captures
// (PS2X_VU1_CAPTURE, see tools/vu1_replay.cpp) to C++ functions that run on
// VU1Interpreter's state (src/lib/vu/ps2_vu1_jit.h), plus a registry keyed by
// ps2Vu1ProgramKey.
//
// usage: vu1_recomp <out.cpp> <capture.bin>...
//
// Every pair is decoded with the interpreter's own decoder, so stalls,
// latencies and write tracking match it exactly. Programs using features the
// generator doesn't model (D/T halt bits, branches or E-bits in delay slots,
// reserved instructions) are skipped and stay interpreted.
#define private public // markVU1CodeModified() is a private PS2Memory helper
#include "runtime/ps2_memory.h"
#undef private
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "../src/lib/vu/ps2_vu1_jit.h"
#include "../src/lib/vu/ps2_vu1_detail.h"
#include "ps2_runtime.h"
#include "vu_unit_config.h"

#include <cstdio>
#include <filesystem>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

class VU1Recompiler
{
public:
    using Decoded = VU1Interpreter::DecodedInstructionPair;

    VU1Recompiler(const uint8_t *code, uint32_t startPC, const std::vector<uint32_t> &roots)
        : m_code(code), m_start(startPC), m_roots(roots) {}

    // Returns false (with a reason) when the program can't be compiled.
    bool generate(uint64_t key, std::string &out, std::string &reason)
    {
        ps2Vu1ProgramPairs(m_code, kCodeSize, m_start, m_normal, m_delay, m_roots.data(),
                           static_cast<uint32_t>(m_roots.size()));
        m_normalSet.insert(m_normal.begin(), m_normal.end());
        std::set<uint32_t> delaySet(m_delay.begin(), m_delay.end());
        std::set<uint32_t> all = m_normalSet;
        all.insert(delaySet.begin(), delaySet.end());
        for (uint32_t pc : all)
        {
            const Decoded d = decode(pc);
            if (d.upperUsage.reserved || d.lowerUsage.reserved)
                return fail(reason, pc, "reserved instruction");
            if (d.dBit || d.tBit)
                return fail(reason, pc, "D/T halt bit");
            const bool branch = isBranch(d);
            if (d.eBit && branch)
                return fail(reason, pc, "E-bit on a branch");
        }
        for (uint32_t pc : delaySet)
        {
            const Decoded d = decode(pc);
            if (isBranch(d) || d.eBit)
                return fail(reason, pc, "branch / E-bit in a delay slot");
        }
        // The VI branch-backup is only read by a branch executing right after the
        // pair that wrote a VI (see emitPair). If the program has no branch op at
        // all, the per-instruction clear/record stores are dead weight, so skip
        // them entirely (~1-3 host cycles per instruction).
        m_programHasBranches = false;
        for (uint32_t pc : all)
        {
            if (isBranch(decode(pc)))
            {
                m_programHasBranches = true;
                break;
            }
        }
        m_flagless = true;
        for (uint32_t pc : all)
        {
            const Decoded d = decode(pc);
            const uint32_t op = opHi(d);
            if (op == 0x11u || op == 0x15u)
                m_eagerFlags = true;
            // FSEQ/FSAND/FSOR, FMEQ/FMAND/FMOR read the status / MAC flags.
            if (op == 0x14u || op == 0x15u || op == 0x16u || op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu)
                m_flagless = false;
        }
        static const bool s_keepFlags = kVu0 || std::getenv("VU1_RECOMP_KEEP_FLAGS") != nullptr;
        if (m_eagerFlags || s_keepFlags)
            m_flagless = false;
        m_needFlags.clear();
        m_skipsFlags = m_flagless;
        if (!m_flagless && !m_eagerFlags && !s_keepFlags)
            computeLiveFlagWriters(all, delaySet);

        std::ostringstream o;
        o << "static int " << (kVu0 ? "vu0p_" : "vu1p_") << std::hex << key << std::dec << "(VU1Interpreter &vu, uint64_t budgetEnd)\n{\n"
          << "    using J = VU1Jit;\n    VU1State &S = J::st(vu);\n    uint32_t jumpTarget = 0;\n    uint64_t cyc = J::now(vu);\n"
          << (m_skipsFlags ? "    J::noteFlagless(vu);\n" : "")
          << "    switch (S.pc)\n    {\n    case " << m_start << "u: goto L_" << m_start
          << ";\n    default: return VU1CompiledContinue;\n    }\n";
        // Entry points: reached by jumps (state before them unknown). JR/JALR
        // can enter at the program start, the traced jump targets and call
        // return points; anywhere else leaves compiled code.
        std::set<uint32_t> jumpEntries = {m_start};
        jumpEntries.insert(m_roots.begin(), m_roots.end());
        std::set<uint32_t> entries = jumpEntries;
        for (uint32_t pc : m_normal)
        {
            const Decoded d = decode(pc);
            if (!isBranch(d))
                continue;
            const uint32_t op = opHi(d);
            if (op == 0x21u || op == 0x25u)
            {
                jumpEntries.insert(nextPc(nextPc(pc)));
                entries.insert(nextPc(nextPc(pc)));
            }
            if (op != 0x24u && op != 0x25u)
                entries.insert((pc + 8u + static_cast<uint32_t>(IMM11(d.lower) * 8)) & (kCodeSize - 1u));
        }
        // Forward dataflow over the program's labels: the timing / normalization
        // state at a label is the pessimistic merge of every static
        // predecessor's exit state (jump entries are unknown), iterated to a
        // fixed point.
        std::map<uint32_t, Rel> labelState;
        // The branch-backup valid bit is only observable by a VI branch in
        // the immediately following pair. Track whether a valid backup can
        // reach each label so generated code need not store `false` when it
        // is already known to be false. Jump entries start conservatively:
        // a previous compiled invocation may have left the bit set.
        std::map<uint32_t, bool> labelBranchBackup;
        std::vector<uint32_t> work;
        for (uint32_t pc : jumpEntries)
            if (m_normalSet.count(pc))
            {
                labelState[pc] = Rel::unknown();
                labelBranchBackup[pc] = true;
                work.push_back(pc);
            }
        std::vector<std::pair<uint32_t, History>> successors;
        while (!work.empty())
        {
            const uint32_t pc = work.back();
            work.pop_back();
            History h = labelState[pc].toHistory();
            bool branchBackupMayBeValid = labelBranchBackup[pc];
            std::ostringstream scratch;
            successors.clear();
            (void)emitInstance(scratch, pc, h, &successors, branchBackupMayBeValid);
            for (auto &[next, exitHistory] : successors)
            {
                if (!m_normalSet.count(next))
                    continue;
                const Rel incoming = Rel::fromHistory(exitHistory);
                auto it = labelState.find(next);
                if (it == labelState.end())
                {
                    labelState[next] = incoming;
                    labelBranchBackup[next] = branchBackupMayBeValid;
                    work.push_back(next);
                }
                else
                {
                    Rel merged = it->second;
                    merged.merge(incoming);
                    const bool mergedBranchBackup = labelBranchBackup[next] || branchBackupMayBeValid;
                    if (!(merged == it->second) || mergedBranchBackup != labelBranchBackup[next])
                    {
                        it->second = merged;
                        labelBranchBackup[next] = mergedBranchBackup;
                        work.push_back(next);
                    }
                }
            }
        }

        bool dispatch = false;
        for (uint32_t pc : m_normal)
        {
            const auto it = labelState.find(pc);
            History history = (it != labelState.end() ? it->second : Rel::unknown()).toHistory();
            const auto backupIt = labelBranchBackup.find(pc);
            bool branchBackupMayBeValid = backupIt != labelBranchBackup.end() ? backupIt->second : true;
            o << "L_" << pc << ":\n    {\n";
            dispatch |= emitInstance(o, pc, history, nullptr, branchBackupMayBeValid);
            o << "    }\n";
        }
        if (dispatch)
        {
            o << "L_dispatch:\n    switch (jumpTarget)\n    {\n";
            for (uint32_t label : jumpEntries)
                if (m_normalSet.count(label))
                    o << "    case " << label << "u: goto L_" << label << ";\n";
            o << "    default: return J::leaveAt(vu, cyc, jumpTarget);\n    }\n";
        }
        else
        {
            o << "    (void)jumpTarget;\n";
        }
        o << "}\n\n";
        out = o.str();
        return true;
    }

private:
    static constexpr uint32_t kCodeSize = kVuCodeSize;

    enum class Context
    {
        Normal,
        BranchDelay, // locals `taken`, `target` describe the pending branch
        EbitDelay,
    };

    // Static lower bound on operand readiness along straight-line code. Each
    // pair takes at least one cycle, so a VF/VI write by pair j with latency
    // L can't stall pair k >= j + L. `ready` holds j + L per register lane;
    // after a jump entry, unknown earlier writers (latency <= 4) are bounded
    // by entry + 3. ACC forwards in one cycle and never stalls.
    struct History
    {
        uint32_t index = 0;
        uint32_t vf[32][4] = {};
        uint32_t vi[16] = {};
        // Lanes known to hold PS2-normalized values (FMAC results etc.), whose
        // operand normalization can be skipped.
        bool norm[32][4] = {};
        bool accNorm[4] = {};
        void reset()
        {
            for (auto &reg : norm)
                for (bool &lane : reg)
                    lane = false;
            for (bool &lane : accNorm)
                lane = false;
            for (bool &lane : norm[0])
                lane = true; // VF0 = (0, 0, 0, 1)
            for (auto &reg : vf)
                for (uint32_t &lane : reg)
                    lane = index + 3u;
            for (uint32_t &reg : vi)
                reg = index + 3u;
        }
    };

    // History relative to a label (what the dataflow merges): pairs until a
    // lane may still be in flight (<= 3), and known-normalized lanes.
    struct Rel
    {
        uint8_t vf[32][4] = {};
        uint8_t vi[16] = {};
        bool norm[32][4] = {};
        bool accNorm[4] = {};

        static Rel unknown()
        {
            Rel r;
            for (auto &reg : r.vf)
                for (uint8_t &lane : reg)
                    lane = 3;
            for (uint8_t &reg : r.vi)
                reg = 3;
            for (bool &lane : r.norm[0])
                lane = true;
            return r;
        }
        static Rel fromHistory(const History &h)
        {
            Rel r;
            for (uint32_t reg = 0; reg < 32u; ++reg)
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    r.vf[reg][c] = static_cast<uint8_t>(h.vf[reg][c] > h.index ? std::min<uint32_t>(h.vf[reg][c] - h.index, 3u) : 0u);
                    r.norm[reg][c] = h.norm[reg][c];
                }
            for (uint32_t reg = 0; reg < 16u; ++reg)
                r.vi[reg] = static_cast<uint8_t>(h.vi[reg] > h.index ? std::min<uint32_t>(h.vi[reg] - h.index, 3u) : 0u);
            for (uint32_t c = 0; c < 4u; ++c)
                r.accNorm[c] = h.accNorm[c];
            return r;
        }
        History toHistory() const
        {
            History h;
            h.index = 0;
            for (uint32_t reg = 0; reg < 32u; ++reg)
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    h.vf[reg][c] = vf[reg][c];
                    h.norm[reg][c] = norm[reg][c];
                }
            for (uint32_t reg = 0; reg < 16u; ++reg)
                h.vi[reg] = vi[reg];
            for (uint32_t c = 0; c < 4u; ++c)
                h.accNorm[c] = accNorm[c];
            return h;
        }
        void merge(const Rel &other)
        {
            for (uint32_t reg = 0; reg < 32u; ++reg)
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    vf[reg][c] = std::max(vf[reg][c], other.vf[reg][c]);
                    norm[reg][c] = norm[reg][c] && other.norm[reg][c];
                }
            for (uint32_t reg = 0; reg < 16u; ++reg)
                vi[reg] = std::max(vi[reg], other.vi[reg]);
            for (uint32_t c = 0; c < 4u; ++c)
                accNorm[c] = accNorm[c] && other.accNorm[c];
        }
        bool operator==(const Rel &other) const { return std::memcmp(this, &other, sizeof(Rel)) == 0; }
    };

    const uint8_t *m_code;
    uint32_t m_start;
    std::vector<uint32_t> m_roots;
    std::vector<uint32_t> m_normal, m_delay;
    std::set<uint32_t> m_normalSet;
    bool m_eagerFlags = false; // FSSET/FCSET present

public:
    // No MAC/status flag reader: FMACs queue no flags (MAC/status are then
    // not maintained, which no VU1 program in the capture set can observe).
    bool m_flagless = false;
    // True when the program contains any branch op. Only a branch can read the
    // VI branch-backup, so a branch-free program emits no clear/record stores.
    bool m_programHasBranches = true;

private:
    // Flag liveness (programs with readers): only FMACs whose MAC/status
    // entry a reader can observe queue flags. A reader sees the latest entry
    // committed 4 cycles after its FMAC; every pair takes >= 1 cycle, so the
    // candidates are all writers 1-3 pairs back plus the first writer >= 4
    // pairs back on every path. Program exits (E-bit, JR/JALR) count as
    // readers (the next program or a return point may read right away).
    std::set<uint32_t> m_needFlags; // pairs whose FMAC must queue flags
    bool m_skipsFlags = false;      // some FMAC skips its flags
    bool m_pairFlags = true;        // current pair's FMAC queues flags

    static bool upperWritesFlags(uint32_t upper)
    {
        if ((upper & 0x01E00000u) == 0u) // DEST = 0: no flag entry
            return false;
        const uint32_t funct = upper & 0x3Fu;
        if (funct < 0x3Cu)
            return !((funct >= 0x10u && funct <= 0x17u) || funct == 0x1Du || funct == 0x1Fu || funct == 0x2Bu ||
                     funct == 0x2Fu || funct >= 0x30u);
        const uint32_t op = (funct & 3u) | (((upper >> 6) & 0x1Fu) << 2);
        return !((op >= 0x10u && op <= 0x17u) || op == 0x1Du || op == 0x1Fu || op >= 0x2Fu || op == 0x2Bu);
    }

    void computeLiveFlagWriters(const std::set<uint32_t> &all, const std::set<uint32_t> &delaySet)
    {
        std::map<uint32_t, std::vector<uint32_t>> preds;
        std::vector<uint32_t> exits; // last pairs before leaving the program
        for (uint32_t pc : all)
        {
            const Decoded d = decode(pc);
            const uint32_t next = nextPc(pc);
            if (!delaySet.count(pc) || m_normalSet.count(pc))
            {
                // A pair that starts a delay slot sequence.
                if (isBranch(d) || d.eBit)
                {
                    preds[next].push_back(pc);
                    if (d.eBit)
                        exits.push_back(next);
                    else
                    {
                        const uint32_t op = opHi(d);
                        if (op == 0x24u || op == 0x25u) // JR/JALR: dynamic target
                            exits.push_back(next);
                        else
                        {
                            const uint32_t target = (pc + 8u + static_cast<uint32_t>(IMM11(d.lower) * 8)) & (kCodeSize - 1u);
                            preds[target].push_back(next);
                            if (op != 0x20u && op != 0x21u) // conditional: also falls through
                                preds[nextPc(next)].push_back(next);
                        }
                    }
                }
                else
                {
                    preds[next].push_back(pc);
                }
            }
        }
        std::set<std::pair<uint32_t, uint32_t>> visited;
        std::vector<std::pair<uint32_t, uint32_t>> work; // (pair, distance before the reader)
        auto pushPreds = [&](uint32_t pc, uint32_t distance)
        {
            const auto it = preds.find(pc);
            if (it == preds.end())
                return;
            for (uint32_t p : it->second)
                work.emplace_back(p, std::min(distance, 4u));
        };
        for (uint32_t pc : all)
        {
            const uint32_t op = opHi(decode(pc));
            if (op == 0x14u || op == 0x15u || op == 0x16u || op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu)
                pushPreds(pc, 1u);
        }
        for (uint32_t pc : exits)
            work.emplace_back(pc, 1u);
        while (!work.empty())
        {
            const auto [pc, distance] = work.back();
            work.pop_back();
            if (!visited.insert({pc, distance}).second)
                continue;
            if (upperWritesFlags(decode(pc).upper))
            {
                m_needFlags.insert(pc);
                if (distance >= 4u)
                    continue; // the entry this path's reader sees
            }
            pushPreds(pc, distance + 1u);
        }
        for (uint32_t pc : all)
            if (upperWritesFlags(decode(pc).upper) && !m_needFlags.count(pc))
                m_skipsFlags = true;
        static const bool s_verbose = std::getenv("VU1_RECOMP_VERBOSE") != nullptr;
        if (s_verbose)
            std::fprintf(stderr, "flag liveness: %zu of the FMACs queue flags\n", m_needFlags.size());
    }
    // Normalization tracking for the pair being emitted.
    const History *m_hist = nullptr;
    struct NormWrite
    {
        uint32_t reg = 0;   // 32 = ACC
        uint32_t lanes = 0;
        int kind = 0;       // 0 none, 1 normalized, 2 unknown, 3 copy, 4 rotate-copy
        uint32_t source = 0;
    };
    NormWrite m_upperNorm, m_lowerNorm;
    VU1Interpreter m_decoder{kVuUnit};

    static bool fail(std::string &reason, uint32_t pc, const char *what)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s at 0x%x", what, pc);
        reason = buf;
        return false;
    }

    Decoded decode(uint32_t pc) const { return m_decoder.decodeInstructionPair(m_code, pc); }

    static uint32_t opHi(const Decoded &d) { return d.iBit ? 0xFFu : (d.lower >> 25) & 0x7Fu; }
    static bool isBranch(const Decoded &d)
    {
        const uint32_t op = opHi(d);
        return op == 0x20u || op == 0x21u || op == 0x24u || op == 0x25u || op == 0x28u || op == 0x29u ||
               (op >= 0x2Cu && op <= 0x2Fu);
    }
    static uint32_t nextPc(uint32_t pc) { return pc + 8u >= kCodeSize ? 0u : pc + 8u; }

    // --------------------------------------------------------------- pairs

    // Returns true when it jumps through the shared JR dispatch.
    // `successors` (analysis): static successor labels with the exit state.
    bool emitInstance(std::ostringstream &o, uint32_t pc, History &h, std::vector<std::pair<uint32_t, History>> *successors,
                      bool &branchBackupMayBeValid)
    {
        const Decoded d = decode(pc);
        const bool branch = isBranch(d);
        if (branch)
            o << "    bool taken = false;\n    uint32_t target = 0;\n";
        emitPair(o, pc, d, Context::Normal, h, branchBackupMayBeValid);
        const uint32_t next = nextPc(pc);
        if (branch)
        {
            // Delay slot, then the branch.
            const Decoded ds = decode(next);
            emitPair(o, next, ds, Context::BranchDelay, h, branchBackupMayBeValid);
            const uint32_t op = opHi(d);
            if (op == 0x24u || op == 0x25u)
            {
                o << "    jumpTarget = target;\n    goto L_dispatch;\n";
                return true;
            }
            else
            {
                const int32_t imm = IMM11(d.lower);
                const uint32_t dest = (pc + 8u + static_cast<uint32_t>(imm * 8)) & (kCodeSize - 1u);
                if (successors)
                {
                    successors->emplace_back(dest, h);
                    if (op != 0x20u && op != 0x21u)
                        successors->emplace_back(nextPc(next), h);
                }
                if (op == 0x20u || op == 0x21u)
                    o << "    goto L_" << dest << ";\n";
                else
                    o << "    if (taken) goto L_" << dest << ";\n    goto L_" << nextPc(next) << ";\n";
            }
        }
        else if (d.eBit)
        {
            const Decoded ds = decode(next);
            emitPair(o, next, ds, Context::EbitDelay, h, branchBackupMayBeValid);
            o << "    return J::ended(vu, cyc);\n";
        }
        else
        {
            if (successors)
                successors->emplace_back(next, h);
            o << "    goto L_" << next << ";\n";
        }
        return false;
    }

    void emitPair(std::ostringstream &o, uint32_t pc, const Decoded &d, Context ctx, History &h,
                  bool &branchBackupMayBeValid)
    {
        m_pairFlags = !m_flagless && (m_needFlags.empty() ? !m_skipsFlags : m_needFlags.count(pc) != 0u);
        const uint32_t k = h.index;
        char comment[96];
        std::snprintf(comment, sizeof(comment), "    // 0x%04x: %08x %08x\n", pc, d.upper, d.lower);
        o << comment << "    {\n";
        const std::string exitCode = ctx == Context::BranchDelay
                                         ? "return J::exitInDelaySlot(vu, cyc, " + std::to_string(pc) + "u, taken, target);"
                                         : "return J::exitAt(vu, cyc, " + std::to_string(pc) + "u);";
        // The 65,536-cycle budget (and the reserved-instruction stop it carries)
        // is a guard, not a semantic: checking it on every pair costs a load and
        // a compare per instruction. Check every 16th pair and every branch
        // instead - every loop contains a branch, so no loop can escape it, and
        // the worst overshoot is 15 pairs on a straight path.
        if ((pc & 0x78u) == 0u || isBranch(d))
            o << "        if (J::atBudget(vu, cyc, budgetEnd)) " << exitCode << "\n";

        // Stall until operands are ready (calculatePairReadyCycle).
        std::ostringstream ready;
        for (const auto *usage : {&d.upperUsage, &d.lowerUsage})
        {
            for (uint32_t i = 0; i < usage->vfReadCount; ++i)
            {
                const uint32_t reg = usage->vfRead[i].reg, lanes = usage->vfRead[i].lanes;
                if (reg == 0u || lanes == 0u)
                    continue;
                bool mayStall = false;
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((lanes & VU1Jit::lane(c)) && h.vf[reg][c] > k)
                        mayStall = true;
                if (mayStall)
                    ready << " r = J::vf<" << reg << ", " << lanes << ">(vu, r);";
            }
            for (uint32_t reg = 1; reg < 16u; ++reg)
                if ((usage->viRead & (1u << reg)) && h.vi[reg] > k)
                    ready << " r = J::vi<" << reg << ">(vu, r);";
        }
        const auto &lu = d.lowerUsage;
        if (lu.pipeline == VU1Interpreter::PipelineFdiv || lu.waitQ)
            ready << " r = J::fdiv(vu, r);";
        if (lu.pipeline == VU1Interpreter::PipelineEfu)
            ready << " r = J::efuResource(vu, r);";
        if (lu.waitP)
            ready << " r = J::waitP(vu, r);";
        if (lu.pipeline == VU1Interpreter::PipelineXgkick)
            ready << " r = J::xgkick(vu, cyc, r);";
        if (!ready.str().empty())
            o << "        if (!J::stall(vu, cyc, budgetEnd, [&]() { uint64_t r = cyc;" << ready.str()
              << " return r; })) " << exitCode << "\n";

        uint32_t writtenVi = 0u;
        if (const uint32_t mask = lu.viWrite & 0xFFFEu)
        {
            writtenVi = static_cast<uint32_t>(__builtin_ctz(mask));
            o << "        const int32_t oldVi = S.vi[" << writtenVi << "];\n";
        }

        m_hist = &h;
        m_upperNorm = {};
        m_lowerNorm = {};
        if (d.iBit)
        {
            emitUpper(o, d.upper);
            o << "        S.i = J::N(J::f32(0x" << std::hex << d.lower << std::dec << "u));\n";
        }
        else if (d.upperVfShadowReg != 0u)
        {
            const unsigned reg = d.upperVfShadowReg;
            o << "        {\n            float oldVf[4], upperVf[4];\n            std::memcpy(oldVf, S.vf[" << reg
              << "], 16);\n";
            emitUpper(o, d.upper);
            o << "            std::memcpy(upperVf, S.vf[" << reg << "], 16);\n            std::memcpy(S.vf[" << reg
              << "], oldVf, 16);\n";
            emitLower(o, pc, d.lower, d.upper);
            o << "            std::memcpy(S.vf[" << reg << "], upperVf, 16);\n        }\n";
        }
        else
        {
            emitUpper(o, d.upper);
            emitLower(o, pc, d.lower, d.upper);
        }
        // WORK-005 reverted 2026-10-15 by the lead: the CFG-gated clear (below)
        // was emitted only on paths a compile-time fixpoint judged invalid, but
        // it leaked a valid branch backup on a path the VU capture corpus did not
        // cover, crashing/hanging the game (4/5 abnormal vs 3/3 clean). The
        // unconditional clear is the known-good pre-WORK-005 behaviour.
        o << (m_programHasBranches ? "        J::clearBranchBackup(vu);\n" : "");
        // Keep the (now unused) propagation state consistent for the caller.
        branchBackupMayBeValid = false;
        if (writtenVi)
            o << "        S.vi[" << writtenVi << "] = static_cast<int16_t>(S.vi[" << writtenVi << "]);\n";

        // markPairWrites.
        const auto lw = lu.vfWrite;
        if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
            o << "        J::markVf<" << unsigned(lw.reg) << ", " << unsigned(lw.lanes) << ", "
              << unsigned(lu.vfLatency ? lu.vfLatency : lu.latency) << ">(vu, cyc);\n";
        const auto &uu = d.upperUsage;
        if (uu.vfWrite.reg != 0u)
            o << "        J::markVf<" << unsigned(uu.vfWrite.reg) << ", " << unsigned(uu.vfWrite.lanes) << ", "
              << unsigned(uu.vfLatency ? uu.vfLatency : uu.latency) << ">(vu, cyc);\n";
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if (lu.viWrite & (1u << reg))
                o << "        J::markVi<" << reg << ", " << unsigned(lu.viLatency ? lu.viLatency : lu.latency) << ">(vu, cyc);\n";
        if (uu.accWrite)
            o << "        J::markAcc<" << unsigned(uu.accWrite) << ">(vu, cyc);\n";
        if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
            for (uint32_t c = 0; c < 4u; ++c)
                if (lw.lanes & VU1Jit::lane(c))
                    h.vf[lw.reg][c] = k + (lu.vfLatency ? lu.vfLatency : lu.latency);
        if (uu.vfWrite.reg != 0u)
            for (uint32_t c = 0; c < 4u; ++c)
                if (uu.vfWrite.lanes & VU1Jit::lane(c))
                    h.vf[uu.vfWrite.reg][c] = k + (uu.vfLatency ? uu.vfLatency : uu.latency);
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if (lu.viWrite & (1u << reg))
                h.vi[reg] = k + (lu.viLatency ? lu.viLatency : lu.latency);
        ++h.index;
        // Normalization state after the pair: lower write, then upper (which
        // wins on the same register).
        {
            bool before[32][4];
            std::memcpy(before, h.norm, sizeof(before));
            if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (!(lw.lanes & VU1Jit::lane(c)))
                        continue;
                    if (m_lowerNorm.kind == 3 && m_lowerNorm.reg == lw.reg)
                        h.norm[lw.reg][c] = before[m_lowerNorm.source][c];
                    else if (m_lowerNorm.kind == 4 && m_lowerNorm.reg == lw.reg)
                        h.norm[lw.reg][c] = before[m_lowerNorm.source][(c + 1u) & 3u];
                    else
                        h.norm[lw.reg][c] = false;
                }
            }
            if (m_upperNorm.kind != 0)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (!(m_upperNorm.lanes & VU1Jit::lane(c)))
                        continue;
                    if (m_upperNorm.reg == 32u)
                        h.accNorm[c] = m_upperNorm.kind == 1;
                    else if (m_upperNorm.reg != 0u)
                        h.norm[m_upperNorm.reg][c] = m_upperNorm.kind == 1;
                }
            }
            for (bool &lane : h.norm[0])
                lane = true;
            m_hist = nullptr;
        }
        // The backup is only read by a branch executing right after this pair.
        if (m_programHasBranches && writtenVi && lu.delaysNextBranchRead && nextMayBeBranch(pc, d, ctx))
            o << "        J::recordBranchBackup<" << writtenVi << ">(vu, oldVi);\n";
        branchBackupMayBeValid = writtenVi && lu.delaysNextBranchRead && nextMayBeBranch(pc, d, ctx);
        if (writesVf0(d))
            o << "        S.vf[0][0] = 0.0f; S.vf[0][1] = 0.0f; S.vf[0][2] = 0.0f; S.vf[0][3] = 1.0f;\n";

        // pc and program end (no D/T bits in compiled programs).
        // state.pc is only observed when leaving compiled code; exits set it.
        const uint32_t next = nextPc(pc);
        if (ctx == Context::EbitDelay)
            o << "        S.pc = " << next << "u;\n";
        if (ctx == Context::Normal && d.eBit)
            o << "        S.ebit = true;\n";
        o << (m_eagerFlags ? "        J::endCycleEagerFlags(vu, cyc);\n    }\n" : "        J::endCycle(vu, cyc);\n    }\n");
    }

    // Whether the pair writes VF0 (which then has to be restored).
    bool writesVf0(const Decoded &d) const
    {
        if (m_upperNorm.kind != 0 && m_upperNorm.reg == 0u)
            return true;
        if (d.iBit || d.lower == 0u || d.lower == 0x8000033Cu || FT(d.lower) != 0u)
            return false;
        const uint32_t op = (d.lower >> 25) & 0x7Fu;
        if (op == 0x00u) // LQ
            return true;
        if (op != 0x40u || (d.lower & 0x3Cu) != 0x3Cu)
            return false;
        const uint32_t funct2 = (d.lower & 3u) | ((d.lower >> 4) & 0x7Cu);
        // MOVE MR32 LQI LQD MFIR RNEXT RGET MFP
        return funct2 == 0x30u || funct2 == 0x31u || funct2 == 0x34u || funct2 == 0x36u || funct2 == 0x3Du ||
               funct2 == 0x40u || funct2 == 0x41u || funct2 == 0x64u;
    }

    // Whether a branch can execute right after this pair.
    bool nextMayBeBranch(uint32_t pc, const Decoded &d, Context ctx) const
    {
        if (ctx == Context::BranchDelay)
            return true; // jump targets are unknown here; stay conservative
        if (d.eBit || ctx == Context::EbitDelay)
            return false;
        return isBranch(decode(nextPc(pc)));
    }

    // --------------------------------------------------------------- upper

    std::string vfOperand(uint32_t reg, uint32_t c) const
    {
        const std::string raw = "S.vf[" + std::to_string(reg) + "][" + std::to_string(c) + "]";
        return (m_hist && m_hist->norm[reg][c]) ? raw : "J::N(" + raw + ")";
    }
    std::string vs(uint32_t fs, uint32_t c) const { return vfOperand(fs, c); }
    std::string vt(uint32_t ft, uint32_t c) const { return vfOperand(ft, c); }
    std::string accv(uint32_t c) const
    {
        const std::string raw = "S.acc[" + std::to_string(c) + "]";
        return (m_hist && m_hist->accNorm[c]) ? raw : "J::N(" + raw + ")";
    }

    void emitUpper(std::ostringstream &o, uint32_t instr)
    {
        const uint32_t op = instr & 0x3Fu;
        const uint32_t dest = DEST(instr), fs = FS(instr), ft = FT(instr), fd = FD(instr);
        enum Kind
        {
            Add,
            Sub,
            Madd,
            Msub,
            Mul,
            Max,
            Mini,
            OpMsub,
            OpMula,
            Itof,
            Ftoi,
            Abs,
            Nop,
            Fallback
        };
        Kind kind = Fallback;
        std::string other;   // second operand per lane ("" -> vector vt)
        bool bcOperand = false;
        uint32_t bc = 0;
        bool toAcc = false;
        uint32_t scaleIndex = 0;

        if (op < 0x3Cu)
        {
            if (op <= 0x1Bu && !(op >= 0x10u && op <= 0x17u))
            {
                bcOperand = true;
                bc = op & 3u;
                const uint32_t group = op >> 2;
                kind = group == 0 ? Add : group == 1 ? Sub : group == 2 ? Madd : group == 3 ? Msub : Mul;
            }
            else if (op >= 0x10u && op <= 0x17u)
            {
                bcOperand = true;
                bc = op & 3u;
                kind = op <= 0x13u ? Max : Mini;
            }
            else
            {
                switch (op)
                {
                case 0x1Cu: kind = Mul; other = "q"; break;
                case 0x1Du: kind = Max; other = "i"; break;
                case 0x1Eu: kind = Mul; other = "i"; break;
                case 0x1Fu: kind = Mini; other = "i"; break;
                case 0x20u: kind = Add; other = "q"; break;
                case 0x21u: kind = Madd; other = "q"; break;
                case 0x22u: kind = Add; other = "i"; break;
                case 0x23u: kind = Madd; other = "i"; break;
                case 0x24u: kind = Sub; other = "q"; break;
                case 0x25u: kind = Msub; other = "q"; break;
                case 0x26u: kind = Sub; other = "i"; break;
                case 0x27u: kind = Msub; other = "i"; break;
                case 0x28u: kind = Add; break;
                case 0x29u: kind = Madd; break;
                case 0x2Au: kind = Mul; break;
                case 0x2Bu: kind = Max; break;
                case 0x2Cu: kind = Sub; break;
                case 0x2Du: kind = Msub; break;
                case 0x2Eu: kind = OpMsub; break;
                case 0x2Fu: kind = Mini; break;
                default: kind = Fallback; break;
                }
            }
        }
        else
        {
            const uint32_t sp = (instr & 3u) | ((instr >> 4) & 0x7Cu);
            toAcc = true;
            if (sp <= 0x0Fu || (sp >= 0x18u && sp <= 0x1Bu))
            {
                bcOperand = true;
                bc = sp & 3u;
                const uint32_t group = sp >> 2;
                kind = group == 0 ? Add : group == 1 ? Sub : group == 2 ? Madd : group == 3 ? Msub : Mul;
            }
            else
            {
                switch (sp)
                {
                case 0x10u: case 0x11u: case 0x12u: case 0x13u: kind = Itof; scaleIndex = sp & 3u; toAcc = false; break;
                case 0x14u: case 0x15u: case 0x16u: case 0x17u: kind = Ftoi; scaleIndex = sp & 3u; toAcc = false; break;
                case 0x1Cu: kind = Mul; other = "q"; break;
                case 0x1Du: kind = Abs; toAcc = false; break;
                case 0x1Eu: kind = Mul; other = "i"; break;
                case 0x20u: kind = Add; other = "q"; break;
                case 0x21u: kind = Madd; other = "q"; break;
                case 0x22u: kind = Add; other = "i"; break;
                case 0x23u: kind = Madd; other = "i"; break;
                case 0x24u: kind = Sub; other = "q"; break;
                case 0x25u: kind = Msub; other = "q"; break;
                case 0x26u: kind = Sub; other = "i"; break;
                case 0x27u: kind = Msub; other = "i"; break;
                case 0x28u: kind = Add; break;
                case 0x29u: kind = Madd; break;
                case 0x2Au: kind = Mul; break;
                case 0x2Cu: kind = Sub; break;
                case 0x2Du: kind = Msub; break;
                case 0x2Eu: kind = OpMula; break;
                case 0x2Fu: case 0x30u: kind = Nop; break;
                case 0x1Fu:
                    o << "        J::clip(vu, cyc, S.vf[" << fs << "], S.vf[" << ft << "]);\n";
                    return;
                default: kind = Fallback; break;
                }
            }
        }

        if (kind == Nop)
            return;
        if (kind == Fallback)
        {
            o << "        J::execUpper(vu, cyc, 0x" << std::hex << instr << std::dec << "u);\n";
            m_upperNorm = {fd, 0xFu, 2, 0}; // (reserved ops are refused; be conservative)
            return;
        }
        m_upperNorm = {toAcc ? 32u : ((kind == Itof || kind == Ftoi || kind == Abs) ? ft : fd), dest,
                       kind == Ftoi ? 2 : 1, 0};

        if (kind == Itof || kind == Ftoi || kind == Abs)
        {
            // Four-lane conversions; ITOF reads raw integer bits.
            static const char *itofDiv[4] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
            static const char *ftoiScale4[4] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
            const std::string raw = "J::load4(S.vf[" + std::to_string(fs) + "])";
            bool norm = m_hist != nullptr;
            for (uint32_t c = 0; c < 4u && norm; ++c)
                if ((dest & VU1Jit::lane(c)) && !m_hist->norm[fs][c])
                    norm = false;
            const std::string value = norm ? raw : "J::N4(" + raw + ")";
            std::string expr = kind == Itof   ? "J::itof4(" + raw + ", " + itofDiv[scaleIndex] + ")"
                               : kind == Ftoi ? "J::ftoi4(" + value + ", " + ftoiScale4[scaleIndex] + ")"
                                              : "J::abs4(" + value + ")";
            o << "        J::store4<" << dest << ">(S.vf[" << ft << "], " << expr << ");\n";
            m_upperNorm = {ft, dest, kind == Ftoi ? 2 : 1, 0};
            return;
        }
        if (kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max || kind == Mini ||
            kind == OpMsub || kind == OpMula)
        {
            emitUpperSimd(o, instr, kind == Add ? 0 : kind == Sub ? 1 : kind == Mul ? 2 : kind == Madd ? 3 : kind == Msub ? 4
                                                    : kind == Max ? 5 : kind == Mini ? 6 : kind == OpMsub ? 7 : 8,
                          bcOperand, bc, other, toAcc);
            return;
        }

        auto operand = [&](uint32_t c) -> std::string
        {
            if (bcOperand)
                return "bc";
            if (other == "q")
                return "J::N(S.q)";
            if (other == "i")
                return "J::N(S.i)";
            return vt(ft, c);
        };

        o << "        {\n";
        if (bcOperand)
            o << "            const float bc = " << vt(ft, bc) << ";\n";
        o << "            float r[4];\n";
        const bool productSum = kind == Madd || kind == Msub || kind == OpMsub;
        if (productSum)
            o << "            uint32_t xs = 0u;\n";
        o << "            uint32_t ez = 0u;\n";
        static constexpr uint32_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint32_t crossRight[4] = {2u, 0u, 1u, 3u};
        static const char *itofScale[4] = {"", " / 16.0f", " / 4096.0f", " / 32768.0f"};
        static const char *ftoiScale[4] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if (!(dest & VU1Jit::lane(c)))
                continue;
            const std::string rc = "r[" + std::to_string(c) + "]";
            switch (kind)
            {
            case Add: o << "            " << rc << " = " << vs(fs, c) << " + " << operand(c) << "; ez |= " << (1u << c) << "u;\n"; break;
            case Sub: o << "            " << rc << " = " << vs(fs, c) << " - " << operand(c) << "; ez |= " << (1u << c) << "u;\n"; break;
            case Mul:
                o << "            { const float a = " << vs(fs, c) << ", b = " << operand(c) << "; " << rc
                  << " = a * b; if (a == 0.0f || b == 0.0f) ez |= " << (1u << c) << "u; }\n";
                break;
            case Madd:
            case Msub:
                // A zero product leaves acc unchanged exactly, so a zero result is exact.
                o << "            { const float a = " << vs(fs, c) << ", b = " << operand(c) << "; " << rc << " = "
                  << accv(c) << (kind == Madd ? " + " : " - ") << "a * b; xs |= J::productFlags(a, b); if (a == 0.0f || b == 0.0f) ez |= "
                  << (1u << c) << "u; }\n";
                break;
            case Max:
            case Mini:
                o << "            { const float a = " << vs(fs, c) << ", b = " << operand(c) << "; " << rc << " = (a "
                  << (kind == Max ? ">" : "<") << " b) ? a : b; }\n";
                break;
            case OpMsub:
                if (c == 3u)
                    o << "            r[3] = 0.0f; ez |= 8u; xs |= J::productFlags(" << vs(fs, 3) << ", " << vt(ft, 3) << ");\n";
                else
                    o << "            { const float a = " << vs(fs, crossLeft[c]) << ", b = " << vt(ft, crossRight[c])
                      << "; " << rc << " = " << accv(c) << " - a * b; xs |= J::productFlags(a, b); if (a == 0.0f || b == 0.0f) ez |= "
                      << (1u << c) << "u; }\n";
                break;
            case OpMula:
                if (c == 3u)
                    o << "            r[3] = 0.0f; ez |= 8u;\n";
                else
                    o << "            { const float a = " << vs(fs, crossLeft[c]) << ", b = " << vt(ft, crossRight[c]) << "; " << rc
                      << " = a * b; if (a == 0.0f || b == 0.0f) ez |= " << (1u << c) << "u; }\n";
                break;
            case Itof:
                o << "            { int32_t iv; std::memcpy(&iv, &S.vf[" << fs << "][" << c << "], 4); " << rc
                  << " = static_cast<float>(iv)" << itofScale[scaleIndex] << "; }\n";
                break;
            case Ftoi:
                o << "            { const int32_t iv = J::floatToInt(" << vs(fs, c) << ", " << ftoiScale[scaleIndex]
                  << "); std::memcpy(&" << rc << ", &iv, 4); }\n";
                break;
            case Abs: o << "            " << rc << " = std::fabs(" << vs(fs, c) << ");\n"; break;
            default: break;
            }
        }
        const bool flags = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == OpMsub ||
                           kind == OpMula;
        const std::string dst = toAcc ? "S.acc" : (kind == Itof || kind == Ftoi || kind == Abs) ? "S.vf[" + std::to_string(ft) + "]"
                                                                                                  : "S.vf[" + std::to_string(fd) + "]";
        if (flags)
            o << "            J::fmacFinish<" << dest << (m_pairFlags ? "" : ", false") << ">(vu, cyc, 0x" << std::hex << instr
              << std::dec << "u, r, " << dst << ", " << (productSum && m_pairFlags ? "xs" : "0u") << ", ez);\n";
        else
            o << "            J::applyDest<" << dest << ">(" << dst << ", r);\n";
        o << "        }\n";
    }

    // Four-lane SSE2 form of the FMAC arithmetic. op: 0 add, 1 sub, 2 mul,
    // 3 madd, 4 msub, 5 max, 6 mini, 7 opmsub, 8 opmula.
    void emitUpperSimd(std::ostringstream &o, uint32_t instr, int op, bool bcOperand, uint32_t bc, const std::string &other,
                       bool toAcc)
    {
        const uint32_t dest = DEST(instr), fs = FS(instr), ft = FT(instr), fd = FD(instr);
        const bool cross = op == 7 || op == 8;
        auto allNorm = [&](uint32_t reg, uint32_t lanes) -> bool
        {
            if (!m_hist)
                return false;
            for (uint32_t c = 0; c < 4u; ++c)
                if ((lanes & VU1Jit::lane(c)) && !m_hist->norm[reg][c])
                    return false;
            return true;
        };
        auto vector = [&](uint32_t reg) -> std::string
        {
            const std::string load = "J::load4(S.vf[" + std::to_string(reg) + "])";
            return allNorm(reg, cross ? 0xFu : dest) ? load : "J::N4(" + load + ")";
        };
        std::string b;
        if (bcOperand)
            b = "_mm_set1_ps(" + vt(ft, bc) + ")";
        else if (other == "q")
            b = "_mm_set1_ps(J::N(S.q))";
        else if (other == "i")
            b = "_mm_set1_ps(J::N(S.i))";
        else
            b = vector(ft);
        o << "        {\n            const __m128 a0 = " << vector(fs) << ";\n            const __m128 b0 = " << b << ";\n";
        if (cross)
            o << "            const __m128 a = _mm_shuffle_ps(a0, a0, _MM_SHUFFLE(3, 0, 2, 1));\n"
                 "            const __m128 b = _mm_shuffle_ps(b0, b0, _MM_SHUFFLE(3, 1, 0, 2));\n";
        else
            o << "            const __m128 a = a0, b = b0;\n";
        if (op == 3 || op == 4 || op == 7)
        {
            bool accNorm = m_hist != nullptr;
            for (uint32_t c = 0; c < 4u && accNorm; ++c)
                if ((dest & VU1Jit::lane(c)) && !m_hist->accNorm[c])
                    accNorm = false;
            o << "            const __m128 acc = " << (accNorm ? "J::load4(S.acc)" : "J::N4(J::load4(S.acc))") << ";\n";
        }
        static const char *exprs[9] = {"_mm_add_ps(a, b)", "_mm_sub_ps(a, b)", "_mm_mul_ps(a, b)",
                                       "_mm_add_ps(acc, _mm_mul_ps(a, b))", "_mm_sub_ps(acc, _mm_mul_ps(a, b))",
                                       "_mm_max_ps(a, b)", "_mm_min_ps(a, b)", "_mm_sub_ps(acc, _mm_mul_ps(a, b))",
                                       "_mm_mul_ps(a, b)"};
        o << "            __m128 r = " << exprs[op] << ";\n";
        if (cross) // w is 0
            o << "            r = _mm_and_ps(r, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));\n";
        const std::string dst = toAcc ? "S.acc" : "S.vf[" + std::to_string(fd) + "]";
        if (op == 5 || op == 6)
        {
            o << "            J::store4<" << dest << ">(" << dst << ", r);\n        }\n";
            m_upperNorm = {toAcc ? 32u : fd, dest, 1, 0};
            return;
        }
        // Exactly-known zeros: sums always, products when an operand is zero.
        std::string ez = op <= 1 ? "15" : "J::zeroOperands(a, b)";
        if (cross)
            ez += " | 8";
        const bool productSum = op == 3 || op == 4 || op == 7;
        o << "            J::fmac4<" << dest << (m_pairFlags ? "" : ", false") << ">(vu, cyc, 0x" << std::hex << instr << std::dec
          << "u, r, " << dst << ", "
          << (productSum && m_pairFlags ? "J::productFlags4<" + std::to_string(dest) + ">(a, b)" : std::string("0u")) << ", " << ez
          << ");\n        }\n";
        m_upperNorm = {toAcc ? 32u : fd, dest, 1, 0};
    }

    // --------------------------------------------------------------- lower

    void fallbackLower(std::ostringstream &o, uint32_t instr, uint32_t upper)
    {
        o << "        J::execLower(vu, cyc, 0x" << std::hex << instr << "u, 0x" << upper << std::dec << "u);\n";
    }

    void emitLower(std::ostringstream &o, uint32_t pc, uint32_t instr, uint32_t upper)
    {
        if (instr == 0u || instr == 0x8000033Cu)
            return;
        const uint32_t op = (instr >> 25) & 0x7Fu;
        const uint32_t dest = (instr >> 21) & 0xFu;
        const int32_t imm = IMM11(instr);
        const uint32_t vfT = FT(instr), vfS = FS(instr), viT = VIT(instr), viS = VIS(instr), viD = VID(instr);
        const uint32_t branchTarget = (pc + 8u + static_cast<uint32_t>(imm * 8)) & (kCodeSize - 1u);
        auto addr = [&](const std::string &base) -> std::string
        {
            return "uint32_t a = (" + base + ") * 16u; a &= (J::dataSize(vu) - 1u);";
        };
        auto load = [&](const std::string &base, uint32_t reg)
        {
            o << "        { " << addr(base) << " if (a + 16u <= J::dataSize(vu)) { float t[4]; std::memcpy(t, J::data(vu) + a, 16); J::applyDest<"
              << dest << ">(S.vf[" << reg << "], t); } }\n";
        };
        auto storeVf = [&](const std::string &base, uint32_t reg)
        {
            o << "        { " << addr(base) << " if (a + 16u <= J::dataSize(vu)) { uint32_t w[4]; std::memcpy(w, S.vf[" << reg
              << "], 16); J::store(vu, cyc, a, w, " << dest << "u); } }\n";
        };
        const std::string imm11 = std::to_string(imm);
        switch (op)
        {
        case 0x00u: // LQ
            load("static_cast<uint32_t>(static_cast<int32_t>(S.vi[" + std::to_string(viS) + "] + (" + imm11 + ")))", vfT);
            return;
        case 0x01u: // SQ
            storeVf("static_cast<uint32_t>(static_cast<int32_t>(S.vi[" + std::to_string(viT) + "] + (" + imm11 + ")))", vfS);
            return;
        case 0x04u: // ILW
        {
            const uint32_t comp = (dest & 8u) ? 0u : (dest & 4u) ? 1u : (dest & 2u) ? 2u : 3u;
            o << "        { " << addr("static_cast<uint32_t>(static_cast<int32_t>(S.vi[" + std::to_string(viS) + "] + (" + imm11 + ")))")
              << " if (a + 16u <= J::dataSize(vu)) { uint32_t v; std::memcpy(&v, J::data(vu) + a + " << comp * 4u << ", 4);";
            if (viT != 0u)
                o << " S.vi[" << viT << "] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));";
            o << " (void)v; } }\n";
            return;
        }
        case 0x05u: // ISW
            o << "        { " << addr("static_cast<uint32_t>(static_cast<int32_t>(S.vi[" + std::to_string(viS) + "] + (" + imm11 + ")))")
              << " if (a + 16u <= J::dataSize(vu)) { const uint32_t v = static_cast<uint16_t>(S.vi[" << viT
              << "] & 0xFFFF); const uint32_t w[4] = {v, v, v, v}; J::store(vu, cyc, a, w, " << dest << "u); } }\n";
            return;
        case 0x08u: // IADDIU
        case 0x09u: // ISUBIU
        {
            const int32_t imm15 = static_cast<int16_t>(static_cast<int16_t>(instr & 0x7FFu) | ((instr >> 10) & 0x7800u));
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int16_t>(S.vi[" << viS << "] " << (op == 0x08u ? "+" : "-")
                  << " (" << imm15 << "));\n";
            return;
        }
        case 0x10u:
            o << "        J::syncFlags(vu, cyc);\n";
            o << "        S.vi[1] = ((S.clip & 0xFFFFFFu) == " << (instr & 0xFFFFFFu) << "u) ? 1 : 0;\n";
            return;
        case 0x12u:
            o << "        J::syncFlags(vu, cyc);\n";
            o << "        S.vi[1] = ((S.clip & " << (instr & 0xFFFFFFu) << "u) != 0u) ? 1 : 0;\n";
            return;
        case 0x13u:
            o << "        J::syncFlags(vu, cyc);\n";
            o << "        S.vi[1] = ((S.clip | " << (instr & 0xFFFFFFu) << "u) == 0xFFFFFFu) ? 1 : 0;\n";
            return;
        case 0x14u:
        case 0x16u:
        case 0x17u:
        {
            const uint32_t imm12 = (((instr >> 21) & 1u) << 11) | (instr & 0x7FFu);
            if (viT == 0u)
                return;
            o << "        J::syncFlags(vu, cyc);\n";
            if (op == 0x14u)
                o << "        S.vi[" << viT << "] = ((S.status & 0xFFFu) == " << imm12 << "u) ? 1 : 0;\n";
            else
                o << "        S.vi[" << viT << "] = static_cast<int32_t>((S.status & 0xFFFu) " << (op == 0x16u ? "&" : "|")
                  << " " << imm12 << "u);\n";
            return;
        }
        case 0x18u:
            o << "        J::syncFlags(vu, cyc);\n";
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = ((S.mac & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" << viS
                  << "]))) ? 1 : 0;\n";
            return;
        case 0x1Au:
        case 0x1Bu:
            o << "        J::syncFlags(vu, cyc);\n";
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int32_t>(S.mac " << (op == 0x1Au ? "&" : "|")
                  << " static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" << viS << "])));\n";
            return;
        case 0x1Cu:
            o << "        J::syncFlags(vu, cyc);\n";
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int32_t>(S.clip & 0x0FFFu);\n";
            return;
        case 0x20u: // B
            o << "        taken = true; target = " << branchTarget << "u;\n";
            return;
        case 0x21u: // BAL
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = " << (pc + 16u) / 8u << ";\n";
            o << "        taken = true; target = " << branchTarget << "u;\n";
            return;
        case 0x24u: // JR
        case 0x25u: // JALR
            o << "        target = (static_cast<uint32_t>(static_cast<uint16_t>(J::branchVi<" << viS << ">(vu))) * 8u) & " << (kCodeSize - 1u) << "u; taken = true;\n";
            if (op == 0x25u && viT != 0u)
                o << "        S.vi[" << viT << "] = " << (pc + 16u) / 8u << ";\n";
            return;
        case 0x28u:
        case 0x29u:
            o << "        if (static_cast<int16_t>(J::branchVi<" << viS << ">(vu)) " << (op == 0x28u ? "==" : "!=")
              << " static_cast<int16_t>(J::branchVi<" << viT << ">(vu))) { taken = true; target = " << branchTarget << "u; }\n";
            return;
        case 0x2Cu:
        case 0x2Du:
        case 0x2Eu:
        case 0x2Fu:
        {
            static const char *cmp[4] = {"< 0", "> 0", "<= 0", ">= 0"};
            o << "        if (static_cast<int16_t>(J::branchVi<" << viS << ">(vu)) " << cmp[op - 0x2Cu]
              << ") { taken = true; target = " << branchTarget << "u; }\n";
            return;
        }
        case 0x40u:
            break;
        default:
            fallbackLower(o, instr, upper);
            return;
        }

        const uint32_t funct = instr & 0x3Fu;
        switch (funct)
        {
        case 0x30u:
        case 0x31u:
            if (viD != 0u)
                o << "        S.vi[" << viD << "] = static_cast<int16_t>(S.vi[" << viS << "] " << (funct == 0x30u ? "+" : "-")
                  << " S.vi[" << viT << "]);\n";
            return;
        case 0x32u:
        {
            const int32_t imm5 = static_cast<int32_t>(((instr >> 6) & 0x1Fu) << 27) >> 27;
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int16_t>(S.vi[" << viS << "] + (" << imm5 << "));\n";
            return;
        }
        case 0x34u:
        case 0x35u:
            if (viD != 0u)
                o << "        S.vi[" << viD << "] = S.vi[" << viS << "] " << (funct == 0x34u ? "&" : "|") << " S.vi[" << viT << "];\n";
            return;
        case 0x3Cu:
        case 0x3Du:
        case 0x3Eu:
        case 0x3Fu:
            break;
        default:
            fallbackLower(o, instr, upper);
            return;
        }

        const uint32_t funct2 = (instr & 3u) | ((instr >> 4) & 0x7Cu);
        switch (funct2)
        {
        case 0x30u: // MOVE
            m_lowerNorm = {vfT, dest, 3, vfS};
            o << "        { float t[4]; std::memcpy(t, S.vf[" << vfS << "], 16); J::applyDest<" << dest << ">(S.vf[" << vfT
              << "], t); }\n";
            return;
        case 0x31u: // MR32
            m_lowerNorm = {vfT, dest, 4, vfS};
            o << "        { const float t[4] = {S.vf[" << vfS << "][1], S.vf[" << vfS << "][2], S.vf[" << vfS << "][3], S.vf[" << vfS
              << "][0]}; J::applyDest<" << dest << ">(S.vf[" << vfT << "], t); }\n";
            return;
        case 0x34u: // LQI
            load("static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" + std::to_string(viS) + "]))", vfT);
            if (viS != 0u)
                o << "        S.vi[" << viS << "] = static_cast<int16_t>(S.vi[" << viS << "] + 1);\n";
            return;
        case 0x35u: // SQI
            storeVf("static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" + std::to_string(viT) + "]))", vfS);
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int16_t>(S.vi[" << viT << "] + 1);\n";
            return;
        case 0x36u: // LQD
            if (viS != 0u)
                o << "        S.vi[" << viS << "] = static_cast<int16_t>(S.vi[" << viS << "] - 1);\n";
            load("static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" + std::to_string(viS) + "]))", vfT);
            return;
        case 0x37u: // SQD
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int16_t>(S.vi[" << viT << "] - 1);\n";
            storeVf("static_cast<uint32_t>(static_cast<uint16_t>(S.vi[" + std::to_string(viT) + "]))", vfS);
            return;
        case 0x3Bu: // WAITQ
        case 0x7Bu: // WAITP
            return;
        case 0x3Cu: // MTIR
            if (viT != 0u)
                o << "        { uint32_t v; std::memcpy(&v, &S.vf[" << vfS << "][" << ((instr >> 21) & 3u) << "], 4); S.vi[" << viT
                  << "] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu)); }\n";
            return;
        case 0x3Du: // MFIR
            o << "        { const int32_t v = static_cast<int16_t>(S.vi[" << viS << "] & 0xFFFF); float t[4]; std::memcpy(&t[0], &v, 4); t[1] = t[0]; t[2] = t[0]; t[3] = t[0]; J::applyDest<"
              << dest << ">(S.vf[" << vfT << "], t); }\n";
            return;
        case 0x64u: // MFP
            o << "        { const float t[4] = {S.p, S.p, S.p, S.p}; J::applyDest<" << dest << ">(S.vf[" << vfT << "], t); }\n";
            return;
        case 0x68u: // XTOP
        case 0x69u: // XITOP
            if (viT != 0u)
                o << "        S.vi[" << viT << "] = static_cast<int32_t>(S." << (funct2 == 0x68u ? "top" : "itop") << " & 0x3FFu);\n";
            return;
        default:
            fallbackLower(o, instr, upper);
            return;
        }
    }
};

namespace
{
    std::set<uint32_t> *g_jumpTargets = nullptr;
    void recordJump(uint32_t target)
    {
        if (g_jumpTargets)
            g_jumpTargets->insert(target);
    }

    struct Program
    {
        uint32_t startPC = 0;
        std::vector<uint8_t> code;
        std::set<uint32_t> jumps;
    };
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <out.cpp> <capture.bin>...\n", argv[0]);
        return 2;
    }
    // Replay every captured MSCAL through the interpreter to learn the
    // JR/JALR targets each program takes (jump tables in VU data memory).
    PS2Memory memory;
    if (!memory.initialize())
        return 1;
    GS gs;
    memory.setGifPacketCallback([](const uint8_t *, uint32_t) {});
    VU1Interpreter vu1(kVuUnit);
    g_ps2Vu1JumpTraceHook = recordJump;

    // One group per (program, whole code image): the code at a program's
    // jump targets depends on what else is loaded.
    std::map<std::pair<uint64_t, uint64_t>, Program> programs;
    std::vector<uint8_t> dataAfter(kVuDataSize);
    for (int arg = 2; arg < argc; ++arg)
    {
        FILE *f = std::fopen(argv[arg], "rb");
        if (!f)
        {
            std::fprintf(stderr, "can't open %s\n", argv[arg]);
            return 1;
        }
        size_t records = 0;
        for (;;)
        {
            uint32_t header[4];
            VU1State before{}, after{};
            const size_t headerBytes = std::fread(header, 1, sizeof(header), f);
            if (headerBytes == 0u && std::feof(f)) break;
            if (headerBytes != sizeof(header) || header[0] != kVuCaptureMagic)
            {
                std::fprintf(stderr, "invalid or truncated VU capture header: %s\n", argv[arg]);
                std::fclose(f);
                return 1;
            }
            if (std::fread(&before, sizeof(before), 1, f) != 1 ||
                std::fread(vuCode(memory), 1, kVuCodeSize, f) != kVuCodeSize ||
                std::fread(vuData(memory), 1, kVuDataSize, f) != kVuDataSize ||
                std::fread(&after, sizeof(after), 1, f) != 1 ||
                std::fread(dataAfter.data(), 1, dataAfter.size(), f) != dataAfter.size())
            {
                std::fprintf(stderr, "truncated VU capture payload: %s\n", argv[arg]);
                std::fclose(f);
                return 1;
            }
            ++records;
            markVuCodeModified(memory);
            const uint32_t startPC = header[1] & (kVuCodeSize - 1u) & ~7u;
            const uint64_t key = ps2Vu1ProgramKey(vuCode(memory), kVuCodeSize, startPC);
            uint64_t imageHash = 1469598103934665603ull;
            for (uint32_t i = 0; i < kVuCodeSize; ++i)
                imageHash = (imageHash ^ vuCode(memory)[i]) * 1099511628211ull;
            Program &program = programs[{key, imageHash}];
            if (program.code.empty())
            {
                program.startPC = startPC;
                program.code.assign(vuCode(memory), vuCode(memory) + kVuCodeSize);
            }
            g_jumpTargets = &program.jumps;
            if constexpr (kVu0) vu1.reset();
            vu1.state() = before;
            vu1.execute(vuCode(memory), kVuCodeSize, vuData(memory), kVuDataSize, gs, &memory,
                        header[1], header[2], header[3], kVuBudget);
            g_jumpTargets = nullptr;
        }
        std::fclose(f);
        std::fprintf(stderr, "%s: %zu records\n", argv[arg], records);
        if (records == 0u) return 1;
    }

    struct Variant
    {
        uint64_t baseKey;
        std::vector<uint32_t> roots;
        std::string text;
        bool flagless = false;
    };
    std::map<uint64_t, Variant> generated; // by full key
    size_t skipped = 0;
    for (const auto &[ids, program] : programs)
    {
        const uint64_t baseKey = ids.first;
        std::vector<uint32_t> roots(program.jumps.begin(), program.jumps.end());
        const uint64_t fullKey = ps2Vu1ProgramKey(program.code.data(), kVuCodeSize, program.startPC, roots.data(),
                                                  static_cast<uint32_t>(roots.size()));
        if (std::getenv("VU1_RECOMP_VERBOSE"))
        {
            std::fprintf(stderr, "group base=%016llx pc=0x%x image=%016llx full=%016llx roots:", static_cast<unsigned long long>(baseKey),
                         program.startPC, static_cast<unsigned long long>(ids.second), static_cast<unsigned long long>(fullKey));
            for (uint32_t root : roots)
                std::fprintf(stderr, " %x", root);
            std::fprintf(stderr, "\n");
        }
        if (generated.count(fullKey))
            continue;
        VU1Recompiler recompiler(program.code.data(), program.startPC, roots);
        std::string text, reason;
        if (!recompiler.generate(fullKey, text, reason))
        {
            std::fprintf(stderr, "skipped %016llx (pc 0x%x): %s\n", static_cast<unsigned long long>(fullKey), program.startPC,
                         reason.c_str());
            ++skipped;
            continue;
        }
        generated.emplace(fullKey, Variant{baseKey, std::move(roots), std::move(text), recompiler.m_flagless});
    }
    std::fprintf(stderr, "%zu program variants compiled, %zu skipped\n", generated.size(), skipped);

    // <outDir>/vu1p_<key>.cpp per program (compiled in parallel) plus the
    // registry; files whose content is unchanged are left alone.
    const std::string dir = argv[1];
    const std::string prefix = kVu0 ? "vu0" : "vu1";
    auto writeIfChanged = [](const std::string &path, const std::string &content)
    {
        if (FILE *in = std::fopen(path.c_str(), "rb"))
        {
            std::string existing;
            char buffer[65536];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
                existing.append(buffer, n);
            std::fclose(in);
            if (existing == content)
                return true;
        }
        FILE *out = std::fopen(path.c_str(), "wb");
        if (!out)
            return false;
        std::fwrite(content.data(), 1, content.size(), out);
        std::fclose(out);
        return true;
    };
    const char *prologue = "// Generated by tools/vu1_recomp.cpp; do not edit.\n"
                           "#include \"ps2_vu1_jit.h\"\n\n"
                           "#include <cmath>\n#include <cstring>\n\n"
                           "#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n";
    std::string registry = prologue;
    for (const auto &[key, variant] : generated)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%llx", static_cast<unsigned long long>(key));
        std::string text = prologue;
        std::string function = variant.text;
        function.replace(0, std::strlen("static int"), "int");
        text += function;
        text += "extern const uint32_t " + prefix + "r_" + std::string(name) + "[] = {";
        for (uint32_t root : variant.roots)
            text += std::to_string(root) + "u, ";
        text += "0u};\n";
        if (!writeIfChanged(dir + "/" + prefix + "p_" + name + ".cpp", text))
            return 1;
        registry += "int " + prefix + "p_" + std::string(name) + "(VU1Interpreter &, uint64_t);\nextern const uint32_t " + prefix + "r_" +
                    std::string(name) + "[];\n";
    }
    registry += kVu0 ? "\nextern const VU1CompiledProgram g_ps2Vu0CompiledPrograms[] = {\n" : "\nextern const VU1CompiledProgram g_ps2Vu1CompiledPrograms[] = {\n";
    for (const auto &[key, variant] : generated)
    {
        char line[256];
        std::snprintf(line, sizeof(line), "    {0x%016llxull, %sp_%llx, %sr_%llx, %zuu, 0x%016llxull, %s},\n",
                      static_cast<unsigned long long>(variant.baseKey), prefix.c_str(), static_cast<unsigned long long>(key),
                      prefix.c_str(), static_cast<unsigned long long>(key), variant.roots.size(), static_cast<unsigned long long>(key),
                      variant.flagless ? "true" : "false");
        registry += line;
    }
    registry += std::string(kVu0 ? "};\nextern const size_t g_ps2Vu0CompiledProgramCount = " : "};\nextern const size_t g_ps2Vu1CompiledProgramCount = ") + std::to_string(generated.size()) + ";\n";
    if (!writeIfChanged(dir + "/" + prefix + "_programs.cpp", registry))
        return 1;
    // Remove programs from earlier runs that are no longer generated.
    std::set<std::string> current;
    for (const auto &[key, variant] : generated)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%sp_%llx.cpp", prefix.c_str(), static_cast<unsigned long long>(key));
        current.insert(name);
    }
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        const std::string file = entry.path().filename().string();
        if (file.rfind(prefix + "p_", 0) == 0 && file.size() > 4 && file.substr(file.size() - 4) == ".cpp" && !current.count(file))
            std::filesystem::remove(entry.path());
    }
    return 0;
}
