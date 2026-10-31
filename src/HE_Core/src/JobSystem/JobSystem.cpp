#include "JobSystem/JobSystem.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/Profiler.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <unordered_set>
#include <utility>

// ─── Internals shared by the pool and its jobs ───────────────────────────────
namespace HE::detail {

struct CancelState
{
    std::atomic<bool>            flag{ false };
    std::shared_ptr<CancelState> parent;   // child(): cancelled together with it
};

static bool chainCancelled(const CancelState* s)
{
    for (; s; s = s->parent.get())
        if (s->flag.load(std::memory_order_acquire)) return true;
    return false;
}

// One queue entry. A plain task (submit/post) carries `fn`; a scheduled job
// carries `job`, whose body lives in the JobState. So an entry whose job was
// cancelled meanwhile — or already run inline by a wait() — is simply dropped
// when a worker pops it: the job's status is no longer Queued.
using QueueClock = std::chrono::steady_clock;

struct PoolTask
{
    std::function<void()>     fn;
    const char*               name = nullptr;
    std::shared_ptr<JobState> job;
    QueueClock::time_point    queuedAt{};   // for ThreadPoolStats::Lane::waitNs
};

// One priority's queue: smallest (order, sequence) first. With every order equal
// it is the FIFO the deque was; the heap only costs a few comparisons per push.
class TaskQueue
{
public:
    bool   empty() const { return m_heap.empty(); }
    size_t size()  const { return m_heap.size(); }

    void push(PoolTask&& task, uint32_t order, uint64_t seq)
    {
        m_heap.push_back(Entry{ order, seq, std::move(task) });
        std::push_heap(m_heap.begin(), m_heap.end(), later);
    }
    // (order, sequence) of what pop() would return; the queue must not be empty.
    std::pair<uint32_t, uint64_t> frontKey() const { return { m_heap.front().order, m_heap.front().seq }; }
    PoolTask pop()
    {
        std::pop_heap(m_heap.begin(), m_heap.end(), later);
        PoolTask task = std::move(m_heap.back().task);
        m_heap.pop_back();
        return task;
    }
    // Everything, in no particular order (the destructor's leftovers).
    void takeAll(std::vector<PoolTask>& out)
    {
        for (Entry& e : m_heap) out.push_back(std::move(e.task));
        m_heap.clear();
    }

private:
    struct Entry { uint32_t order; uint64_t seq; PoolTask task; };
    static bool later(const Entry& a, const Entry& b)
    {
        return a.order != b.order ? a.order > b.order : a.seq > b.seq;
    }
    std::vector<Entry> m_heap;
};

// The queues, shared by the ThreadPool and every job it scheduled. A Waiting job
// pushes ITSELF here when its last dependency finishes, and that can happen after
// the ThreadPool object is gone (the dependency ran in a longer-lived pool), so
// the job keeps the core alive and `dead` turns the late push into a
// cancellation instead of a write into a destroyed pool.
struct PoolCore
{
    mutable std::mutex      mutex;
    std::condition_variable cv;
    TaskQueue               queues[kJobKindCount][kJobPriorityCount];
    size_t                  ioRunning = 0;
    size_t                  ioLimit   = 0;
    uint64_t                nextSeq = 0;   // FIFO tie-break within one order key
    size_t                  running[kJobPriorityCount] = {};   // only for capped priorities
    size_t                  limit[kJobPriorityCount]   = {};
    size_t                  threads = 0;
    bool                    stop = false;   // destructor: drain the queues, then exit
    bool                    dead = false;   // workers joined: nothing runs here any more
    // Set when the destructor starts: scheduled Low jobs that have not begun end
    // Cancelled instead of being run to completion on the way out (see runClaimed).
    std::atomic<bool>       draining{ false };

    // ThreadPool::stats(). Atomics, bumped outside the mutex: a worker already
    // takes it once per task and a diagnostic must not make that twice.
    std::atomic<size_t>     active[kJobPriorityCount]    = {};
    std::atomic<uint64_t>   executed[kJobPriorityCount]  = {};
    std::atomic<uint64_t>   busyNs[kJobPriorityCount]    = {};
    std::atomic<uint64_t>   waitNs[kJobPriorityCount]    = {};
    std::atomic<uint64_t>   waitMaxNs[kJobPriorityCount] = {};
    std::atomic<uint64_t>   cancelled[kJobPriorityCount] = {};
    std::atomic<uint64_t>   failed[kJobPriorityCount]    = {};

