#include "ps2_iop_worker.h"

#include "ThreadNaming.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace
{
    // The EE clock and one VBlank of it (294.912 MHz / 59.94 Hz), matching
    // ps2_runtime.cpp: the allowance is counted in EE cycles.
    constexpr uint64_t kEeCyclesPerVBlank = 4'920'120u;

    // How much IOP work to run between checks for posted requests. This is the
    // worst case a game-thread call waits: it posts, then blocks until the
    // worker reaches the queue, so a small slice bounds that wait - but every
    // slice also takes the queue lock, and a sixteenth VBlank meant 3000 of
    // those a second. On Windows that cost more than it bought: the worker fell
    // behind, the EE stalled waiting on the IOP through the SIF, and the whole
    // emulation degraded over a session (lostMs climbing 4s -> 6.7s per 10s,
    // underrun back up to 237k). A quarter VBlank is the value that measured
    // good on the owner's machines.
    constexpr uint64_t kSliceEeCycles = kEeCyclesPerVBlank / 4u;

    // How far the IOP may fall behind before its backlog is dropped. A long
    // stall (a load, a disc read) must not come back as a burst of catch-up
    // work - but the backlog *is* audio, and two VBlanks (33 ms) of slack was
    // less than a busy laptop delays this thread routinely: it lost ~7% of the
    // cutscene audio for good, and the voices fell steadily behind the
    // animation. Twenty VBlanks (~0.33 s) still fits the 0.68 s output ring.
    constexpr uint64_t kMaxLagEeCycles = 20u * kEeCyclesPerVBlank;
}

// Sampled-thread bookkeeping and call counts for the perf log. The PC sampler
// is process-wide (ITIMER_PROF), so without the flag this worker's own futex
// traffic lands in the game thread's profile; the counters say how many
// cross-thread round trips a session is actually paying for.
thread_local bool t_onIopWorker = false;
std::atomic<uint64_t> g_iopWorkerPosts{0};
std::atomic<uint64_t> g_iopWorkerAllows{0};

IopWorker::IopWorker(ps2x::iop::IopSubsystem &iop)
    : m_iop(iop),
      // Off by default while it is being verified: PS2X_IOP_THREAD=1 enables it,
      // PS2X_IOP_THREAD=0 (or unset) keeps the inline path.
      m_enabled([]()
                {
                    const char *env = std::getenv("PS2X_IOP_THREAD");
                    return env != nullptr && std::atoi(env) != 0;
                }())
{
}

IopWorker::~IopWorker()
{
    stop();
}

void IopWorker::start()
{
    if (!m_enabled || m_running)
        return;
    m_stop = false;
    m_running = true;
    m_thread = std::thread([this]() { loop(); });
}

void IopWorker::stop()
{
    if (!m_thread.joinable())
    {
        m_running = false;
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join(); // the loop drains posted work before it exits
    m_running = false;
}

void IopWorker::run(const std::function<void()> &fn)
{
    if (!m_enabled || !m_running)
    {
        fn(); // not in use: run inline, exactly as before
        return;
    }
    std::unique_lock lock(m_mutex);
    g_iopWorkerPosts.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t ticket = ++m_posted;
    m_work.push_back(fn);
    m_wake.notify_one();
    m_done.wait(lock, [this, ticket]() { return m_completed >= ticket; });
}

void IopWorker::addAllowedEeCycles(uint64_t eeCycles)
{
    if (!m_enabled || !m_running)
        return;
    g_iopWorkerAllows.fetch_add(1u, std::memory_order_relaxed);
    std::lock_guard lock(m_mutex);
    m_allowed += eeCycles;
    m_wake.notify_one();
}

void IopWorker::loop()
{
    ThreadNaming::SetCurrentThreadName("IopWorker");
    t_onIopWorker = true;
    std::unique_lock lock(m_mutex);
    // Posted work is the synchronous path - whoever posted it is waiting - so
    // drain it before leaving, even once stopping.
    while (!m_stop || !m_work.empty())
    {
        if (!m_work.empty())
        {
            std::function<void()> fn = std::move(m_work.front());
            m_work.pop_front();
            lock.unlock();
            fn();
            lock.lock();
            ++m_completed;
            m_done.notify_all();
            continue;
        }
        // Then the cycle allowance, in slices.
        const uint64_t iopEe = m_iop.cycles() * 8u;
        if (m_allowed > iopEe)
        {
            if (m_allowed - iopEe > kMaxLagEeCycles)
                m_allowed = iopEe + kMaxLagEeCycles; // drop a long stall's backlog
            const uint64_t give = std::min<uint64_t>(m_allowed - iopEe, kSliceEeCycles);
            lock.unlock();
            m_iop.runEeCycles(give);
            lock.lock();
            continue;
        }
        m_wake.wait(lock);
    }
}
