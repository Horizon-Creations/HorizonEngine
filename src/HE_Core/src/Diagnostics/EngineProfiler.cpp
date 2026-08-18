#include "Diagnostics/EngineProfiler.h"
#include <cstdint>
#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/ProfilerStats.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
inline uint64_t nowNs() { return SDL_GetTicksNS(); }
inline double   nsToMs(uint64_t ns) { return static_cast<double>(ns) * 1e-6; }

std::string timestampStamp()
{
	std::time_t t = std::time(nullptr);
	char ts[32]{};
	std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
	return ts;
}

// ─── Per-thread timeline buffers (v3) ────────────────────────────────────────
// One buffer per thread that ever opens a scope during a capture. The owning
// thread is the ONLY writer; the profiler's merge is the only other reader, and
// it takes `mtx` — uncontended in practice, which is why a plain mutex beats a
// lock-free ring here (a torn POD read is a worse failure than 20 ns).
//
// Held by shared_ptr from BOTH the thread_local and the registry: a JobSystem
// worker that exits mid-capture drops its reference, the registry keeps the spans
// alive, and the merge never touches a dangling buffer.
struct ThreadCapture
{
	std::mutex                  mtx;          // guards spans/dropped/generation
	std::vector<ProfThreadSpan> spans;        // completed spans, session-relative
	std::vector<uint64_t>       openStart;    // absolute start ns per open scope (owner only)
	std::vector<const char*>    openName;     // parallel to openStart          (owner only)
	uint64_t                    generation = UINT64_MAX;   // capture this buffer belongs to
	size_t                      dropped    = 0;            // spans lost to the cap
	std::string                 label;
	bool                        isMain     = false;
};

// Hard per-thread span budget. A 60-second capture at 60 fps with a few hundred
// job scopes per frame lands well under this; a runaway instrumentation bug hits
// the cap instead of the swap file. Overflow is COUNTED, never silently dropped.
constexpr size_t kMaxSpansPerThread = 262144;

std::mutex& registryMutex()
{
	static std::mutex m;
	return m;
}

std::vector<std::shared_ptr<ThreadCapture>>& registry()
{
	static std::vector<std::shared_ptr<ThreadCapture>> v;
	return v;
}

// The calling thread's buffer, created and registered on first use. `isMain` is
// fixed at creation: a thread's role in a capture never changes, and the main
// thread always reaches this before any worker does (it starts the capture).
ThreadCapture& threadCapture(bool isMain)
{
	thread_local std::shared_ptr<ThreadCapture> tls = [isMain] {
		auto p     = std::make_shared<ThreadCapture>();
		p->isMain  = isMain;
		std::lock_guard<std::mutex> lk(registryMutex());
		size_t workers = 0;
		for (const auto& e : registry())
			if (!e->isMain) ++workers;
		p->label = isMain ? std::string("Main") : ("Worker " + std::to_string(workers));
		registry().push_back(p);
		return p;
	}();
	return *tls;
}

// Drop buffers whose owning thread has exited. A live thread holds a second
// reference from its thread_local, so use_count()==1 means the registry is the
// only owner left and nothing will ever touch that buffer again — its spans
// would otherwise pin up to a quarter-million entries for the rest of the
// process. Called at capture start, where a copy is being thrown away anyway.
void pruneDeadThreadBuffers()
{
	std::lock_guard<std::mutex> lk(registryMutex());
	auto& v = registry();
	v.erase(std::remove_if(v.begin(), v.end(),
	                       [](const std::shared_ptr<ThreadCapture>& p) { return p.use_count() == 1; }),
	        v.end());
}

// Drop everything from an earlier capture. Called on first touch in a new
// generation so a worker that slept through the previous capture starts clean.
void resetIfStale(ThreadCapture& tc, uint64_t generation)
{
	if (tc.generation == generation) return;
	tc.openStart.clear();
	tc.openName.clear();
	std::lock_guard<std::mutex> lk(tc.mtx);
	tc.generation = generation;
	tc.dropped    = 0;
	tc.spans.clear();
}
} // namespace

EngineProfiler& EngineProfiler::instance()
{
	static EngineProfiler s_instance;
	return s_instance;
}

