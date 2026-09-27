#include "JobSystem/JobSystem.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/Profiler.h"
#include <algorithm>
#include <cstdio>
#include <exception>

// ─── ThreadPool ───────────────────────────────────────────────────────────────
ThreadPool::ThreadPool(size_t threadCount)
{
    HE_LOG_INFO(Job, "ThreadPool starting with %zu worker(s)", threadCount);
    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i)
    {
        m_threads.emplace_back([this, i]
        {
            // Named so every log line a worker produces says which worker it was —
            // otherwise concurrent asset streaming and export logs are unreadable.
            char name[16];
            std::snprintf(name, sizeof(name), "Worker-%zu", i);
            HE::Log::setThreadName(name);

            for (;;)
            {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]{ return m_stop || !m_queue.empty(); });
                    if (m_stop && m_queue.empty()) return;
                    task = std::move(m_queue.front());
                    m_queue.pop();
                }
                // Named after the work, not after the mechanism: the profiler's
                // worker lanes used to be a solid wall of "Job::Execute", which is
                // a timeline of the fact that jobs ran and of nothing else.
                HE_PROFILE_SCOPE_DYN(task.name ? task.name : "Job::Execute");
                // An exception escaping a job used to travel through
                // std::packaged_task into the caller's future().get() — or, for
                // fire-and-forget jobs, straight into std::terminate with no clue
                // where it came from. Log it here, on the thread that actually
                // failed, before it goes anywhere.
                try
                {
                    task.fn();
                }
                catch (const std::exception& e)
                {
                    HE_LOG_ERROR(Job, "Job threw std::exception: %s", e.what());
                    throw;
                }
                catch (...)
                {
                    HE_LOG_ERROR(Job, "%s", "Job threw a non-std exception");
                    throw;
                }
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
        if (!m_queue.empty())
            HE_LOG_WARN(Job, "ThreadPool shutting down with %zu queued task(s) still pending",
                        m_queue.size());
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        t.join();
    HE_LOG_INFO(Job, "%s", "ThreadPool stopped");
}

// ─── parallel_for ─────────────────────────────────────────────────────────────
void parallel_for_run_chunks(ParallelForJob& job)
{
    for (;;)
    {
        const size_t c = job.next.fetch_add(1, std::memory_order_relaxed);
        if (c >= job.chunks) return;   // all claimed — a late helper leaves here

        if (!job.failed.load(std::memory_order_relaxed))
        {
            // Balanced split: chunk sizes differ by at most one index, and none is
            // empty because chunks <= count / minGrain.
            const size_t begin = c * job.count / job.chunks;
            const size_t end   = (c + 1) * job.count / job.chunks;
            try
            {
                job.body(job.ctx, begin, end);
            }
            catch (...)
            {
                // Same courtesy as the worker loop: say it on the thread that failed.
                // Then keep it for the caller instead of letting it reach the worker
                // loop, which would rethrow into std::terminate (helpers have no future).
                HE_LOG_ERROR(Job, "%s", "parallel_for body threw; rethrowing on the caller");
                bool expected = false;
                if (job.failed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                    job.error = std::current_exception();
            }
        }

        // acq_rel: the chunk's writes (and `error`) happen-before the caller's
        // acquire load of remaining == 0.
        if (job.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard<std::mutex> lock(job.doneMutex);
            job.doneCv.notify_all();
        }
    }
}

void parallel_for_wait(ParallelForJob& job)
{
    // Only chunks a worker is actively running are left at this point, so they
    // typically end within microseconds: yield briefly before paying for a
    // sleep + wake-up on the condition variable.
    for (int spin = 0; spin < 64 && job.remaining.load(std::memory_order_acquire) != 0; ++spin)
        std::this_thread::yield();

    if (job.remaining.load(std::memory_order_acquire) != 0)
    {
        std::unique_lock<std::mutex> lock(job.doneMutex);
        job.doneCv.wait(lock, [&]{ return job.remaining.load(std::memory_order_acquire) == 0; });
    }

    if (job.error)
        std::rethrow_exception(job.error);
}

// ─── globalPool ───────────────────────────────────────────────────────────────
ThreadPool& globalPool()
{
    static ThreadPool pool(std::max<size_t>(1, std::thread::hardware_concurrency()));
    return pool;
}
