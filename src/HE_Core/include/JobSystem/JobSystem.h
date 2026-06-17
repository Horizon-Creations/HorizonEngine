#pragma once
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed-size thread pool. Workers pull tasks from a shared queue.
class ThreadPool {
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
ThreadPool& globalPool();

// Distribute [0, count) invocations of f across the global pool and block until
// all finish. f(i) is called exactly once for each i in [0, count).
template<typename F>
void parallel_for(size_t count, F&& f)
{
    if (count == 0) return;
    if (count == 1) { f(size_t{0}); return; }

    ThreadPool& pool = globalPool();
    std::vector<std::future<void>> futures;
    futures.reserve(count);
    for (size_t i = 0; i < count; ++i)
        futures.push_back(pool.submit([&f, i]{ f(i); }));
    for (auto& fut : futures)
        fut.get();
}