    // The next task that may start now: the highest priority with one whose limits
    // allow it; within a priority the Cpu and the Io queue compete by (order,
    // sequence), an Io queue that is at its limit does not take part. Caller holds mutex.
    bool pick(size_t& prio, size_t& kind) const
    {
        for (size_t p = 0; p < kJobPriorityCount; ++p)
        {
            if (running[p] >= limit[p]) continue;
            const TaskQueue& cpu = queues[static_cast<size_t>(JobKind::Cpu)][p];
            const TaskQueue& io  = queues[static_cast<size_t>(HE::JobKind::Io)][p];
            const bool cpuOk = !cpu.empty();
            const bool ioOk  = !io.empty() && ioRunning < ioLimit;
            if (!cpuOk && !ioOk) continue;
            prio = p;
            kind = (cpuOk && (!ioOk || cpu.frontKey() <= io.frontKey()))
                 ? static_cast<size_t>(JobKind::Cpu) : static_cast<size_t>(HE::JobKind::Io);
            return true;
        }
        return false;
    }
    bool empty() const
    {
        for (const auto& byKind : queues)
            for (const auto& q : byKind)
                if (!q.empty()) return false;
        return true;
    }
    size_t queuedTotal() const
    {
        size_t n = 0;
        for (const auto& byKind : queues)
            for (const auto& q : byKind) n += q.size();
        return n;
    }
    size_t queued(size_t prio) const
    {
        size_t n = 0;
        for (const auto& byKind : queues) n += byKind[prio].size();
        return n;
    }
};

struct JobState
{
    std::function<void()> fn;
    std::function<bool()> stale;
    std::function<void()> onCancelled;
    CancelToken           cancel;
    const char*           name     = "Job::Execute";
    // Atomic since promote() can change it while the job is queued; the lane a
    // pushed entry sits in is what counts for running it, this decides where the
    // next push (and the stats of a cancel) goes.
    std::atomic<JobPriority> priority{ JobPriority::Normal };
    uint32_t              order = 0;
    JobKind               kind  = JobKind::Cpu;
    // When the job (first) became ready, in QueueClock ticks; 0 = not yet. A
    // promote() re-queues with this, so the wait it reports is the whole one.
    std::atomic<int64_t>  queuedTicks{ 0 };
    std::shared_ptr<PoolCore> core;

    // Every transition out of Waiting/Queued is a compare-exchange, so exactly one
    // thread "claims" a job — a worker, a wait() helping out, or a cancel(). The
    // claimer moves it to Running and is the only one to finish it, including the
    // Cancelled case: that way onCancelled has run and the dependents are released
    // by the time any waiter sees a final status.
    std::atomic<JobStatus> status{ JobStatus::Waiting };
    std::atomic<int>       pendingDeps{ 0 };
    std::atomic<bool>      depFailed{ false };
    std::atomic<bool>      cancelRequested{ false };

