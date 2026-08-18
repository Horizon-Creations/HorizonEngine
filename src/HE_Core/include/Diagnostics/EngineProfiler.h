#pragma once
// ─── EngineProfiler ──────────────────────────────────────────────────────────
// Runtime, start/stop-able self-diagnostic profiler. Independent of Tracy.
//
//  • Zero cost when not recording (one bool check per scope).
//  • CPU scope timers (nested, RAII via ProfileScope / the HE_PROFILE_* macros).
//  • Per-frame render counters + per-pass GPU times pushed by the active backend.
//  • Dumps a structured JSON report to <deploy>/dumps/ on stop.
//
// Two capture models run side by side, deliberately:
//
//  1. PER-FRAME scope tree (m_current.scopes) — main thread ONLY. The frame loop,
//     scene-system tick and editor UI all run on the main thread, so begin/endFrame,
//     snapshot() and the dump are never called concurrently. Scopes CAN be opened on
//     worker threads (the JobSystem pool wraps every task in HE_PROFILE_SCOPE_N),
//     so this path records only from the capture thread and ignores every other one;
//     that keeps the shared scope stack single-writer. Concurrent push_back on it
//     corrupted the heap in v2.1 — this guard is the fix and it stays.
//
//  2. PER-THREAD TIMELINE (v3) — every thread, including the workers dropped by (1).
//     Each thread owns a thread_local buffer of COMPLETED spans carrying absolute
//     start/end stamps, so it is a capture-wide event stream rather than a per-frame
//     tree: a span that straddles a frame boundary needs no ownership policy, and
//     frames are just marks along the same axis. Nothing is shared between threads
//     on the hot path — the owner is the only writer — and the buffer is a shared_ptr
//     held by a registry, so it outlives a worker that exits mid-capture. A per-buffer
//     mutex (uncontended: owner pushing vs. the UI merging) covers the one place a
//     second thread reads. This is what makes parallel_for work visible at all.
// Worker-thread Tracy zones are unaffected by either path.

#include "Types/Defines.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// One CPU scope, finalized into the frame it closed in. `name` must be a string
// literal / static storage (the macros only ever pass literals or __FUNCTION__).
struct ProfScopeSample
{
	const char* name  = "";
	double      ms    = 0.0;
	uint32_t    depth = 0;   // nesting depth (0 = top level)
};

// One GPU pass time, supplied by the backend (e.g. Metal encode passes). `name`
// is a static string literal owned by the backend.
//   approx = true  → the time is a draw-boundary interval inside a single render
//   encoder (an intra-"Scene" element split). On TBDR GPUs the fragment work is
//   tile-deferred, so such sub-encoder deltas are approximations, not exact pass
//   costs — flagged so the dump and the UI never present them as ground truth.
struct ProfGpuPass
{
	const char* name   = "";
	double      ms     = 0.0;
	bool        approx = false;
};

// ─── Per-thread timeline (v3) ────────────────────────────────────────────────
// One completed CPU span on one thread. Timestamps are nanoseconds RELATIVE to the
// capture start, so every thread's spans share one axis and can be laid out in
// parallel lanes. `name` is a static literal (same contract as ProfScopeSample).
struct ProfThreadSpan
{
	const char* name    = "";
	uint64_t    startNs = 0;
	uint64_t    endNs   = 0;
	uint32_t    depth   = 0;   // nesting depth within its own thread
};

// One thread's lane on the timeline, produced by timelineSnapshot().
struct ProfThreadTimeline
{
	std::string                 label;        // "Main" / "Worker 0" / …
	bool                        isMain = false;
	size_t                      dropped = 0;  // spans lost to the per-thread cap
	std::vector<ProfThreadSpan> spans;        // in completion order
};

// A frame boundary drawn along the timeline axis (same relative-ns units as spans).
struct ProfFrameMark
{
	uint64_t index   = 0;
	uint64_t startNs = 0;
	uint64_t endNs   = 0;
};

// Coarse per-frame counters. Cheap to gather; filled from the renderer + scene.
struct ProfRenderStats
{
	uint32_t drawCalls      = 0;
	uint32_t triangles      = 0;
	uint32_t visibleObjects = 0;
	uint32_t totalObjects   = 0;
	uint32_t entities       = 0;
	uint32_t lights         = 0;
	uint32_t particles      = 0;   // CPU particle count
	uint32_t gpuParticles   = 0;   // GPU precip pool cap (if active)
	uint32_t streamingInFlight = 0;
	double   vramUsedMB     = 0.0;
	double   vramBudgetMB   = 0.0;
};

