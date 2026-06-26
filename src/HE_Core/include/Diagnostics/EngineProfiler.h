#pragma once
// ─── EngineProfiler ──────────────────────────────────────────────────────────
// Runtime, start/stop-able self-diagnostic profiler. Independent of Tracy.
//
//  • Zero cost when not recording (one bool check per scope).
//  • CPU scope timers (nested, RAII via ProfileScope / the HE_PROFILE_* macros).
//  • Per-frame render counters + per-pass GPU times pushed by the active backend.
//  • Dumps a structured JSON report to <deploy>/dumps/ on stop.
//
// Single-threaded scope model: the frame loop, scene-system tick and editor UI all
// run on the main thread, so begin/endFrame, snapshot() and the dump are never
// called concurrently. Scopes, however, CAN be opened on worker threads — the
// JobSystem pool wraps every task in HE_PROFILE_SCOPE_N("Job::Execute") — so
// begin/endScope record ONLY from the capture (main) thread and no-op on any
// other; this keeps the shared scope stack single-writer (otherwise concurrent
// push_back corrupts the heap). Worker-thread Tracy zones still work. (A real
// per-thread job-timeline capture is future work — see docs/profiler-design.md.)

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
	void requestToggle(const ProfSessionInfo& info, size_t maxFrames = 0);

	bool isRecording()        const { return m_recording.load(std::memory_order_acquire); }
	bool isRecordingOrPending() const { return isRecording() || m_pending == Pending::Start; }

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
	double lastDeltaMs()    const { return m_lastDeltaMs; }

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
	bool              m_forceDetailed = false; // detailed forced for a single-frame capture (main thread)
	std::thread::id   m_captureThread{};
	Pending         m_pending     = Pending::None;

	// Live overview (always-on when enabled; no scopes).
	bool                       m_liveEnabled   = false;
	std::vector<ProfLiveFrame> m_live;
	size_t                     m_liveCap       = 240;
	double                     m_lastCpuFrameMs = 0.0;
	double                     m_lastDeltaMs    = 0.0;

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