    // Guarded by `m`. dependents are strong: they must live until released. deps
    // are weak: only wait() follows them, to help — strong ones would make a job
    // and its dependency own each other until both had finished.
    std::mutex                             m;
    std::condition_variable                cv;
    std::vector<std::shared_ptr<JobState>> dependents;
    std::vector<std::weak_ptr<JobState>>   deps;
    std::exception_ptr                     error;   // written by the runner before the final status
};

using Worklist = std::vector<std::pair<std::shared_ptr<JobState>, JobStatus>>;

static bool isFinal(JobStatus s)
{
    return s == JobStatus::Done || s == JobStatus::Failed || s == JobStatus::Cancelled;
}

static bool cancelWanted(const JobState& j)
{
    return j.cancelRequested.load(std::memory_order_acquire) || j.cancel.cancelled();
}

static bool claim(JobState& j, JobStatus from)
{
    return j.status.compare_exchange_strong(from, JobStatus::Running, std::memory_order_acq_rel);
}

// Wake waiters after a status change. Taking the mutex first means a waiter
// cannot test the old status and then sleep through this notify.
static void notifyStatus(JobState& j)
{
    { std::lock_guard<std::mutex> lock(j.m); }
    j.cv.notify_all();
}

static QueueClock::time_point queuedAtOf(JobState& job)
{
    int64_t ticks = job.queuedTicks.load(std::memory_order_relaxed);
    if (ticks == 0)
    {
        // First push: remember it. Losing the race to a concurrent promote() is fine,
        // both stamp "now".
        const int64_t now = QueueClock::now().time_since_epoch().count();
        job.queuedTicks.compare_exchange_strong(ticks, now, std::memory_order_relaxed);
        ticks = job.queuedTicks.load(std::memory_order_relaxed);
    }
    return QueueClock::time_point(QueueClock::duration(ticks));
}

static bool pushReady(const std::shared_ptr<JobState>& job)
{
    PoolCore& core = *job->core;
    const QueueClock::time_point at = queuedAtOf(*job);
    {
        std::lock_guard<std::mutex> lock(core.mutex);
        if (core.dead) return false;
        const JobPriority prio = job->priority.load();
        core.queues[static_cast<size_t>(job->kind)][static_cast<size_t>(prio)]
            .push(PoolTask{ {}, job->name, job, at }, job->order, core.nextSeq++);
    }
    core.cv.notify_one();
    return true;
}

// The last dependency of `job` has finished (or it had none): queue it — or, if
// it can no longer run, hand it to the worklist to be finished as Cancelled.
static void makeReady(const std::shared_ptr<JobState>& job, Worklist& work)
{
    {
        std::lock_guard<std::mutex> lock(job->m);
        job->deps.clear();
    }
    if (job->depFailed.load(std::memory_order_acquire) || cancelWanted(*job))
    {
        if (claim(*job, JobStatus::Waiting)) work.emplace_back(job, JobStatus::Cancelled);
        return;
    }
    JobStatus expected = JobStatus::Waiting;
    if (!job->status.compare_exchange_strong(expected, JobStatus::Queued, std::memory_order_acq_rel))
        return;   // cancel() got there first and finishes it
    notifyStatus(*job);
    if (!pushReady(job) && claim(*job, JobStatus::Queued))
        work.emplace_back(job, JobStatus::Cancelled);
}

// Finish every (job, status) on the worklist and release their dependents; a
// dependent that can no longer run joins the list. Iterative on purpose: a long
// chain of cancelled dependents must not recurse once per link — worker threads
// have small stacks.
static void finishJobs(Worklist& work)
{
    while (!work.empty())
    {
        auto [job, st] = std::move(work.back());
        work.pop_back();

        if (job->core && (st == JobStatus::Cancelled || st == JobStatus::Failed))
        {
            const size_t p = static_cast<size_t>(job->priority.load());
            (st == JobStatus::Cancelled ? job->core->cancelled[p] : job->core->failed[p])
                .fetch_add(1, std::memory_order_relaxed);
        }

        if (st == JobStatus::Cancelled && job->onCancelled)
        {
            try { job->onCancelled(); }
            catch (...) { HE_LOG_ERROR(Job, "onCancelled of '%s' threw; ignored", job->name); }
        }

        std::vector<std::shared_ptr<JobState>> dependents;
        // The callables die outside the lock and right here, so whatever they
        // captured (a buffer, a sink) is released when the job ends, not when the
        // last handle to it goes away.
        std::function<void()> fn, onCancelled;
        std::function<bool()> stale;
        {
            std::lock_guard<std::mutex> lock(job->m);
            job->status.store(st, std::memory_order_release);
            dependents.swap(job->dependents);
            job->deps.clear();
            fn.swap(job->fn);
            stale.swap(job->stale);
            onCancelled.swap(job->onCancelled);
        }
        job->cv.notify_all();

        for (auto& d : dependents)
        {
            if (st != JobStatus::Done) d->depFailed.store(true, std::memory_order_release);
            if (d->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1)
                makeReady(d, work);
        }
    }
}

// Run a job this thread has claimed (status Running): unless it turned out to be
// cancelled or stale, in which case it finishes as Cancelled without running.
// True when the body ran (ThreadPool::stats counts only those as work).
static bool runClaimed(const std::shared_ptr<JobState>& job)
{
    JobStatus st = JobStatus::Done;
    bool skip = cancelWanted(*job);
    // The pool is being destroyed: background work that has not begun is not worth
    // waiting for on the way out (hundreds of queued asset reads made quitting take
    // as long as reading them). High jobs still run — someone may be blocked on one.
    if (!skip && job->core && job->priority.load() == JobPriority::Low
        && job->core->draining.load(std::memory_order_acquire))
        skip = true;
    if (!skip && job->stale)
    {
        try { skip = job->stale(); }
        catch (...)
        {
            HE_LOG_ERROR(Job, "stale() of '%s' threw; job treated as stale", job->name);
            skip = true;
        }
    }

    if (skip)
    {
        st = JobStatus::Cancelled;
    }
    else
    {
        HE_PROFILE_SCOPE_DYN(job->name);
        // Unlike a post()ed task, a scheduled job's exception never reaches the
        // worker loop: it would take the process down, and its dependents and
        // waiters deserve an answer. Logged here, on the thread that failed.
        try
        {
            job->fn();
        }
        catch (const std::exception& e)
        {
            HE_LOG_ERROR(Job, "Job '%s' threw std::exception: %s", job->name, e.what());
            job->error = std::current_exception();
            st = JobStatus::Failed;
        }
        catch (...)
        {
            HE_LOG_ERROR(Job, "Job '%s' threw a non-std exception", job->name);
            job->error = std::current_exception();
            st = JobStatus::Failed;
        }
    }

    Worklist work;
    work.emplace_back(job, st);
    finishJobs(work);
    return !skip;
}

// wait() on a Waiting job: find a queued job somewhere upstream and run it here.
// Returns false when there is nothing to take (everything upstream is running
// elsewhere or already finished).
static bool helpDependencies(const std::shared_ptr<JobState>& root)
{
    std::vector<std::shared_ptr<JobState>> stack;
    std::unordered_set<const JobState*>    seen;
    auto pushDeps = [&](JobState& j) {
        std::lock_guard<std::mutex> lock(j.m);
        for (const auto& w : j.deps)
            if (auto d = w.lock())
                if (seen.insert(d.get()).second) stack.push_back(std::move(d));
    };
    pushDeps(*root);
    while (!stack.empty())
    {
        std::shared_ptr<JobState> d = std::move(stack.back());
        stack.pop_back();
        const JobStatus s = d->status.load(std::memory_order_acquire);
        if (s == JobStatus::Queued)
        {
            if (claim(*d, JobStatus::Queued)) { runClaimed(d); return true; }
        }
        else if (s == JobStatus::Waiting)
        {
            if (cancelWanted(*d) && claim(*d, JobStatus::Waiting))
            {
                Worklist work;
                work.emplace_back(d, JobStatus::Cancelled);
                finishJobs(work);
                return true;
            }
            pushDeps(*d);
        }
    }
    return false;
}

} // namespace HE::detail