// Everything captured for a single rendered frame.
struct ProfFrameRecord
{
	uint64_t index      = 0;
	double   wallMs     = 0.0;   // ms since capture start (frame begin)
	double   deltaMs    = 0.0;   // wall-clock frame pacing (vsync-pinned if vsync on!)
	double   cpuFrameMs = 0.0;   // CPU time begin→endFrame
	double   gpuFrameMs = -1.0;  // whole-frame GPU time, -1 = unavailable
	std::vector<ProfGpuPass>     gpuPasses;  // per-pass GPU time (the cost breakdown that matters)
	const char* gpuTimingMode = "";          // which backend path produced gpuPasses (see FrameGpuStats)
	ProfRenderStats              stats;
	std::vector<ProfScopeSample> scopes;     // CPU scope breakdown, in close order
};

// Lightweight always-on live sample (no CPU scopes) for the editor overview HUD +
// frame-time graph. Fed every frame by the app while the profiler window is open.
struct ProfLiveFrame
{
	double   deltaMs    = 0.0;    // frame pacing (1000/deltaMs = FPS)
	double   cpuFrameMs = 0.0;    // CPU frame-loop time
	double   gpuFrameMs = -1.0;   // whole-frame GPU time (-1 = unavailable)
	uint32_t draws = 0, triangles = 0, visible = 0, total = 0;
};

// Metadata recorded once per capture session (goes into the dump header).
struct ProfSessionInfo
{
	std::string backend  = "unknown";   // "OpenGL" / "Metal" / "D3D12" …
	std::string gpuName  = "unknown";
	std::string os       = "unknown";
	uint32_t    width    = 0;
	uint32_t    height   = 0;
	bool        vsync    = true;         // vsync state DURING the capture
	std::string note;
};

class HE_API EngineProfiler
{
public:
	static EngineProfiler& instance();

	// ── Control (may be called mid-frame; applied at the next beginFrame) ───
	// requestStart stashes the session info; the capture actually begins on the
	// next beginFrame so a frame is always recorded whole or not at all.
	void requestStart(const ProfSessionInfo& info, size_t maxFrames = 0);
	void requestStop();                 // stop + dump at next beginFrame
	// Start-if-idle / stop-if-recording in one call. Application does NOT use this
	// for its F9 handler: that path has to save the current vsync before starting
	// and restore it on stop, so it open-codes the branch around setVSync().
	void requestToggle(const ProfSessionInfo& info, size_t maxFrames = 0);

	bool isRecording()        const { return m_recording.load(std::memory_order_acquire); }
	bool isRecordingOrPending() const { return isRecording() || m_pending == Pending::Start; }
	// True only during the one frame of a single-frame capture (set at beginFrame,
	// cleared at endFrame). Backends with async GPU timing (OpenGL timer queries,
	// reaped 1–N frames late) use this to force a same-frame read for that one frame,
	// so the captured record gets THAT frame's GPU times rather than a stale slot.
	bool isSingleFrameCapture() const { return m_singleMode; }

	// ── Detailed GPU capture ───────────────────────────────────────────────
	// When on, backends that support it (Metal) submit each render pass as its own
	// command buffer to measure exclusive, additive per-pass GPU time. This
	// SERIALIZES the GPU (capture-only), so frame pacing is perturbed and the
	// per-pass numbers are costs-under-serialization (a reliable ranking + upper
	// bound), not the shipping single-command-buffer cost. Off = normal capture.
	// Read by the renderer on the main thread; settable any time.
	void setDetailedGpuCapture(bool on) { m_detailedGpu.store(on, std::memory_order_relaxed); }
	bool detailedGpuCapture() const     { return m_detailedGpu.load(std::memory_order_relaxed) || m_forceDetailed; }

	// ── Live overview (editor HUD) ──────────────────────────────────────────
	// While enabled, the app pushes one ProfLiveFrame per frame (cheap; no scopes).
	// The editor turns this on while its profiler window is open.
	void setLiveEnabled(bool on) { m_liveEnabled = on; }
	bool liveEnabled() const     { return m_liveEnabled; }
	void pushLive(const ProfLiveFrame& f);
	std::vector<ProfLiveFrame> liveSnapshot() const { return m_live; }
	double lastCpuFrameMs() const { return m_lastCpuFrameMs; }

	// ── Single-frame capture ────────────────────────────────────────────────
	// Capture exactly ONE frame in full detail (CPU scopes + detailed per-pass GPU,
	// forced on so the breakdown is exclusive) into an in-memory record for the UI —
	// no dump. Applied on the next beginFrame; the result is available right after.
	void requestSingleFrameCapture()       { m_pendingSingle = true; }
	const ProfFrameRecord* singleFrame() const { return m_haveSingleFrame ? &m_singleFrame : nullptr; }

	// True for exactly one frame after a dump was written; lets the app log the
	// path / restore vsync. Read via consumeJustDumped().
	bool consumeJustDumped(std::string& outPath);

	// ── Frame lifecycle (call once per frame from Application::Run) ─────────
	void beginFrame(double deltaMs);    // applies pending start/stop first
	void endFrame();

	// ── CPU scopes (prefer the HE_PROFILE_* macros / ProfileScope) ─────────
	void beginScope(const char* name);
	void endScope();

