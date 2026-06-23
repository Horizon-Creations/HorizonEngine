#pragma once
#include "Types/Defines.h"
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed-size thread pool. Workers pull tasks from a shared queue.
class HE_API ThreadPool {
public:
    explicit ThreadPool(size_t threadCount);
    ~ThreadPool();

    template<typename F>
    std::future<void> submit(F&& f)
    {
        auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
        std::future<void> fut = task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push([task]{ (*task)(); });
        }
        m_cv.notify_one();
        return fut;
    }

    size_t threadCount() const { return m_threads.size(); }

private:
    std::vector<std::thread>          m_threads;
    std::queue<std::function<void()>> m_queue;
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    bool                              m_stop = false;
};

// Process-wide thread pool (hardware_concurrency threads, created on first use).
HE_API ThreadPool& globalPool();

// Distribute [0, count) invocations of f across the global pool and block until
// all finish. f(i) is called exactly once for each i in [0, count).
//
// Work is split into a handful of contiguous CHUNKS (one task per worker, not one
// task per index) so fine-grained bodies — a single billboard matrix, one frustum
// test — aren't drowned by per-task queue/future overhead. The calling thread runs
// the first chunk itself instead of blocking idle, so N items cost ~N/(workers+1)
// per thread with only `workers` tasks enqueued.
template<typename F>
void parallel_for(size_t count, F&& f)
{
    if (count == 0) return;
    if (count == 1) { f(size_t{0}); return; }

    ThreadPool& pool = globalPool();
    const size_t workers   = std::max<size_t>(1, pool.threadCount());
    const size_t chunks    = std::min(count, workers + 1);   // +1: the caller helps
    const size_t chunkSize = (count + chunks - 1) / chunks;

    std::vector<std::future<void>> futures;
    futures.reserve(chunks - 1);
    for (size_t c = 1; c < chunks; ++c)
    {
        const size_t begin = c * chunkSize;
        if (begin >= count) break;
        const size_t end = std::min(count, begin + chunkSize);
        futures.push_back(pool.submit([&f, begin, end]{
            for (size_t i = begin; i < end; ++i) f(i);
        }));
    }
    // Run the first chunk inline rather than leave the caller idle.
    const size_t end0 = std::min(count, chunkSize);
    for (size_t i = 0; i < end0; ++i) f(i);

    for (auto& fut : futures)
        fut.get();
}