// ─── Control ─────────────────────────────────────────────────────────────────

void EngineProfiler::requestStart(const ProfSessionInfo& info, size_t maxFrames)
{
	m_pendingInfo = info;
	m_pendingMax  = maxFrames;
	m_pending     = Pending::Start;
}

void EngineProfiler::requestStop()
{
	m_pending = Pending::Stop;
}

void EngineProfiler::requestToggle(const ProfSessionInfo& info, size_t maxFrames)
{
	if (isRecordingOrPending()) requestStop();
	else                        requestStart(info, maxFrames);
}

bool EngineProfiler::consumeJustDumped(std::string& outPath)
{
	if (!m_justDumped) return false;
	outPath      = m_lastDumpPath;
	m_justDumped = false;
	return true;
}

void EngineProfiler::doStart()
{
	m_session        = m_pendingInfo;
	m_maxFrames      = m_pendingMax;
	m_frames.clear();
	m_frameMarks.clear();
	m_frameCounter   = 0;
	m_sessionStartNs = nowNs();
	m_stack.clear();
	m_dumpsDir       = GlobalState::getInstance().getDumpsDir();
	// The thread that runs doStart (the frame loop / main thread) owns the scope
	// stack; scopes from any other thread are ignored. Set before publishing
	// m_recording so worker threads that observe recording also see this id.
	m_captureThread  = std::this_thread::get_id();
	pruneDeadThreadBuffers();
	// Everything a worker reads without its own synchronisation — m_sessionStartNs,
	// m_captureThread, the generation — is written ABOVE this release store, and a
	// worker only reads it after the acquire load in beginScope. That edge is the
	// whole happens-before argument; keep new plain state on this side of it.
	m_generation.fetch_add(1, std::memory_order_relaxed);
	m_recording.store(true, std::memory_order_release);
	HE_LOG_INFO(Profiler, "%s", "Profiler: capture started");
}

// ─── Frame lifecycle ─────────────────────────────────────────────────────────

void EngineProfiler::beginFrame(double deltaMs)
{
	// Apply a pending start/stop on the frame boundary so frames are recorded
	// whole or not at all.
	if (m_pending == Pending::Start && !m_recording)
	{
		doStart();
	}
	else if (m_pending == Pending::Stop && m_recording)
	{
		m_lastDumpPath = doStopDump();
		m_justDumped   = true;
		m_recording.store(false, std::memory_order_release);
	}
	m_pending = Pending::None;

	// Apply a pending single-frame capture (records exactly one detailed frame into
	// m_singleFrame on endFrame; no dump). Only if a multi-frame capture isn't running.
	if (m_pendingSingle && !m_recording)
	{
		m_pendingSingle = false;
		m_singleMode    = true;
		// Do NOT force detailed GPU here — detailed serializes the GPU
		// (waitUntilCompleted per pass), which makes the captured frame much slower.
		// Respect the user's "Detailed GPU pass timing" checkbox instead: fast by
		// default (CPU scopes + counters + whole-frame GPU), exclusive per-pass only
		// when the user opts in. (m_forceDetailed stays false.)
		doStart();
	}

	m_frameStartNs = nowNs();   // always — cheap; drives lastCpuFrameMs() for the live HUD
	if (!m_recording) return;   // deltaMs is only stored per recorded frame (m_current.deltaMs);
	                            // the live HUD gets its delta straight from Application's own dt

	m_current           = ProfFrameRecord{};
	m_current.index     = m_frameCounter;
	m_current.deltaMs   = deltaMs;
	m_current.wallMs    = nsToMs(m_frameStartNs - m_sessionStartNs);
	m_stack.clear();
}

