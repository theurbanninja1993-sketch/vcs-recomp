#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_sysmem.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testSysmemReuseAndQueries()
    {
        NullHost host;
        IopMemory memory;
        IopSysmem sysmem(host, memory);
        const uint32_t capacity = IopMemory::HeapLimit - IopMemory::HeapBase;
        const uint32_t low = memory.allocate(0x40000u);
        const uint32_t middle = memory.allocate(0x40000u);
        const uint32_t high = memory.allocate(capacity - 0x80000u);
        if (!expect(low && middle && high, "could not fill IOP arena") ||
            !expect(memory.maxFreeMemory() == 0u, "full arena reports free memory") ||
            !expect(memory.freeAllocation(low), "could not release low buffer"))
            return false;
        memory.write32(middle, 0x12345678u);
        memory.write32(high, 0x87654321u);
        // Repeated movie-buffer lifetimes must not exhaust the arena or damage
        // modules above the hole. Exercise the guest sysmem import each time.
        IopCpuState cpu{};
        for (uint32_t i = 0; i < 32u; ++i)
        {
            cpu.gpr[4] = 0u;
            cpu.gpr[5] = 0x40000u;
            if (!expect(sysmem.dispatchImport(4u, cpu) && cpu.gpr[2] == low,
                        "AllocSysMemory did not reuse freed movie buffer"))
                return false;
            cpu.gpr[4] = cpu.gpr[2];
            if (!expect(sysmem.dispatchImport(5u, cpu) && cpu.gpr[2] == 0u,
                        "FreeSysMemory failed"))
                return false;
        }
        if (!expect(memory.read32(middle) == 0x12345678u && memory.read32(high) == 0x87654321u,
                    "reusing a hole corrupted a live allocation") ||
            !expect(memory.freeAllocation(high), "could not release high buffer"))
            return false;
        if (!expect(sysmem.dispatchImport(7u, cpu) && cpu.gpr[2] == capacity - 0x80000u,
                    "QueryMaxFreeMemSize did not report largest hole") ||
            !expect(sysmem.dispatchImport(8u, cpu) && cpu.gpr[2] == capacity - 0x40000u,
                    "QueryTotalFreeMemSize did not sum disjoint holes") ||
            !expect(memory.allocate(0xFFFFFFFFu) == 0u && memory.allocate(0xFFFFFFF0u) == 0u,
                    "oversized request wrapped to a small allocation"))
            return false;
        memory.reset();
        // Fixed module reservations and alignment must constrain first-fit reuse.
        const uint32_t reserved = IopMemory::HeapBase + 0x200u;
        if (!expect(memory.allocate(0x100u, 0x100u, reserved) == reserved,
                    "fixed module reservation failed") ||
            !expect(memory.allocate(0x100u, 0x100u, reserved) == 0u,
                    "overlapping reservation succeeded") ||
            !expect(memory.allocate(0x180u, 0x100u) == IopMemory::HeapBase,
                    "aligned low hole was not used") ||
            !expect(memory.allocate(0x100u, 0x100u) == reserved + 0x100u,
                    "aligned allocation overlapped fixed module"))
            return false;
        return true;
    }

    bool testCdvdPendingRead()
    {
        class SectorHost final : public NullHost
        {
        public:
            bool readCdSectors(uint32_t, uint32_t sectors, void *destination) override
            {
                ++reads;
                std::memset(destination, 0x5Au, sectors * 2048u);
                return true;
            }
            unsigned reads = 0;
        } host;
        IopMemory memory;
        IopKernel kernel(memory);
        IopCdvd cdvd(host, memory, kernel);
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11);
        cpu.gpr[5] = 0x1000u;
        if (!expect(cdvd.dispatchImport(50u, cpu), "CDVD interrupt flag query failed"))
            return false;
        const uint32_t flag = cpu.gpr[2];
        cpu = {};
        cpu.gpr[4] = 0x1234u;
        if (!expect(cdvd.dispatchImport(37u, cpu), "CDVD callback registration failed"))
            return false;
        const uint32_t buffer = memory.allocate(5u * 2048u);
        cpu = {};
        cpu.gpr[4] = 0x100u;
        cpu.gpr[5] = 5u;
        cpu.gpr[6] = buffer;
        if (!expect(cdvd.dispatchImport(6u, cpu, 100u) && cpu.gpr[2] == 1u,
                    "CDVD read was not accepted"))
            return false;
        const uint64_t deadline = cdvd.completionCycle();
        const auto callback = cdvd.takeCompletionCallback();
        if (!expect(deadline > 100u && callback && callback->deadlineCycle == deadline,
                    "read callback was not tied to asynchronous completion") ||
            !expect(memory.read8(buffer) == 0x5Au, "host sector data was not copied"))
            return false;
        cpu.gpr[4] = 1u;
        if (!expect(cdvd.dispatchImport(11u, cpu, deadline - 1u) && cpu.gpr[2] == 1u,
                    "poll reported completion before the deadline"))
            return false;
        cpu.gpr[4] = 0x100u;
        if (!expect(cdvd.dispatchImport(6u, cpu, deadline - 1u) && cpu.gpr[2] == 0u && host.reads == 1u,
                    "pending read was replaced by another read"))
            return false;
        cpu = {};
        cpu.gpr[4] = flag;
        cpu.gpr[5] = 0x29u;
        cpu.gpr[6] = 1u;
        if (!expect(kernel.dispatchEventImport(11u, cpu) && static_cast<int32_t>(cpu.gpr[2]) == -418,
                    "CDVD completion event was signaled while pending"))
            return false;
        cdvd.advanceCycles(deadline);
        if (!expect(kernel.dispatchEventImport(11u, cpu) && cpu.gpr[2] == 0u,
                    "CDVD completion event was not signaled at deadline"))
            return false;
        cpu.gpr[4] = 1u;
        if (!expect(cdvd.dispatchImport(11u, cpu, deadline) && cpu.gpr[2] == 0u,
                    "poll did not report completed read"))
            return false;
        cpu = {};
        cpu.gpr[4] = 0x200u;
        cpu.gpr[5] = 5u;
        cpu.gpr[6] = buffer;
        if (!expect(cdvd.dispatchImport(6u, cpu, deadline) && cpu.gpr[2] == 1u,
                    "completed command prevented the next read"))
            return false;
        cdvd.reset();
        return expect(cdvd.completionCycle() == 0u && !cdvd.takeCompletionCallback(),
                      "reset retained a pending read or callback");
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        const bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        std::filesystem::remove_all(root, error);
        return passed;
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }
}

int main()
{
    if (!testSysmemReuseAndQueries() || !testCdvdPendingRead() || !testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testTimrmanPeriodicCallback())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
