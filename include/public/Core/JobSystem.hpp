#ifndef _JOBSYSTEM_HPP_
#define _JOBSYSTEM_HPP_

#include <Core/OSDef.hpp>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Sleak {

/// Refers to a submitted job (or a ParallelFor batch group) that can be
/// waited on. Copyable, and stays valid to query after the job finishes.
/// @ingroup core
struct JobHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
};

/// Fixed pool of worker threads fed by one shared queue.
///
/// Application owns the pool for the lifetime of the process and it is
/// reachable through GetInstance(). Jobs are plain callables. Submit()
/// returns a JobHandle to wait on, ParallelFor() splits an index range
/// into batches, and both waits run queued jobs on the calling thread
/// instead of sleeping, so a job may submit and wait on other jobs.
///
/// Submit, Wait, IsDone and ParallelFor are safe to call from any thread,
/// including from inside a job. Jobs must not touch the GPU, the scene,
/// or anything else that is main-thread only. Keep jobs coarse (a
/// texture decode, a few hundred items of a ParallelFor), every job goes
/// through one mutex.
///
/// @code{.cpp}
/// auto* jobs = Sleak::JobSystem::GetInstance();
/// Sleak::JobHandle h = jobs->Submit([&] { DecodeSomething(); });
/// jobs->ParallelFor(items.size(), 64, [&](uint32_t i) { Work(items[i]); });
/// jobs->Wait(h);
/// @endcode
/// @ingroup core
class ENGINE_API JobSystem {
   public:
    /// Starts workerCount threads, or hardware_concurrency minus one
    /// (at least one) when workerCount is 0.
    explicit JobSystem(uint32_t workerCount = 0);
    /// Runs every job still queued, then joins the workers.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Queues fn to run on a worker.
    JobHandle Submit(std::function<void()> fn);

    /// Returns once the job behind handle has finished, running other
    /// queued jobs on this thread in the meantime.
    void Wait(JobHandle handle);

    /// True when the job behind handle has finished.
    bool IsDone(JobHandle handle) const;

    /// Calls fn(i) for every i in [0, count) in batches of batchSize and
    /// returns when all of them have run. The calling thread helps.
    void ParallelFor(uint32_t count, uint32_t batchSize,
                     const std::function<void(uint32_t)>& fn);

    uint32_t GetWorkerCount() const {
        return static_cast<uint32_t>(m_workers.size());
    }

    /// The pool Application created, or null outside of an Application.
    static JobSystem* GetInstance() { return Instance; }

   private:
    struct Job {
        std::function<void()> fn;
        uint32_t counter;
    };

    struct Counter {
        uint32_t pending = 0;
        uint32_t generation = 0;
    };

    uint32_t AllocCounter(uint32_t pending);
    void Push(std::function<void()> fn, uint32_t counter);
    bool RunOne(std::unique_lock<std::mutex>& lock);
    void WorkerLoop();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::vector<Counter> m_counters;
    std::vector<uint32_t> m_freeCounters;
    std::vector<std::thread> m_workers;
    bool m_stopping = false;

    static JobSystem* Instance;
};

}  // namespace Sleak

#endif  // _JOBSYSTEM_HPP_
