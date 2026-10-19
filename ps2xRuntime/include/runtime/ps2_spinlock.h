#ifndef PS2_SPINLOCK_H
#define PS2_SPINLOCK_H

#include <atomic>
#include <cstdint>
#include <thread>

// Recursive spinlock for the locks taken on the emulation's hot paths - the GS
// registers, the GIF arbiter, SIF RPC and the IOP call gate. They are taken
// millions of times a second while their critical sections are a handful of
// stores and contention is rare (the renderer and the settings UI mostly), and
// a std::recursive_mutex costs a full pthread lock/unlock every time - which
// showed up as 14% of a sampled profile. An atomic owner plus a depth is
// equivalent for that shape and far cheaper.
class Ps2SpinLock
{
public:
    void lock()
    {
        const uintptr_t self = currentThreadToken();
        if (m_owner.load(std::memory_order_relaxed) == self)
        {
            ++m_depth;
            return;
        }
        while (m_owner.exchange(self, std::memory_order_acquire) != 0u)
            while (m_owner.load(std::memory_order_relaxed) != 0u)
                std::this_thread::yield();
        m_depth = 1;
    }

    void unlock()
    {
        if (--m_depth == 0u)
            m_owner.store(0u, std::memory_order_release);
    }

private:
    static uintptr_t currentThreadToken()
    {
        static std::atomic<uintptr_t> s_next{1u};
        static thread_local const uintptr_t s_token = s_next.fetch_add(1u, std::memory_order_relaxed);
        return s_token;
    }

    std::atomic<uintptr_t> m_owner{0u};
    uint32_t m_depth = 0u; // only the owner touches this
};

#endif