void EngineProfiler::endFrame()
{
	const uint64_t endNs = nowNs();
	m_lastCpuFrameMs = nsToMs(endNs - m_frameStartNs);     // always (live HUD)
	if (!m_recording) return;

	// Close any scopes the caller forgot to (defensive; should not happen).
	while (!m_stack.empty()) endScope();

	m_current.cpuFrameMs = m_lastCpuFrameMs;

	// Frame boundary on the timeline axis (same relative-ns units as the spans),
	// so the timeline can draw frame separators over the thread lanes. Kept in
	// lockstep with the m_frames ring below so a mark always has its record.
	if (!m_singleMode)
	{
		if (m_maxFrames != 0 && m_frameMarks.size() >= m_maxFrames)
			m_frameMarks.erase(m_frameMarks.begin());
		m_frameMarks.push_back({ m_current.index,
		                         m_frameStartNs > m_sessionStartNs ? m_frameStartNs - m_sessionStartNs : 0,
		                         endNs          > m_sessionStartNs ? endNs          - m_sessionStartNs : 0 });
	}

	// Single-frame capture: stash the one frame for the UI and stop (no dump, not
	// added to the multi-frame ring).
	if (m_singleMode)
	{
		m_singleFrame     = std::move(m_current);
		m_haveSingleFrame = true;
		m_singleMode      = false;
		m_forceDetailed   = false;
		m_recording.store(false, std::memory_order_release);
		++m_frameCounter;
		HE_LOG_INFO(Profiler, "%s", "Profiler: single-frame capture taken");
		return;
	}

	if (m_maxFrames != 0 && m_frames.size() >= m_maxFrames)
		m_frames.erase(m_frames.begin());   // ring: drop oldest
	m_frames.push_back(std::move(m_current));
	++m_frameCounter;
}

void EngineProfiler::pushLive(const ProfLiveFrame& f)
{
	if (m_live.size() >= m_liveCap) m_live.erase(m_live.begin());
	m_live.push_back(f);
}

// ─── CPU scopes ──────────────────────────────────────────────────────────────

// The per-FRAME scope tree (m_stack → m_current.scopes) is main-thread only:
// scopes opened on worker threads (the JobSystem pool names each task) never
// touch it, so it stays single-writer. Without that guard, concurrent push_back
// corrupts the heap — the v2.1 crash.
//
// The per-THREAD timeline runs for every thread, into that thread's own buffer,
// which is why worker work is visible at all now. Both paths share one timestamp.
void EngineProfiler::beginScope(const char* name)
{
	if (!m_recording.load(std::memory_order_acquire)) return;
	const bool     isMain = std::this_thread::get_id() == m_captureThread;
	const uint64_t now    = nowNs();

	if (m_timelineOn.load(std::memory_order_relaxed))
	{
		ThreadCapture& tc = threadCapture(isMain);
		resetIfStale(tc, m_generation.load(std::memory_order_relaxed));
		tc.openStart.push_back(now);
		tc.openName.push_back(name);
	}

	if (!isMain) return;
	m_stack.push_back({ name, now, static_cast<uint32_t>(m_stack.size()) });
}

void EngineProfiler::endScope()
{
	if (!m_recording.load(std::memory_order_acquire)) return;
	const bool     isMain = std::this_thread::get_id() == m_captureThread;
	const uint64_t now    = nowNs();

	if (m_timelineOn.load(std::memory_order_relaxed))
	{
		ThreadCapture& tc  = threadCapture(isMain);
		const uint64_t gen = m_generation.load(std::memory_order_relaxed);
		// A scope that OPENED before this capture (or before the timeline was
		// switched on) has no matching entry here; skip it rather than pairing it
		// with somebody else's start. ProfileScope latching makes this rare, but a
		// mid-scope setThreadTimelineEnabled(true) reaches it.
		if (tc.generation == gen && !tc.openStart.empty())
		{
			const uint64_t startNs = tc.openStart.back();
			const char*    nm      = tc.openName.back();
			const uint32_t depth   = static_cast<uint32_t>(tc.openStart.size() - 1);
			tc.openStart.pop_back();
			tc.openName.pop_back();

			std::lock_guard<std::mutex> lk(tc.mtx);
			if (tc.spans.size() < kMaxSpansPerThread)
				tc.spans.push_back({ nm,
				                     startNs > m_sessionStartNs ? startNs - m_sessionStartNs : 0,
				                     now     > m_sessionStartNs ? now     - m_sessionStartNs : 0,
				                     depth });
			else
				++tc.dropped;
		}
	}

	if (!isMain) return;
	if (m_stack.empty()) return;
	const LiveScope s = m_stack.back();
	m_stack.pop_back();
	m_current.scopes.push_back({ s.name, nsToMs(now - s.startNs), s.depth });
}

