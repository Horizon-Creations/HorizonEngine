#pragma once
#include "Types/Defines.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>   // no longer used here; kept for includers that got it from us
#include <thread>
#include <vector>

class ThreadPool;

// ─── Job planning: priorities, dependencies, cancellation ────────────────────
//
// The pool used to be one FIFO behind a mutex. Asset loads, thumbnails, content
// sync and the frame's parallel_for helpers all stood in the same line, so a
// burst of streaming queued in front of a frame left the frame computing alone
// (world-streaming baseline, Thema 153 §4.1). Three things were missing, and are
// what this section adds:
//   - a PRIORITY per job, so frame work overtakes background work in the queue,
//     plus a cap on how many workers background work may occupy at once;
//   - DEPENDENCIES, so "read, then decode, then upload" is one submission and
//     not a chain of callbacks that each re-enter the pool by hand;
//   - CANCELLATION, so a load nobody wants any more (a zone unloaded before its
//     meshes arrived, a chunk the camera has left) never starts.
// submit()/post()/parallel_for keep their signatures and behaviour; the new
// entry point is ThreadPool::schedule().
namespace HE {

// Queue order. A worker always takes the oldest job of the highest priority it
// is allowed to run; within one priority the queue stays FIFO. A priority only
// reorders what is WAITING — a job that is already running is never preempted,
// which is why Low additionally has a concurrency cap (ThreadPool::
// setConcurrencyLimit): without it ten 50 ms pak reads still occupy all ten
// workers and the frame's helpers find nobody to pick them up.
enum class JobPriority : uint8_t
{
    High   = 0,   // someone is blocked on it right now: parallel_for helpers
    Normal = 1,   // default of submit()/post()/schedule()
    Low    = 2,   // background streaming: asset reads, thumbnails
};
inline constexpr size_t kJobPriorityCount = 3;

enum class JobStatus : uint8_t
{
    Waiting,     // dependencies not finished yet — parked, not in any queue
    Queued,      // in its priority's queue
    Running,
    Done,
    Failed,      // the body threw; JobHandle::wait() rethrows
    Cancelled,   // never ran: cancelled, stale, or a dependency did not succeed
};

namespace detail { struct CancelState; struct JobState; struct PoolCore; }

// A shared flag that marks work as no longer wanted. Cheap to copy (one
// shared_ptr); every copy sees the same flag. Cancelling is sticky.
//
// A default-constructed token is the "none" token: it can never be cancelled and
// allocates nothing — the default of JobDesc::cancel. Use create() for a real one.
//
// Cancellation is checked, not pushed: the pool looks at the token when a job is
// about to become ready and again right before it starts, and a long body can
// poll cancelled() itself between steps. A job that is already running is never
// interrupted.
class HE_API CancelToken
{
public:
    CancelToken();
    ~CancelToken();
    CancelToken(const CancelToken&);
    CancelToken(CancelToken&&) noexcept;
    CancelToken& operator=(const CancelToken&);
    CancelToken& operator=(CancelToken&&) noexcept;

    static CancelToken create();
    // A token that is cancelled when it OR this one is — e.g. one per chunk under
    // one per region, so the region can drop all its chunks at once while a single
    // chunk can still be dropped on its own. On the none token this is create().
    CancelToken child() const;

    void cancel() const;        // no-op on the none token
    bool cancelled() const;
    bool cancellable() const;   // false for the none token

    // Same flag (copies of one token), not merely the same state.
    bool operator==(const CancelToken& other) const;
    bool operator!=(const CancelToken& other) const { return !(*this == other); }

private:
    std::shared_ptr<detail::CancelState> m_state;
};

// Refers to one job made by ThreadPool::schedule(). Copyable; an empty handle
// (default-constructed) is valid() == false and finished.
class HE_API JobHandle
{
public:
    JobHandle();
    ~JobHandle();
    JobHandle(const JobHandle&);
    JobHandle(JobHandle&&) noexcept;
    JobHandle& operator=(const JobHandle&);
    JobHandle& operator=(JobHandle&&) noexcept;

    bool      valid() const;
    JobStatus status() const;
    bool      finished() const;   // Done, Failed or Cancelled (or empty)

    // Cancel this one job if it has not started: a Waiting or Queued job is
    // finished as Cancelled at once — its dependents with it, and wait() returns.
    // A running job is not interrupted; give it a CancelToken to poll for that.
    void cancel() const;