namespace HE {

using namespace detail;

// ─── CancelToken ──────────────────────────────────────────────────────────────
CancelToken::CancelToken() = default;
CancelToken::~CancelToken() = default;
CancelToken::CancelToken(const CancelToken&) = default;
CancelToken::CancelToken(CancelToken&&) noexcept = default;
CancelToken& CancelToken::operator=(const CancelToken&) = default;
CancelToken& CancelToken::operator=(CancelToken&&) noexcept = default;

CancelToken CancelToken::create()
{
    CancelToken t;
    t.m_state = std::make_shared<CancelState>();
    return t;
}

CancelToken CancelToken::child() const
{
    CancelToken t = create();
    t.m_state->parent = m_state;
    return t;
}

void CancelToken::cancel() const
{
    if (m_state) m_state->flag.store(true, std::memory_order_release);
}

bool CancelToken::cancelled() const   { return chainCancelled(m_state.get()); }
bool CancelToken::cancellable() const { return m_state != nullptr; }
bool CancelToken::operator==(const CancelToken& other) const { return m_state == other.m_state; }

// ─── JobHandle ────────────────────────────────────────────────────────────────
JobHandle::JobHandle() = default;
JobHandle::~JobHandle() = default;
JobHandle::JobHandle(const JobHandle&) = default;
JobHandle::JobHandle(JobHandle&&) noexcept = default;
JobHandle& JobHandle::operator=(const JobHandle&) = default;
JobHandle& JobHandle::operator=(JobHandle&&) noexcept = default;
JobHandle::JobHandle(std::shared_ptr<JobState> state) : m_state(std::move(state)) {}

bool JobHandle::valid() const { return m_state != nullptr; }

JobStatus JobHandle::status() const
{
    return m_state ? m_state->status.load(std::memory_order_acquire) : JobStatus::Done;
}

bool JobHandle::finished() const { return isFinal(status()); }

void JobHandle::cancel() const
{
    if (!m_state) return;
    m_state->cancelRequested.store(true, std::memory_order_release);
    // Not started yet → finish it now, so waiters and dependents hear at once. A
    // Queued one leaves a dead entry behind that the worker popping it drops.
    if (claim(*m_state, JobStatus::Waiting) || claim(*m_state, JobStatus::Queued))
    {
        Worklist work;
        work.emplace_back(m_state, JobStatus::Cancelled);
        finishJobs(work);
    }
}

void JobHandle::promote(JobPriority priority) const
{
    if (!m_state) return;
    JobState& j = *m_state;
    if (static_cast<size_t>(priority) >= static_cast<size_t>(j.priority.load())) return;
    // The new priority first, the status second (both seq_cst): a job that turns
    // Queued after the status read below pushes itself into the new lane, and one
    // that was already Queued gets the second entry from here.
    j.priority.store(priority);
    if (j.status.load() != JobStatus::Queued) return;   // Waiting queues itself later; the rest is past promoting
    // A second entry in the higher lane. The first one stays where it is and is
    // dropped by whichever worker pops it after this one has run: claim(Queued)
    // fails there, exactly like the entry of a cancelled or inline-run job.
    pushReady(m_state);
}

void JobHandle::wait() const
{
    const std::shared_ptr<JobState>& job = m_state;
    if (!job) return;
    for (;;)
    {
        const JobStatus s = job->status.load(std::memory_order_acquire);
        if (isFinal(s)) break;

        if (s == JobStatus::Queued)
        {
            // Run it here rather than sleep while it waits for a worker: on a
            // saturated pool — or when called from inside a job — that worker may
            // never come.
            if (claim(*job, JobStatus::Queued)) runClaimed(job);
            continue;
        }
        if (s == JobStatus::Waiting)
        {
            if (cancelWanted(*job) && claim(*job, JobStatus::Waiting))
            {
                Worklist work;
                work.emplace_back(job, JobStatus::Cancelled);
                finishJobs(work);
                continue;
            }
            if (helpDependencies(job)) continue;
            // Upstream is running on other threads. Its completion may queue a job
            // further up that wakes nobody here, so look again after a moment
            // instead of sleeping until this job's own status changes.
            std::unique_lock<std::mutex> lock(job->m);
            job->cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return job->status.load(std::memory_order_acquire) != JobStatus::Waiting;
            });
            continue;
        }
        // Running on another thread (or being finished off by a cancel()).
        std::unique_lock<std::mutex> lock(job->m);
        job->cv.wait(lock, [&] { return isFinal(job->status.load(std::memory_order_acquire)); });
    }
    if (job->status.load(std::memory_order_acquire) == JobStatus::Failed && job->error)
        std::rethrow_exception(job->error);
}

} // namespace HE

