#include "iop_memory.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_spuMadrAtEnd[0] = m_spuMadrAtEnd[1] = false;
        m_allocations.clear();
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        m_spu2.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (isSpu2Register(phys))
            return static_cast<uint8_t>(const_cast<Spu2 &>(m_spu2).read16(phys & ~1u) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
            return const_cast<Spu2 &>(m_spu2).read16(phys);
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys))
        {
            Spu2 &spu = const_cast<Spu2 &>(m_spu2);
            return static_cast<uint32_t>(spu.read16(phys)) | (static_cast<uint32_t>(spu.read16(phys + 2u)) << 16u);
        }
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (isSpu2Register(phys))
        {
            const uint32_t aligned16 = phys & ~1u;
            const uint32_t shift16 = (phys & 1u) * 8u;
            const uint16_t current16 = m_spu2.read16(aligned16);
            m_spu2.write16(aligned16, static_cast<uint16_t>((current16 & ~(0xFFu << shift16)) | (static_cast<uint32_t>(value) << shift16)));
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
        {
            m_spu2.write16(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys))
        {
            m_spu2.write16(phys, static_cast<uint16_t>(value));
            m_spu2.write16(phys + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        // SPU2 DMA channels (4: core 0, 7: core 1) report AutoDMA progress:
        // libsd derives sceSdBlockTransStatus from MADR and the busy bit.
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t chcr = core == 0u ? kDmaSpu0Chcr : kDmaSpu1Chcr;
            if (address == chcr - 8u && m_spuMadrAtEnd[core] && !m_spu2.autoDmaActive(core))
            {
                // Between AutoDMA blocks: the hardware MADR stays at the end of the
                // finished transfer until libsd programs the next one. Reading the
                // start address back made the movie player's sceSdBlockTransStatus
                // jump 12 KiB backwards, so it stopped refilling and the SPU looped
                // stale audio (stuttering logo/cutscene sound).
                return m_spu2.autoDmaAddress(core);
            }
            if ((address != chcr && address != chcr - 8u) || !m_spu2.autoDmaActive(core))
                continue;
            if (address == chcr - 8u)
                return m_spu2.autoDmaAddress(core);
            const auto stored = m_hardware.find(address);
            return (stored != m_hardware.end() ? stored->second : 0u) | kDmaStart;
        }
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if (address == kDmaSpu0Chcr - 8u || address == kDmaSpu1Chcr - 8u)
            m_spuMadrAtEnd[address == kDmaSpu1Chcr - 8u ? 1u : 0u] = false;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        uint32_t madr = 0u;
        uint32_t bcr = 0u;
        if (const auto it = m_hardware.find(address - 8u); it != m_hardware.end())
            madr = it->second;
        if (const auto it = m_hardware.find(address - 4u); it != m_hardware.end())
            bcr = it->second;
        const uint64_t delay = m_spu2.startDma(secondCore ? 1u : 0u, m_ram.data(), m_ram.size(), madr, bcr, value);
        m_spuMadrAtEnd[secondCore ? 1u : 0u] = m_spu2.autoDmaActive(secondCore ? 1u : 0u);
        // AutoDMA transfers complete as the SPU2 plays them (see Spu2::takeDmaIrqMask).
        if (delay != 0u)
            m_dmaStart = DmaStart{secondCore ? kDmaSpu1Irq : kDmaSpu0Irq, delay};
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        // Reject oversized requests before rounding, which could otherwise wrap.
        if (size > HeapLimit - HeapBase)
            return 0u;
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if ((alignment & (alignment - 1u)) != 0u || alignment > HeapLimit)
            return 0u;
        uint32_t candidate = alignUp(HeapBase, alignment);
        if (fixed)
        {
            candidate = *fixed;
            if (candidate < HeapBase || candidate > HeapLimit || size > HeapLimit - candidate)
                return 0u;
            for (const auto &block : m_allocations)
                if (candidate < block.address + block.size && block.address < candidate + size)
                    return 0u;
        }
        else
        {
            // Keep live blocks sorted and choose the first fitting gap. A freed
            // block becomes available immediately, including below live modules.
            for (const auto &block : m_allocations)
            {
                if (candidate <= block.address && size <= block.address - candidate)
                    break;
                candidate = alignUp(block.address + block.size, alignment);
            }
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        const auto insertion = std::lower_bound(m_allocations.begin(), m_allocations.end(), candidate,
                                                [](const Allocation &block, uint32_t address)
                                                { return block.address < address; });
        m_allocations.insert(insertion, {candidate, size});
        markOwned(candidate, size);
        if (std::getenv("PS2X_IOP_ALLOC_TRACE"))
            std::fprintf(stderr, "[iop:alloc] address=0x%x size=0x%x maxFree=%u totalFree=%u\n",
                         candidate, size, maxFreeMemory(), totalFreeMemory());
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        const uint32_t freedSize = block->size;
        m_allocations.erase(block);
        if (std::getenv("PS2X_IOP_ALLOC_TRACE"))
            std::fprintf(stderr, "[iop:free] address=0x%x size=0x%x maxFree=%u totalFree=%u\n",
                         address, freedSize, maxFreeMemory(), totalFreeMemory());
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        uint32_t cursor = HeapBase;
        uint32_t largest = 0u;
        for (const auto &block : m_allocations)
        {
            largest = std::max(largest, block.address - cursor);
            cursor = block.address + block.size;
        }
        return std::max(largest, HeapLimit - cursor);
    }

    uint32_t IopMemory::totalFreeMemory() const
    {
        uint32_t free = HeapLimit - HeapBase;
        for (const auto &block : m_allocations)
            free -= block.size;
        return free;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