// ─── Per-thread timeline access ──────────────────────────────────────────────

std::vector<ProfThreadTimeline> EngineProfiler::timelineSnapshot() const
{
	const uint64_t gen = m_generation.load(std::memory_order_relaxed);
	std::vector<ProfThreadTimeline> out;

	std::lock_guard<std::mutex> reg(registryMutex());
	for (const auto& tc : registry())
	{
		std::lock_guard<std::mutex> lk(tc->mtx);
		if (tc->generation != gen || tc->spans.empty()) continue;   // stale or idle thread
		ProfThreadTimeline t;
		t.label   = tc->label;
		t.isMain  = tc->isMain;
		t.dropped = tc->dropped;
		t.spans   = tc->spans;
		out.push_back(std::move(t));
	}
	// Main lane first, workers after it in registration order — a stable lane
	// ordering matters more than any particular one: lanes that jump between
	// snapshots make the timeline unreadable while it updates live.
	std::stable_sort(out.begin(), out.end(),
	                 [](const ProfThreadTimeline& a, const ProfThreadTimeline& b)
	                 { return a.isMain && !b.isMain; });
	return out;
}

size_t EngineProfiler::timelineDroppedSpans() const
{
	const uint64_t gen = m_generation.load(std::memory_order_relaxed);
	size_t total = 0;
	std::lock_guard<std::mutex> reg(registryMutex());
	for (const auto& tc : registry())
	{
		std::lock_guard<std::mutex> lk(tc->mtx);
		if (tc->generation == gen) total += tc->dropped;
	}
	return total;
}

// ─── Per-frame data ──────────────────────────────────────────────────────────

// Renderer-owned fields only. The scene counters in m_current.stats were pushed
// earlier in this same frame by the world tick (a different producer at a different
// point in the loop); assigning the whole struct here would zero them every frame,
// which is precisely how entities/lights/particles stayed 0 in every dump until v3.
void EngineProfiler::setRenderStats(const ProfRenderStats& s)
{
	if (!m_recording) return;
	m_current.stats.drawCalls      = s.drawCalls;
	m_current.stats.triangles      = s.triangles;
	m_current.stats.visibleObjects = s.visibleObjects;
	m_current.stats.totalObjects   = s.totalObjects;
	m_current.stats.vramUsedMB     = s.vramUsedMB;
	m_current.stats.vramBudgetMB   = s.vramBudgetMB;
}

void EngineProfiler::setSceneCounters(const ProfSceneCounters& c)
{
	m_sceneCounters = c;                       // cached for the live HUD too
	if (m_recording) m_current.stats.scene = c;
}

void EngineProfiler::setGpuTimes(double gpuFrameMs, const std::vector<ProfGpuPass>& passes,
                                 const char* mode)
{
	if (!m_recording) return;
	m_current.gpuFrameMs    = gpuFrameMs;
	m_current.gpuPasses     = passes;
	m_current.gpuTimingMode = mode ? mode : "";
}

// ─── Live view ───────────────────────────────────────────────────────────────

const ProfFrameRecord* EngineProfiler::lastFrame() const
{
	return m_frames.empty() ? nullptr : &m_frames.back();
}

std::vector<ProfFrameRecord> EngineProfiler::snapshot() const
{
	return m_frames;
}

// ─── Dump ────────────────────────────────────────────────────────────────────

namespace {
// Accumulate min/avg/max/count for a named series.
struct Stat
{
	double   mn  = std::numeric_limits<double>::max();
	double   mx  = 0.0;
	double   sum = 0.0;
	uint64_t n   = 0;
	void add(double v) { mn = std::min(mn, v); mx = std::max(mx, v); sum += v; ++n; }
	json toJson() const
	{
		return json{ {"min", n ? mn : 0.0}, {"avg", n ? sum / n : 0.0},
		             {"max", mx}, {"count", n} };
	}
};
} // namespace

std::string EngineProfiler::doStopDump()
{
	std::string path = dumpNow();
	HE_LOG_INFO(Profiler, "%s",
	            ("Profiler: capture stopped — " + std::to_string(m_frames.size()) +
	             " frames").c_str());
	return path;
}

