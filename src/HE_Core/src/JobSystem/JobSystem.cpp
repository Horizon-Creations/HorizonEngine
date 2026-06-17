#include "JobSystem/JobSystem.h"
#include "Diagnostics/Profiler.h"
#include <algorithm>

// ─── ThreadPool ───────────────────────────────────────────────────────────────
ThreadPool::ThreadPool(size_t threadCount)
{
    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i)
    {
        m_threads.emplace_back([this]
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]{ return m_stop || !m_queue.empty(); });
                    if (m_stop && m_queue.empty()) return;
                    task = std::move(m_queue.front());
                    m_queue.pop();
                }
                HE_PROFILE_SCOPE_N("Job::Execute");
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        t.join();
}

// ─── globalPool ───────────────────────────────────────────────────────────────
ThreadPool& globalPool()
{
    static ThreadPool pool(std::max<size_t>(1, std::thread::hardware_concurrency()));
    return pool;
}