// ─── ThreadPool ───────────────────────────────────────────────────────────────
ThreadPool::ThreadPool(size_t threadCount)
    : m_core(std::make_shared<HE::detail::PoolCore>())
{
    using HE::JobPriority;
    HE_LOG_INFO(Job, "ThreadPool starting with %zu worker(s)", threadCount);

    HE::detail::PoolCore& core = *m_core;
    core.threads = threadCount;
    const size_t all = std::max<size_t>(1, threadCount);
    core.limit[static_cast<size_t>(JobPriority::High)]   = all;
    core.limit[static_cast<size_t>(JobPriority::Normal)] = all;
    // Background streaming leaves two workers for the frame (one on a two-thread
    // pool): queue order alone does not help a parallel_for whose helpers find
    // every worker inside a 50 ms pak read.
    core.limit[static_cast<size_t>(JobPriority::Low)]    = threadCount > 3 ? threadCount - 2 : 1;

    core.ioLimit = std::clamp<size_t>(threadCount / 2, 2, 4);
    if (core.ioLimit > all) core.ioLimit = all;

    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i)
    {
        m_threads.emplace_back([core = m_core, i]
        {
            // Named so every log line a worker produces says which worker it was —
            // otherwise concurrent asset streaming and export logs are unreadable.
            char name[16];
            std::snprintf(name, sizeof(name), "Worker-%zu", i);
            HE::Log::setThreadName(name);

            for (;;)
            {
                HE::detail::PoolTask task;
                size_t prio    = 0;
                size_t kind    = 0;
                bool   counted = false;
                bool   isIo    = false;
                {
                    std::unique_lock<std::mutex> lock(core->mutex);
                    core->cv.wait(lock, [&] {
                        return core->pick(prio, kind) || (core->stop && core->empty());
                    });
                    if (!core->pick(prio, kind)) return;   // stopping and drained
                    task = core->queues[kind][prio].pop();
                    isIo = kind == static_cast<size_t>(HE::JobKind::Io);
                    if (isIo) ++core->ioRunning;
                    // Only capped priorities are counted: an uncapped one would pay
                    // a second lock per task for a number nobody reads.
                    counted = core->limit[prio] < core->threads;
                    if (counted) ++core->running[prio];
                }

                // A dead entry (a wait() ran it inline) is not work, and neither
                // is a job claimed only to find it cancelled or stale.
                const bool live = !task.job || HE::detail::claim(*task.job, HE::JobStatus::Queued);
                using StatClock = std::chrono::steady_clock;
                const StatClock::time_point began = live ? StatClock::now() : StatClock::time_point{};
                if (live) core->active[prio].fetch_add(1, std::memory_order_relaxed);
                bool ran = live;
                if (task.job)
                {
                    if (live) ran = HE::detail::runClaimed(task.job);
                }
                else
                {
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
                if (live) core->active[prio].fetch_sub(1, std::memory_order_relaxed);
                if (ran)
                {
                    core->executed[prio].fetch_add(1, std::memory_order_relaxed);
                    core->busyNs[prio].fetch_add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(StatClock::now() - began).count()),
                        std::memory_order_relaxed);
                    if (task.queuedAt.time_since_epoch().count() != 0 && began > task.queuedAt)
                    {
                        const uint64_t waited = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(began - task.queuedAt).count());
                        core->waitNs[prio].fetch_add(waited, std::memory_order_relaxed);
                        uint64_t peak = core->waitMaxNs[prio].load(std::memory_order_relaxed);
                        while (waited > peak &&
                               !core->waitMaxNs[prio].compare_exchange_weak(peak, waited, std::memory_order_relaxed)) {}
                    }
                }

                if (counted || isIo)
                {
                    bool more = false;
                    {
                        std::lock_guard<std::mutex> lock(core->mutex);
                        if (counted) --core->running[prio];
                        if (isIo)    --core->ioRunning;
                        more = core->queued(prio) > 0 || (isIo && !core->empty());
                    }
                    // A cap may be what kept the next one waiting; nothing else
                    // would wake a worker for it.
                    if (more) core->cv.notify_one();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    m_core->draining.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(m_core->mutex);
        m_core->stop = true;
        if (!m_core->empty())
            HE_LOG_WARN(Job, "ThreadPool shutting down with %zu queued task(s) still pending",
                        m_core->queuedTotal());
    }
    m_core->cv.notify_all();
    for (std::thread& t : m_threads)
        t.join();

    // The workers drained every queue before leaving. Anything here was pushed by
    // a dependency that finished on a thread outside this pool after the last
    // worker had gone: it can no longer run, so it ends as Cancelled.
    std::vector<HE::detail::PoolTask> leftovers;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->dead = true;
        for (auto& byKind : m_core->queues)
            for (auto& q : byKind) q.takeAll(leftovers);
    }
    HE::detail::Worklist work;
    for (auto& t : leftovers)
        if (t.job && HE::detail::claim(*t.job, HE::JobStatus::Queued))
            work.emplace_back(t.job, HE::JobStatus::Cancelled);
    HE::detail::finishJobs(work);
    HE_LOG_INFO(Job, "%s", "ThreadPool stopped");
}

