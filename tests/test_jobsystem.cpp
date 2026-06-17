#include "doctest.h"
#include <JobSystem/JobSystem.h>
#include <atomic>
#include <numeric>
#include <vector>

TEST_CASE("parallel_for runs the task for every index exactly once")
{
    const size_t N = 64;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0);

    parallel_for(N, [&](size_t i) { hits[i].fetch_add(1); });

    for (size_t i = 0; i < N; ++i)
        CHECK(hits[i].load() == 1);
}

TEST_CASE("parallel_for handles count == 0 without invoking f")
{
    bool ran = false;
    parallel_for(0, [&](size_t) { ran = true; });
    CHECK_FALSE(ran);
}

TEST_CASE("parallel_for handles count == 1 inline (no pool submission)")
{
    size_t seen = SIZE_MAX;
    parallel_for(1, [&](size_t i) { seen = i; });
    CHECK(seen == 0);
}

TEST_CASE("parallel_for produces the correct sum")
{
    const size_t N = 100;
    std::atomic<long long> sum{0};
    parallel_for(N, [&](size_t i) { sum.fetch_add(static_cast<long long>(i)); });
    // 0 + 1 + … + 99 = 4950
    CHECK(sum.load() == 4950LL);
}

TEST_CASE("ThreadPool executes submitted tasks")
{
    ThreadPool pool(2);
    std::atomic<int> total{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 10; ++i)
        futs.push_back(pool.submit([&total, i]{ total.fetch_add(i); }));
    for (auto& f : futs) f.get();
    CHECK(total.load() == 45); // 0+1+…+9
}

TEST_CASE("globalPool returns the same instance on every call")
{
    ThreadPool& a = globalPool();
    ThreadPool& b = globalPool();
    CHECK(&a == &b);
}

TEST_CASE("globalPool has at least one thread")
{
    CHECK(globalPool().threadCount() >= 1);
}
