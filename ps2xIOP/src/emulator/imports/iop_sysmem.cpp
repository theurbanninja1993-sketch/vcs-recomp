#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>
#include <cstdlib>
#include <sstream>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory
        {
            const uint32_t address = a0 == 2u
                                         ? m_memory.allocate(a1, 16u, a2)
                                         : m_memory.allocate(a1, 16u);
            if (std::getenv("PS2X_IOP_ALLOC_TRACE"))
            {
                std::ostringstream out;
                out << "[IOP sysmem] alloc mode=" << a0 << " size=" << a1
                    << " address=0x" << std::hex << address << " ra=0x" << cpu.gpr[31];
                m_host.log(LogLevel::Info, out.str());
            }
            if (address == 0u)
            {
                m_host.log(LogLevel::Warning,
                           "[IOP] AllocSysMemory failed: mode=" + std::to_string(a0) +
                               " size=" + std::to_string(a1) +
                               " maxFree=" + std::to_string(m_memory.maxFreeMemory()) +
                               " totalFree=" + std::to_string(m_memory.totalFreeMemory()));
            }
            setV0(address);
            return true;
        }
        case 5: // FreeSysMemory
            setV0(m_memory.freeAllocation(a0) ? 0u : 0xFFFFFFFFu);
            return true;
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize
            setV0(m_memory.maxFreeMemory());
            return true;
        case 8: // QueryTotalFreeMemSize
            setV0(m_memory.totalFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->address);
            else
                setV0(0u);
            return true;
        case 10: // QueryBlockSize
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->size);
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 14: // Kprintf
        {
            const std::string format = m_memory.readString(a0, 512u);
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + format);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}