void ThreadPool::enqueue(std::function<void()> fn, const char* name, HE::JobPriority priority,
                         size_t copies)
{
    if (copies == 0) return;
    const HE::detail::QueueClock::time_point at = HE::detail::QueueClock::now();
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        auto& q = m_core->queues[static_cast<size_t>(HE::JobKind::Cpu)][static_cast<size_t>(priority)];
        for (size_t i = 1; i < copies; ++i)
            q.push(HE::detail::PoolTask{ fn, name, nullptr, at }, 0, m_core->nextSeq++);
        q.push(HE::detail::PoolTask{ std::move(fn), name, nullptr, at }, 0, m_core->nextSeq++);
    }
    if (copies >= m_threads.size()) m_core->cv.notify_all();
    else for (size_t i = 0; i < copies; ++i) m_core->cv.notify_one();
}

HE::JobHandle ThreadPool::scheduleFn(std::function<void()> fn, HE::JobDesc desc)
{
    using namespace HE::detail;
    auto job = std::make_shared<JobState>();
    job->fn          = std::move(fn);
    job->stale       = std::move(desc.stale);
    job->onCancelled = std::move(desc.onCancelled);
    job->cancel      = std::move(desc.cancel);
    job->name        = desc.name ? desc.name : "Job::Execute";
    job->priority.store(desc.priority);
    job->order       = desc.order;
    job->kind        = desc.kind;
    job->core        = m_core;

    // One count held by this function, so no dependency finishing on another
    // thread can make the job ready while the rest are still being registered.
    job->pendingDeps.store(1, std::memory_order_relaxed);
    for (const HE::JobHandle& h : desc.after)
    {
        const std::shared_ptr<JobState>& dep = h.m_state;
        if (!dep || dep == job) continue;
        // Under dep->m: finishJobs stores the final status and takes the dependents
        // list in one critical section, so the job is either seen as finished here
        // or registered in time to be released.
        std::lock_guard<std::mutex> lock(dep->m);
        const HE::JobStatus s = dep->status.load(std::memory_order_acquire);
        if (isFinal(s))
        {
            if (s != HE::JobStatus::Done) job->depFailed.store(true, std::memory_order_release);
            continue;
        }
        dep->dependents.push_back(job);
        job->deps.push_back(dep);   // job->m not needed: nobody else can see job->deps yet
        job->pendingDeps.fetch_add(1, std::memory_order_acq_rel);
    }

    Worklist work;
    if (job->pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1)
        makeReady(job, work);
    finishJobs(work);
    return HE::JobHandle(job);
}