std::string EngineProfiler::dumpNow()
{
	if (m_frames.empty()) return "";

	if (m_dumpsDir.empty())
		m_dumpsDir = GlobalState::getInstance().getDumpsDir();

	// Per-scope and per-GPU-pass summaries across the whole capture.
	// CPU scopes are summarised by HE::Prof::aggregateScopes below (it adds self
	// time and p95, which this Stat cannot express); only the GPU passes still use it.
	std::map<std::string, Stat> gpuSummary;
	std::map<std::string, bool> gpuApprox;   // a pass name is approx if any sample was
	std::map<std::string, uint64_t> gpuModeCounts;  // which GPU-timing path produced each frame
	Stat cpuFrame, gpuFrame, delta;
	Stat passOverlap;   // Σ(exact passes)/gpuFrameMs per frame; >1 ⇒ spans overlap (TBDR)
	for (const auto& f : m_frames)
	{
		cpuFrame.add(f.cpuFrameMs);
		delta.add(f.deltaMs);
		if (f.gpuFrameMs >= 0.0) gpuFrame.add(f.gpuFrameMs);
		if (f.gpuTimingMode && f.gpuTimingMode[0]) gpuModeCounts[f.gpuTimingMode]++;
		double sumExact = 0.0; bool anyExact = false;
		for (const auto& g : f.gpuPasses)
		{
			gpuSummary[g.name].add(g.ms);
			if (g.approx) gpuApprox[g.name] = true;
			else { sumExact += g.ms; anyExact = true; }
		}
		if (f.gpuFrameMs > 0.0 && anyExact) passOverlap.add(sumExact / f.gpuFrameMs);
	}

	json j;
	j["tool"]    = "HorizonEngine EngineProfiler";
	j["version"] = 2;   // 2 = v3 profiler: per-thread timeline, frameMarks, statistics
	j["session"] = {
		{"backend", m_session.backend}, {"gpu", m_session.gpuName},
		{"os", m_session.os}, {"width", m_session.width}, {"height", m_session.height},
		{"vsync", m_session.vsync}, {"note", m_session.note},
		{"frameCount", m_frames.size()},
	};
	if (!m_session.vsync)
		j["session"]["captureNote"] = "vsync OFF — frame/FPS reflect true cost";
	else
		j["session"]["captureNote"] = "vsync ON — CPU frame time is refresh-pinned; trust gpuPasses, not FPS";

	// Summary block (the at-a-glance answer to "what is slow").
	json summary;
	summary["cpuFrameMs"] = cpuFrame.toJson();
	summary["deltaMs"]    = delta.toJson();
	if (gpuFrame.n) summary["gpuFrameMs"] = gpuFrame.toJson();

	// ── Tail statistics (v3) ────────────────────────────────────────────────
	// min/avg/max above answers "how fast on average", which is the question
	// nobody has. Stutter lives in the tail, so the tail gets its own block:
	// percentiles per series, benchmark-convention low-percentile FPS, and the
	// individual hitch frames with the scope that was expensive in each.
	{
		std::vector<double> deltas, cpus, gpus;
		deltas.reserve(m_frames.size()); cpus.reserve(m_frames.size());
		for (const auto& f : m_frames)
		{
			deltas.push_back(f.deltaMs);
			cpus.push_back(f.cpuFrameMs);
			if (f.gpuFrameMs >= 0.0) gpus.push_back(f.gpuFrameMs);
		}
		auto pctJson = [](const HE::Prof::Percentiles& p) {
			return json{ {"min", p.min}, {"mean", p.mean}, {"p50", p.p50},
			             {"p95", p.p95}, {"p99", p.p99}, {"max", p.max},
			             {"stddev", p.stddev}, {"count", p.count} };
		};
		json stats;
		stats["deltaMs"] = pctJson(HE::Prof::computePercentiles(deltas));
		stats["cpuMs"]   = pctJson(HE::Prof::computePercentiles(cpus));
		if (!gpus.empty()) stats["gpuMs"] = pctJson(HE::Prof::computePercentiles(gpus));

		const HE::Prof::FpsLows lows = HE::Prof::computeFpsLows(deltas);
		stats["fps"] = { {"avg", lows.avgFps}, {"low1Percent", lows.low1Fps},
		                 {"low01Percent", lows.low01Fps}, {"frames", lows.frames},
		                 {"note", "1%/0.1% low = AVERAGE of the slowest 1%/0.1% of frames "
		                          "expressed as FPS (benchmark convention), not the p99 frame time"} };
		summary["stats"] = stats;

		const std::vector<HE::Prof::Hitch> hitches = HE::Prof::findHitches(m_frames);
		if (!hitches.empty())
		{
			json jh = json::array();
			for (const HE::Prof::Hitch& h : hitches)
			{
				json e = { {"frame", h.frameIndex}, {"deltaMs", h.deltaMs},
				           {"xMedian", h.ratio} };
				if (h.worstScope && h.worstScope[0])
				{ e["worstScope"] = h.worstScope; e["worstScopeMs"] = h.worstScopeMs; }
				if (h.gpuMs >= 0.0) e["gpuMs"] = h.gpuMs;
				jh.push_back(std::move(e));
			}
			summary["hitches"]     = jh;
			summary["hitchCount"]  = hitches.size();
			summary["hitchNote"]   = "frames over 2x the median frame time, worst first "
			                         "(max 64 listed); worstScope is the costliest depth-0 CPU scope";
		}
	}
	// Honesty guard: on tile-deferred (TBDR / Apple Silicon) GPUs the per-encoder
	// stage-boundary spans overlap — fragment/tile work drains together near frame
	// end, so each pass's [startVertex,endFragment] window stretches to ≈ the whole
	// frame and they sum to several× gpuFrameMs. They are wall-clock spans, NOT
	// exclusive cost. Flag it so a reader never sums them (the editor does the same).
	if (passOverlap.n && passOverlap.sum / passOverlap.n > 1.1)
	{
		const double ratio = passOverlap.sum / passOverlap.n;
		char buf[48]; std::snprintf(buf, sizeof(buf), "%.1f", ratio);
		summary["gpuPassesOverlap"] = true;
		summary["gpuPassNote"] =
			std::string("per-encoder GPU spans overlap on this GPU (Σ exact passes ≈ ") + buf +
			"× gpuFrameMs) — they are wall-clock spans, NOT exclusive per-pass cost; do not sum. "
			"Trust gpuFrameMs for total GPU work; use Xcode Metal System Trace for accurate "
			"per-pass / per-draw GPU timing.";
	}
	// Which GPU-timing path actually produced each frame's passes (frames per mode).
	// "detailed" = exclusive/additive (reliable); "counter" = overlapping spans;
	// "whole-frame" = no per-pass. Says what RAN, regardless of the requested toggle.
	if (!gpuModeCounts.empty())
	{
		json modes = json::object();
		for (const auto& [name, n] : gpuModeCounts) modes[name] = n;
		summary["gpuTimingModes"] = modes;
	}
	// Per-scope aggregate, now carrying SELF time (inclusive minus direct children)
	// and p95. Self time is what names the culprit: "Render 9 ms" only says the
	// renderer is on the call chain, "Render self 8.6 ms" says the cost is its own.
	{
		const std::vector<HE::Prof::ScopeAggregate> aggregates = HE::Prof::aggregateScopes(m_frames);
		json cpuScopes = json::object();
		for (const HE::Prof::ScopeAggregate& a : aggregates)
			cpuScopes[a.name] = { {"min", a.minMs}, {"avg", a.avgMs}, {"max", a.maxMs},
			                      {"p95", a.p95Ms}, {"count", a.count},
			                      {"totalMs", a.totalMs}, {"selfMs", a.selfMs},
			                      {"depth", a.minDepth} };
		summary["cpuScopes"] = cpuScopes;

		// The ranking, pre-sorted by self time, so a reader (or a diff script) does
		// not have to re-derive it from the object above.
		json top = json::array();
		size_t n = 0;
		for (const HE::Prof::ScopeAggregate& a : aggregates)
		{
			if (n++ >= 20) break;
			top.push_back({ {"scope", a.name}, {"selfMs", a.selfMs},
			                {"totalMs", a.totalMs}, {"count", a.count} });
		}
		summary["topScopesBySelfTime"] = top;
	}
	if (!gpuSummary.empty())
	{
		json gpuPasses = json::object();
		for (const auto& [name, st] : gpuSummary)
		{
			json entry = st.toJson();
			if (gpuApprox.count(name)) entry["approx"] = true;
			gpuPasses[name] = entry;
		}
		summary["gpuPasses"] = gpuPasses;
	}

	// ── Per-thread timeline (v3) ────────────────────────────────────────────
	// Every lane is summarised (cheap, always useful: "Worker 3 was busy 4% of the
	// capture" is the parallelism verdict). The raw spans are the expensive part —
	// hundreds of thousands over a long capture — so they get a total budget.
	//
	// The budget is spent as a TIME CUTOFF, not as a per-lane quota. The first
	// version wrote lanes in order until the budget ran out, and the main lane ate
	// all of it: a real 4867-frame dump came out with 155k main-thread spans and
	// literally zero on any worker, which reads as an idle thread pool — the exact
	// wrong conclusion, from a lane that was simply never written. Cutting at a
	// shared timestamp instead gives every lane the same shorter window, so the
	// dump is a consistent prefix of the capture rather than an arbitrary subset.
	// Nesting survives for free: a parent starts before its children, so if a child
	// is in, its parent is too.
	const std::vector<ProfThreadTimeline> lanes = timelineSnapshot();
	if (!lanes.empty())
	{
		constexpr size_t kMaxDumpSpans = 120000;

		size_t totalSpans = 0;
		for (const ProfThreadTimeline& lane : lanes) totalSpans += lane.spans.size();

		uint64_t cutoffNs = UINT64_MAX;   // no cutoff = write everything
		if (totalSpans > kMaxDumpSpans)
		{
			std::vector<uint64_t> starts;
			starts.reserve(totalSpans);
			for (const ProfThreadTimeline& lane : lanes)
				for (const ProfThreadSpan& s : lane.spans) starts.push_back(s.startNs);
			std::nth_element(starts.begin(), starts.begin() + kMaxDumpSpans, starts.end());
			cutoffNs = starts[kMaxDumpSpans];
		}

		size_t written = 0, omitted = 0;

		json summaryLanes = json::array();
		json jthreads     = json::array();
		for (const ProfThreadTimeline& lane : lanes)
		{
			// Busy time = union of depth-0 spans; nested spans are already inside
			// their parent, so summing every depth would count the same nanosecond
			// once per level and happily report 300% occupancy.
			double busyMs = 0.0, deepestMs = 0.0;
			uint32_t maxDepth = 0;
			for (const ProfThreadSpan& s : lane.spans)
			{
				const double ms = nsToMs(s.endNs - s.startNs);
				if (s.depth == 0) busyMs += ms;
				if (s.depth > maxDepth) maxDepth = s.depth;
				if (ms > deepestMs) deepestMs = ms;
			}
			summaryLanes.push_back({ {"thread", lane.label}, {"main", lane.isMain},
			                         {"spans", lane.spans.size()}, {"busyMs", busyMs},
			                         {"longestSpanMs", deepestMs}, {"maxDepth", maxDepth},
			                         {"droppedSpans", lane.dropped} });

			json jspans = json::array();
			for (const ProfThreadSpan& s : lane.spans)
			{
				if (s.startNs >= cutoffNs) { ++omitted; continue; }
				jspans.push_back({ {"n", s.name}, {"s", s.startNs}, {"e", s.endNs}, {"d", s.depth} });
				++written;
			}
			jthreads.push_back({ {"thread", lane.label}, {"main", lane.isMain},
			                     {"spans", std::move(jspans)} });
		}
		summary["threads"] = summaryLanes;
		if (omitted > 0)
		{
			summary["timelineTruncated"] = omitted;
			summary["timelineCutoffNs"]  = cutoffNs;
			summary["timelineNote"] =
				std::string("timeline truncated at ") +
				std::to_string(static_cast<double>(cutoffNs) * 1e-6) + " ms into the capture (" +
				std::to_string(written) + " spans kept, " + std::to_string(omitted) +
				" omitted, budget " + std::to_string(kMaxDumpSpans) +
				"). EVERY lane is cut at the same timestamp, so threads[] is a consistent "
				"prefix of the capture, not a subset of lanes. summary.threads covers the "
				"FULL capture — use it for totals.";
			HE_LOG_WARN(Profiler, "%s",
			            ("Profiler: timeline truncated in dump at " +
			             std::to_string(static_cast<double>(cutoffNs) * 1e-6) + " ms — " +
			             std::to_string(omitted) + " spans omitted").c_str());
		}
		const size_t droppedLive = timelineDroppedSpans();
		if (droppedLive > 0) summary["timelineDroppedAtCapture"] = droppedLive;
		j["threads"] = std::move(jthreads);
	}

	j["summary"] = summary;

	// Frame boundaries on the timeline axis, so a reader can line the thread lanes
	// up with the frames[] entries (both carry the same frame index). Clipped to the
	// same cutoff as the spans, so a truncated dump does not draw frame separators
	// across a stretch of timeline where no lane has any data.
	if (!m_frameMarks.empty())
	{
		const uint64_t markCutoff = j["summary"].contains("timelineCutoffNs")
		                          ? j["summary"]["timelineCutoffNs"].get<uint64_t>()
		                          : UINT64_MAX;
		json marks = json::array();
		for (const ProfFrameMark& m : m_frameMarks)
		{
			if (m.startNs >= markCutoff) continue;
			marks.push_back({ {"i", m.index}, {"s", m.startNs}, {"e", m.endNs} });
		}
		j["frameMarks"] = std::move(marks);
	}

	// Per-frame detail.
	json frames = json::array();
	for (const auto& f : m_frames)
	{
		json jf;
		jf["i"]       = f.index;
		jf["wallMs"]  = f.wallMs;
		jf["deltaMs"] = f.deltaMs;
		jf["cpuMs"]   = f.cpuFrameMs;
		if (f.gpuFrameMs >= 0.0) jf["gpuMs"] = f.gpuFrameMs;
		if (f.gpuTimingMode && f.gpuTimingMode[0]) jf["gpuMode"] = f.gpuTimingMode;
		json scopes = json::array();
		for (const auto& s : f.scopes)
			scopes.push_back({ {"n", s.name}, {"ms", s.ms}, {"d", s.depth} });
		jf["cpu"] = scopes;
		if (!f.gpuPasses.empty())
		{
			json gp = json::array();
			for (const auto& g : f.gpuPasses)
			{
				json e = { {"n", g.name}, {"ms", g.ms} };
				if (g.approx) e["approx"] = true;
				gp.push_back(std::move(e));
			}
			jf["gpu"] = gp;
		}
		const auto& s = f.stats;
		jf["stats"] = {
			{"draws", s.drawCalls}, {"tris", s.triangles},
			{"visible", s.visibleObjects}, {"total", s.totalObjects},
			{"entities", s.scene.entities}, {"lights", s.scene.lights},
			{"particles", s.scene.particles}, {"emitters", s.scene.emitters},
			{"gpuParticles", s.scene.gpuParticles},
			{"rigidBodies", s.scene.rigidBodies}, {"audioSources", s.scene.audioSources},
			{"scripts", s.scene.scripts},
			{"streamingInFlight", s.scene.streamingInFlight},
			{"vramUsedMB", s.vramUsedMB}, {"vramBudgetMB", s.vramBudgetMB},
		};
		frames.push_back(std::move(jf));
	}
	j["frames"] = frames;

	fs::path out = fs::path(m_dumpsDir) / ("profile_" + timestampStamp() + ".json");
	std::error_code ec;
	fs::create_directories(m_dumpsDir, ec);
	std::ofstream f(out.string());
	if (!f.is_open())
	{
		HE_LOG_ERROR(Profiler, "%s",
		            ("Profiler: failed to open dump file " + out.string()).c_str());
		return "";
	}
	f << j.dump(1, '\t');
	f.close();
	HE_LOG_INFO(Profiler, "%s", ("Profiler: wrote " + out.string()).c_str());
	return out.string();
}