	// ── Per-frame data pushed before endFrame ──────────────────────────────
	void setRenderStats(const ProfRenderStats& s);
	void setGpuTimes(double gpuFrameMs, const std::vector<ProfGpuPass>& passes,
	                 const char* mode = "");

	// ── Per-thread timeline (v3) ───────────────────────────────────────────
	// Records every thread that opens a scope during a capture, not just the main
	// one — this is what makes JobSystem parallel_for work (FrustumCuller,
	// RenderExtractor) visible. On by default while recording; turn it off to get
	// the v2 behaviour (main-thread frame tree only) and skip the buffers entirely.
	void setThreadTimelineEnabled(bool on) { m_timelineOn.store(on, std::memory_order_relaxed); }
	bool threadTimelineEnabled() const     { return m_timelineOn.load(std::memory_order_relaxed); }

	// Merged copy of every registered thread's spans for the CURRENT capture, main
	// lane first. Safe to call while recording (per-buffer lock); costs a copy, so
	// the UI should call it on demand, not per frame.
	std::vector<ProfThreadTimeline> timelineSnapshot() const;
	// Frame boundaries along the same axis, for drawing frame separators.
	const std::vector<ProfFrameMark>& frameMarks() const { return m_frameMarks; }
	// Spans dropped across all threads because a per-thread cap was hit. Non-zero
	// means the timeline is truncated — reported, never silently swallowed.
	size_t timelineDroppedSpans() const;

	// ── Live view / export ─────────────────────────────────────────────────
	const ProfFrameRecord* lastFrame() const;
	std::vector<ProfFrameRecord> snapshot() const;   // copy for UI graphs
	size_t recordedFrames() const { return m_frames.size(); }
	const std::string& dumpsDir() const { return m_dumpsDir; }

	// Force a dump now (used by the editor "Dump" button). Returns the written
	// path, or "" if nothing was recorded. Does not stop recording.
	std::string dumpNow();

private:
	EngineProfiler() = default;

	void doStart();
	std::string doStopDump();

	struct LiveScope { const char* name; uint64_t startNs; uint32_t depth; };

	enum class Pending { None, Start, Stop };

	// Atomic: read by ProfileScope ctors on worker threads (e.g. the JobSystem
	// pool), written on the main thread at the frame boundary. m_captureThread is
	// the thread that owns the scope stack — scopes opened on any other thread are
	// ignored (see beginScope/endScope) so the single-threaded scope model holds.
	std::atomic<bool> m_recording{ false };
	std::atomic<bool> m_detailedGpu{ false };  // serialized per-pass GPU capture (Metal)
	std::atomic<bool> m_timelineOn{ true };    // per-thread timeline capture (v3)
	// Bumped on every doStart. A thread-local buffer carrying an older generation
	// belongs to a finished capture and is reset on first touch, so a worker that
	// slept through three captures cannot leak stale spans into the fourth.
	std::atomic<uint64_t> m_generation{ 0 };
	bool              m_forceDetailed = false; // detailed forced for a single-frame capture (main thread)
	std::thread::id   m_captureThread{};
	Pending         m_pending     = Pending::None;

	// Live overview (always-on when enabled; no scopes).
	bool                       m_liveEnabled   = false;
	std::vector<ProfLiveFrame> m_live;
	size_t                     m_liveCap       = 240;
	double                     m_lastCpuFrameMs = 0.0;

	// Single-frame capture.
	bool            m_pendingSingle   = false;
	bool            m_singleMode      = false;
	bool            m_haveSingleFrame = false;
	ProfFrameRecord m_singleFrame;
	ProfSessionInfo m_pendingInfo;
	size_t          m_pendingMax  = 0;

	ProfSessionInfo m_session;
	size_t          m_maxFrames   = 0;     // 0 = unlimited (grow)
	uint64_t        m_frameCounter = 0;
	uint64_t        m_sessionStartNs = 0;
	uint64_t        m_frameStartNs   = 0;

	std::vector<LiveScope>       m_stack;
	ProfFrameRecord              m_current;
	std::vector<ProfFrameRecord> m_frames;
	std::vector<ProfFrameMark>   m_frameMarks;   // frame boundaries on the timeline axis

	bool        m_justDumped = false;
	std::string m_lastDumpPath;
	std::string m_dumpsDir;
};

// ─── ProfileScope ────────────────────────────────────────────────────────────
// RAII scope timer. Latches the recording state at construction so a start/stop
// flipped mid-scope can never leave an unbalanced begin/endScope on the stack.
struct HE_API ProfileScope
{
	explicit ProfileScope(const char* name)
		: m_opened(EngineProfiler::instance().isRecording())
	{
		if (m_opened) EngineProfiler::instance().beginScope(name);
	}
	~ProfileScope()
	{
		if (m_opened) EngineProfiler::instance().endScope();
	}
	ProfileScope(const ProfileScope&)            = delete;
	ProfileScope& operator=(const ProfileScope&) = delete;
private:
	bool m_opened;
};