    // Block until the job has finished. Rethrows the body's exception when it
    // Failed; returns normally when it was Cancelled (check status()).
    //
    // Does not deadlock on a saturated pool: a job that is still QUEUED is taken
    // out of the queue and run on the calling thread, and so is a queued job it
    // (transitively) depends on. Only a job another thread is already running is
    // waited for. So wait() is safe inside a pool job — except on a job that
    // depends on the very job calling wait(), which can never finish.
    void wait() const;

private:
    friend class ::ThreadPool;
    explicit JobHandle(std::shared_ptr<detail::JobState> state);
    std::shared_ptr<detail::JobState> m_state;
};

// Everything schedule() needs besides the body.
struct JobDesc
{
    // Profiler lane label. Static storage (string literal) — see ThreadPool::submit.
    const char*  name     = "Job::Execute";
    JobPriority  priority = JobPriority::Normal;
    // Checked before the job is queued and right before it starts.
    CancelToken  cancel;
    // Optional "is this still wanted?" question, asked on the worker right before
    // the body would start; true finishes the job as Cancelled instead. For work
    // whose relevance changes while it waits — a chunk the camera has meanwhile
    // left. Runs on a pool thread, so it must be thread-safe, cheap, and not throw
    // (a throwing predicate counts as stale).
    std::function<bool()> stale;
    // Jobs that must finish first. If any of them Failed or was Cancelled, this
    // job is Cancelled without running: its input is missing. Handles from another
    // pool work too, as long as that pool outlives the dependency.
    std::vector<JobHandle> after;
    // Called exactly once if the job ends Cancelled without its body having run —
    // so an owner can release whatever it reserved for the job (a coalescing key,
    // a progress slot). Runs on whichever thread finished the job off: a worker,
    // or the thread that called cancel()/wait(). Must not throw.
    std::function<void()> onCancelled;
};

} // namespace HE

// Fixed-size thread pool. Workers pull tasks from three priority queues (see
// HE::JobPriority); within one priority they are FIFO.
class HE_API ThreadPool {
public:
    explicit ThreadPool(size_t threadCount);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // `name` labels the task on the profiler's per-thread timeline. It MUST be a
    // string literal / static storage: the profiler stores the pointer in a span
    // and reads it later, at dump or draw time.
    //
    // It exists because the worker lanes were previously a solid wall of
    // "Job::Execute" — technically a timeline, practically unreadable. A lane that
    // says FrustumCull / ExtractMeshes / SkyEnvBake answers "what is the pool
    // doing"; one that says Job::Execute only answers "something".
    template<typename F>
    std::future<void> submit(F&& f, const char* name = "Job::Execute",
                             HE::JobPriority priority = HE::JobPriority::Normal)
    {
        auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
        std::future<void> fut = task->get_future();
        enqueue([task]{ (*task)(); }, name, priority, 1);
        return fut;
    }

    // Fire-and-forget: enqueue `copies` instances of f under ONE lock, with no
    // packaged_task, future or shared state. That is what parallel_for's helpers
    // need — the per-call allocations of submit() were a measurable share of the
    // frame's allocations (perf audit step 3, B2). f MUST NOT throw: nothing waits
    // on a future here, so an exception reaching the worker loop terminates.
    template<typename F>
    void post(F&& f, const char* name = "Job::Execute", size_t copies = 1,
              HE::JobPriority priority = HE::JobPriority::Normal)
    {
        if (copies == 0) return;
        enqueue(std::function<void()>(std::forward<F>(f)), name, priority, copies);
    }

    // A job with priority, dependencies and cancellation — see HE::JobDesc. The
    // body's exception does not reach the worker loop: it is logged, kept, and
    // rethrown by JobHandle::wait(); dependents are then Cancelled. f must be
    // copyable (it is stored in a std::function).
    template<typename F>
    HE::JobHandle schedule(F&& f, HE::JobDesc desc = {})
    {
        return scheduleFn(std::function<void()>(std::forward<F>(f)), std::move(desc));
    }
    HE::JobHandle scheduleFn(std::function<void()> fn, HE::JobDesc desc);

    // At most `maxRunning` workers run jobs of `priority` at the same time (0 is
    // treated as 1). Default: no cap for High/Normal, max(1, threads - 2) for Low —
    // background streaming keeps two workers free for the frame. Jobs over the cap
    // simply wait in their queue; a wait() on one of them still runs it inline.
    void   setConcurrencyLimit(HE::JobPriority priority, size_t maxRunning);
    size_t concurrencyLimit(HE::JobPriority priority) const;
    // Jobs currently queued at `priority`, including cancelled ones a worker has
    // not yet popped and discarded. For diagnostics and tests.
    size_t queuedCount(HE::JobPriority priority) const;

    size_t threadCount() const { return m_threads.size(); }

private:
    void enqueue(std::function<void()> fn, const char* name, HE::JobPriority priority,
                 size_t copies);

    std::vector<std::thread>              m_threads;
    std::shared_ptr<HE::detail::PoolCore> m_core;   // shared with jobs: see JobSystem.cpp
};

// Process-wide thread pool (hardware_concurrency threads, created on first use).
HE_API ThreadPool& globalPool();

// Default minimum number of indices per parallel_for chunk. Below 2× this a call
// runs inline on the caller and never touches the pool.
//
// Why a floor at all: the perf audit (step 3, B2) found extract + cull fanning 4
// objects out as 27 jobs per frame, each doing ~0.7 µs of work behind a queue
// push, a worker wake-up and a future — and under system load the render thread
// waited 8.7 ms p50 per frame on work worth 0.07 ms. Handing a chunk to another
// thread costs microseconds even when every core is idle; a chunk has to carry
// at least that much work to pay for itself. Cheap bodies (a frustum test, a
// per-object copy) take tens to hundreds of ns per index, so a few hundred
// indices is where splitting starts to win. Bodies that are expensive per index
// (a whole sky-bake row) pass a smaller grain explicitly.
constexpr size_t kParallelForMinGrain = 256;

