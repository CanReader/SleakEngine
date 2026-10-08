#include <Core/JobSystem.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <exception>

namespace Sleak {

JobSystem* JobSystem::Instance = nullptr;

JobSystem::JobSystem(uint32_t workerCount) {
    if (workerCount == 0) {
        const uint32_t hw = std::thread::hardware_concurrency();
        workerCount = hw > 1 ? hw - 1 : 1;
    }

    m_workers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i)
        m_workers.emplace_back(&JobSystem::WorkerLoop, this);

    if (!Instance) Instance = this;
    SLEAK_INFO("Job system started with {} workers", workerCount);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (auto& worker : m_workers) worker.join();

    if (Instance == this) Instance = nullptr;
}

uint32_t JobSystem::AllocCounter(uint32_t pending) {
    uint32_t index;
    if (!m_freeCounters.empty()) {
        index = m_freeCounters.back();
        m_freeCounters.pop_back();
    } else {
        index = static_cast<uint32_t>(m_counters.size());
        m_counters.push_back({0, 1});
    }
    m_counters[index].pending = pending;
    return index;
}

void JobSystem::Push(std::function<void()> fn, uint32_t counter) {
    m_queue.push_back({std::move(fn), counter});
}

JobHandle JobSystem::Submit(std::function<void()> fn) {
    JobHandle handle;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        handle.index = AllocCounter(1);
        handle.generation = m_counters[handle.index].generation;
        Push(std::move(fn), handle.index);
    }
    m_wake.notify_one();
    return handle;
}

bool JobSystem::RunOne(std::unique_lock<std::mutex>& lock) {
    if (m_queue.empty()) return false;

    Job job = std::move(m_queue.front());
    m_queue.pop_front();

    lock.unlock();
    try {
        job.fn();
    } catch (const std::exception& e) {
        SLEAK_ERROR("Job threw an exception: {}", e.what());
    } catch (...) {
        SLEAK_ERROR("Job threw an unknown exception");
    }
    job.fn = nullptr;
    lock.lock();

    Counter& counter = m_counters[job.counter];
    if (--counter.pending == 0) {
        if (++counter.generation == 0) counter.generation = 1;
        m_freeCounters.push_back(job.counter);
        m_wake.notify_all();
    }
    return true;
}

void JobSystem::WorkerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        if (RunOne(lock)) continue;
        if (m_stopping) break;
        m_wake.wait(lock);
    }
}

bool JobSystem::IsDone(JobHandle handle) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return handle.generation == 0 || handle.index >= m_counters.size() ||
           m_counters[handle.index].generation != handle.generation;
}

void JobSystem::Wait(JobHandle handle) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (handle.generation == 0 || handle.index >= m_counters.size()) return;

    while (m_counters[handle.index].generation == handle.generation) {
        if (RunOne(lock)) continue;
        m_wake.wait(lock);
    }
    // a push notification may have woken this waiter instead of a worker
    if (!m_queue.empty()) m_wake.notify_one();
}

void JobSystem::ParallelFor(uint32_t count, uint32_t batchSize,
                            const std::function<void(uint32_t)>& fn) {
    if (count == 0) return;
    batchSize = std::max(batchSize, 1u);
    const uint32_t batches = (count - 1) / batchSize + 1;

    JobHandle handle;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        handle.index = AllocCounter(batches);
        handle.generation = m_counters[handle.index].generation;
        for (uint32_t b = 0; b < batches; ++b) {
            const uint32_t begin = b * batchSize;
            const uint32_t end = begin + std::min(batchSize, count - begin);
            Push(
                [&fn, begin, end] {
                    for (uint32_t i = begin; i < end; ++i) fn(i);
                },
                handle.index);
        }
    }
    m_wake.notify_all();
    Wait(handle);
}

}  // namespace Sleak