void ThreadPool::setConcurrencyLimit(HE::JobPriority priority, size_t maxRunning)
{
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->limit[static_cast<size_t>(priority)] = std::max<size_t>(1, maxRunning);
    }
    m_core->cv.notify_all();   // a raised cap lets waiting jobs start now
}

void ThreadPool::setIoConcurrencyLimit(size_t maxRunning)
{
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->ioLimit = std::max<size_t>(1, maxRunning);
    }
    m_core->cv.notify_all();
}

size_t ThreadPool::ioConcurrencyLimit() const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->ioLimit;
}

size_t ThreadPool::concurrencyLimit(HE::JobPriority priority) const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->limit[static_cast<size_t>(priority)];
}

size_t ThreadPool::queuedCount(HE::JobPriority priority) const
{
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->queued(static_cast<size_t>(priority));
}

HE::ThreadPoolStats ThreadPool::stats() const
{
    HE::ThreadPoolStats s;
    const HE::detail::PoolCore& core = *m_core;
    {
        std::lock_guard<std::mutex> lock(core.mutex);
        s.threads   = core.threads;
        s.ioRunning = core.ioRunning;
        s.ioLimit   = core.ioLimit;
        for (size_t p = 0; p < HE::kJobPriorityCount; ++p)
        {
            s.lanes[p].queued = core.queued(p);
            s.lanes[p].limit  = core.limit[p];
        }
    }
    for (size_t p = 0; p < HE::kJobPriorityCount; ++p)
    {
        HE::ThreadPoolStats::Lane& l = s.lanes[p];
        l.running   = core.active[p].load(std::memory_order_relaxed);
        l.executed  = core.executed[p].load(std::memory_order_relaxed);
        l.busyNs    = core.busyNs[p].load(std::memory_order_relaxed);
        l.waitNs    = core.waitNs[p].load(std::memory_order_relaxed);
        l.waitMaxNs = core.waitMaxNs[p].load(std::memory_order_relaxed);
        l.cancelled = core.cancelled[p].load(std::memory_order_relaxed);
        l.failed    = core.failed[p].load(std::memory_order_relaxed);
    }
    return s;
}

void ThreadPool::resetPeaks()
{
    for (auto& m : m_core->waitMaxNs) m.store(0, std::memory_order_relaxed);
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
