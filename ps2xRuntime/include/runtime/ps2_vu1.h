#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

class GS;
class PS2Memory;
class VU1Interpreter;

// Recompiled VU1 microprograms (see tools/vu1_recomp.cpp). A program is
// identified by its entry PC and the code reachable from it; its function
// runs on the interpreter's own state and returns one of VU1CompiledExit.
enum VU1CompiledExit : int
{
    VU1CompiledEnded = 0,    // E-bit program end (pipelines still to flush)
    VU1CompiledBudget = 1,   // cycle budget exhausted / stop requested; state.pc resumes
    VU1CompiledContinue = 2, // left compiled code (e.g. JR elsewhere); interpret from state.pc
};
using VU1CompiledFn = int (*)(VU1Interpreter &vu, uint64_t budgetEnd);
struct VU1CompiledProgram
{
    uint64_t key;          // ps2Vu1ProgramKey without extra roots
    VU1CompiledFn run;
    const uint32_t *roots; // JR/JALR targets compiled as extra entry points
    uint32_t rootCount;
    uint64_t fullKey;      // ps2Vu1ProgramKey including the roots
    bool flagless = false; // reads no MAC/status flags, so computes none
};

// Key of the program entered at startPC: entry, plus the address and words of
// every pair reachable from it (and from `roots`) without following JR/JALR.
uint64_t ps2Vu1ProgramKey(const uint8_t *code, uint32_t codeSize, uint32_t startPC,
                          const uint32_t *roots = nullptr, uint32_t rootCount = 0);

// VU1 microprogram calls run compiled / interpreted (performance logging).
extern std::atomic<uint64_t> g_ps2Vu1CompiledCalls, g_ps2Vu1InterpretedCalls;
extern std::atomic<uint64_t> g_ps2Vu0CompiledCalls, g_ps2Vu0InterpretedCalls;
// Compiled-program lookups after a code upload: cheap revalidations / full key recomputations.
extern std::atomic<uint64_t> g_ps2Vu1LookupRevalidations, g_ps2Vu1LookupRecomputes;

// Debug/tooling hook called with every JR/JALR target the interpreter takes.
extern void (*g_ps2Vu1JumpTraceHook)(uint32_t target);

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

private:
    friend struct VU1Jit;
    friend struct VU1PluginHost; // ps2_vu1_programs.cpp: the plugin host table
    friend class VU1Recompiler;

    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    // Earliest readyCycle among queued pipeline entries; commitReadyPipelines
    // returns immediately before then instead of scanning every queue.
    uint64_t m_nextCommitCycle = 0;
    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    // Valid-slot bitmasks mirroring the `valid` flags above, so the per-cycle
    // commit only visits live entries.
    uint32_t m_efuMask = 0, m_storeMask = 0, m_vfMask = 0, m_viMask = 0, m_accMask = 0;
    // The flag pipeline is a FIFO: every entry has the same latency, so
    // entries become ready in issue order.
    uint32_t m_flagHead = 0, m_flagCount = 0;

    FlagPipelineEntry *allocFlagEntry()
    {
        if (m_flagCount == kMaxFlagEntries)
            return nullptr;
        FlagPipelineEntry &entry = m_flagPipeline[(m_flagHead + m_flagCount++) & (kMaxFlagEntries - 1u)];
        entry = {};
        entry.valid = true;
        entry.issueCycle = m_cycle;
        entry.readyCycle = m_cycle + kFmacLatency;
        if (entry.readyCycle < m_nextCommitCycle)
            m_nextCommitCycle = entry.readyCycle;
        return &entry;
    }
    FlagPipelineEntry &flagEntry(uint32_t i) { return m_flagPipeline[(m_flagHead + i) & (kMaxFlagEntries - 1u)]; }
    // Commits ready flag entries; lowers `next` to the first pending one.
    void commitFlags(uint64_t cycle, uint64_t &next)
    {
        while (m_flagCount != 0u)
        {
            FlagPipelineEntry &entry = m_flagPipeline[m_flagHead];
            if (entry.readyCycle > cycle)
            {
                if (entry.readyCycle < next)
                    next = entry.readyCycle;
                return;
            }
            if (entry.writesMac)
                m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writesSticky)
                m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writesClip)
                m_state.clip = entry.clip;
            entry.valid = false;
            m_flagHead = (m_flagHead + 1u) & (kMaxFlagEntries - 1u);
            --m_flagCount;
        }
    }
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 32> m_vfRegReady{}; // upper bound of m_vfReady[reg][*]
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    // Compiled program for the next run() (set by execute()), and the
    // (code generation, entry PC) -> program cache behind it.
    VU1CompiledFn m_compiledEntry = nullptr;
    uint64_t m_compiledGeneration = ~0ull;
    const uint8_t *m_compiledCode = nullptr;
    // Lookups per entry PC: the result for the current code generation, and
    // the code layouts seen before (games cycle through a few), each with the
    // words (pc, lower, upper) its lookup depended on.
    struct CompiledLayout
    {
        std::vector<uint32_t> words;
        VU1CompiledFn fn = nullptr;
    };
    struct CompiledCacheEntry
    {
        uint64_t generation = ~0ull;
        VU1CompiledFn fn = nullptr;
        std::vector<CompiledLayout> layouts; // most recently used first
    };
    std::unordered_map<uint32_t, CompiledCacheEntry> m_compiledCache;
    bool m_ranFlagless = false; // a flagless compiled program ran (MAC/status not maintained)

public:
    bool ranFlaglessProgram() const { return m_ranFlagless; }
    void clearRanFlagless() { m_ranFlagless = false; }
    // The compiled program for the code at startPC, or null (cached per code
    // generation; also used by PS2X_VU1_LEARN to spot uncompiled programs).
    VU1CompiledFn lookupCompiled(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t startPC);

private:

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    DecodedInstructionPair getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    // PS2 operands: denormals read as signed zero, Inf/NaN as signed FLT_MAX.

    float normalizeOperand(float value) const

    {

        uint32_t bits = 0;

        std::memcpy(&bits, &value, sizeof(bits));

        const uint32_t exponent = (bits >> 23) & 0xFFu;

        if (exponent == 0u)

            bits &= 0x80000000u;

        else if (exponent == 0xFFu)

            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;

        else

            return value;

        std::memcpy(&value, &bits, sizeof(value));

        return value;

    }
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