// How many chunks parallel_for splits `count` indices into, given `workers` pool
// threads. Every chunk holds at least `minGrain` indices (0 is treated as 1), so
// 1 means "run inline". Capped at 4 × (workers + 1): chunks are claimed
// dynamically, so a few more chunks than threads let the others absorb the rest
// when one thread is preempted mid-chunk, at the cost of one atomic per chunk.
inline size_t parallel_for_chunks(size_t count, size_t minGrain, size_t workers)
{
    const size_t grain = std::max<size_t>(1, minGrain);
    const size_t cap   = 4 * (std::max<size_t>(1, workers) + 1);
    return std::clamp<size_t>(count / grain, 1, cap);
}

// Shared state of one parallel_for call. Lives on the heap (shared_ptr) because
// helper tasks may be dequeued AFTER parallel_for has returned — a late helper
// finds no chunk left to claim and touches nothing but this struct. `body`/`ctx`
// point at the caller's stack and are only followed after a successful claim, and
// every claim completes before the caller can return.
struct ParallelForJob
{
    using Body = void (*)(void* ctx, size_t begin, size_t end);

    Body   body   = nullptr;
    void*  ctx    = nullptr;
    size_t count  = 0;
    size_t chunks = 0;

    std::atomic<size_t> next{ 0 };        // next chunk to claim
    std::atomic<size_t> remaining{ 0 };   // chunks not yet finished
    std::atomic<bool>   failed{ false };  // set once, by the thread that stores `error`
    std::exception_ptr  error;
    std::mutex              doneMutex;
    std::condition_variable doneCv;
};

// Claim and run chunks until none are left. Never throws: the first exception is
// kept in job.error and later chunks are skipped (still counted as finished).
HE_API void parallel_for_run_chunks(ParallelForJob& job);
// Block until every chunk has finished, then rethrow job.error if one was stored.
HE_API void parallel_for_wait(ParallelForJob& job);

// Distribute [0, count) invocations of f across the global pool and block until
// all finish. f(i) is called exactly once for each i in [0, count) — unless f
// throws: then the first exception is rethrown here, after every chunk already
// started has finished, and chunks not yet started are skipped. f is never still
// running on another thread when parallel_for returns or throws.
//
// Work is split into contiguous CHUNKS of at least `minGrain` indices (see
// kParallelForMinGrain) — a call too small for two chunks never leaves the
// calling thread. Chunks are CLAIMED, not assigned: `min(workers, chunks - 1)`
// helper tasks go to the pool, and the caller claims chunks from the same atomic
// counter until none are left. So the caller never sleeps while work of its own
// call is still unstarted; it only waits for chunks a worker is actually running.
// A worker that wakes up late (loaded machine, E-cores) finds nothing to claim
// and costs the caller nothing — and a saturated pool, or a parallel_for nested
// inside a job, degrades to the caller doing everything instead of deadlocking.
//
// The caller helps with ITS OWN chunks only, never with the pool's general queue:
// that queue also carries asset loads, thumbnails and content sync, and a frame
// must not pick up a 50 ms import because it had a microsecond to spare.
//
// Helpers are queued at JobPriority::High: they overtake every waiting asset load
// and thumbnail, and the Low cap keeps workers free to take them.
//
// `name` labels every helper on the profiler's worker lanes (string literal —
// see ThreadPool::submit). Give each call site its own: on the timeline the name
// IS the identity of the work, and the default tells you nothing.
template<typename F>
void parallel_for(size_t count, F&& f, const char* name = "ParallelFor",
                  size_t minGrain = kParallelForMinGrain)
{
    if (count == 0) return;
    if (count / std::max<size_t>(1, minGrain) < 2)
    {
        for (size_t i = 0; i < count; ++i) f(i);
        return;
    }

    ThreadPool& pool = globalPool();
    const size_t workers = std::max<size_t>(1, pool.threadCount());
    const size_t chunks  = parallel_for_chunks(count, minGrain, workers);

    using Fn = std::remove_reference_t<F>;
    auto job    = std::make_shared<ParallelForJob>();
    job->body   = [](void* ctx, size_t begin, size_t end) {
        Fn& fn = *static_cast<Fn*>(ctx);
        for (size_t i = begin; i < end; ++i) fn(i);
    };
    job->ctx    = const_cast<void*>(static_cast<const void*>(std::addressof(f)));
    job->count  = count;
    job->chunks = chunks;
    job->remaining.store(chunks, std::memory_order_relaxed);

    pool.post([job]{ parallel_for_run_chunks(*job); }, name,
              std::min(workers, chunks - 1), HE::JobPriority::High);
    parallel_for_run_chunks(*job);
    parallel_for_wait(*job);
}
