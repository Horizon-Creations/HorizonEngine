#pragma once
// ─── GpuPassAccumulator ──────────────────────────────────────────────────────
// Collects per-pass GPU timings for the profiler's "detailed capture" mode, where
// each render pass is submitted as its OWN command buffer so its GPUStartTime/
// GPUEndTime bracket exclusive, additive GPU time (the only reliable per-pass GPU
// measurement on tile-deferred / Apple-Silicon GPUs — stage-boundary counter
// spans overlap there and cannot be summed).
//
// Those command buffers complete asynchronously, OUT OF ORDER, and 1–2 frames
// late, possibly interleaved across frames. report() is called once per completed
// command buffer; when all `expected` passes of a frame have reported, that
// frame's FrameGpuStats is produced. Newer frames win, so a late straggler from an
// old frame never clobbers a fresher result. Thread-safe (called from Metal
// completion handlers on background threads; read on the main thread).
//
// Pure C++ + a mutex on purpose: this is the subtle bookkeeping, so it lives here
// (not buried in an Obj-C++ block) and is unit-tested directly.

#include "Renderer/IRenderer.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// Header-only / all-inline: shared by HE_Core, the Metal backend, and the tests
// without an exported symbol, so no HE_API.
class GpuPassAccumulator
{
public:
	// Report one completed pass command buffer.
	//   frame     — monotonically rising frame index the pass belongs to.
	//   name      — static string literal (stored by pointer; the backend passes
	//               the same literals it uses for the pass names).
	//   startSec  — command-buffer GPUStartTime (seconds). endSec — GPUEndTime.
	//               endSec <= startSec means the buffer ran but measured ~0 (e.g. an
	//               empty pass, like a shadow map with no casters) — it still counts
	//               toward `expected` so the frame can complete.
	//   expected  — how many passes this frame will report in total.
	//   outDone   — filled with the frame's stats iff the return value is true.
	// Returns true exactly once per frame: when this report completes it.
	bool report(uint64_t frame, const char* name,
	            double startSec, double endSec, int expected,
	            IRenderer::FrameGpuStats& outDone);

	// Newest fully-completed frame's stats (gpuFrameMs < 0 until one completes).
	IRenderer::FrameGpuStats latest() const;

	// In-flight (not-yet-complete) frame count — for tests / diagnostics.
	size_t inflightCount() const;

private:
	struct Accum
	{
		std::vector<IRenderer::GpuPassTime> passes;
		double sumMs    = 0.0;     // Σ of per-pass exclusive times (the detailed total)
		bool   any      = false;   // at least one pass had a real (end>start) span
		int    reported = 0;
		int    expected = 0;
	};

	// Drop in-flight frames more than this many indices behind the newest report,
	// so a lost completion (buffer that never fires) can't leak the map forever.
	static constexpr uint64_t kStaleGap = 8;

	mutable std::mutex        m_mutex;
	std::map<uint64_t, Accum> m_inflight;
	IRenderer::FrameGpuStats  m_last;            // newest completed frame
	uint64_t                  m_lastPublished = 0;
	bool                      m_havePublished = false;
};

// ── Inline implementation (header-only so HE_Core and tests share one copy) ────

inline bool GpuPassAccumulator::report(uint64_t frame, const char* name,
                                       double startSec, double endSec, int expected,
                                       IRenderer::FrameGpuStats& outDone)
{
	std::lock_guard<std::mutex> lk(m_mutex);

	Accum& a   = m_inflight[frame];
	a.expected = expected;

	const double ms = (endSec > startSec) ? (endSec - startSec) * 1000.0 : 0.0;
	a.passes.push_back({ name, ms, /*approx=*/false });
	a.sumMs += ms;
	if (endSec > startSec) a.any = true;
	++a.reported;

	bool completed = false;
	if (a.expected > 0 && a.reported >= a.expected)
	{
		IRenderer::FrameGpuStats fs;
		fs.passes     = a.passes;
		// Detailed-capture serializes the passes (waitUntilCompleted), so they are
		// exclusive → the meaningful frame GPU total is their SUM, not a wall-span
		// (which would include the inter-pass CPU stalls).
		fs.gpuFrameMs = a.any ? a.sumMs : -1.0;

		// Newest frame wins: a straggler from an older frame never overwrites a
		// fresher published result.
		if (!m_havePublished || frame >= m_lastPublished)
		{
			m_last          = fs;
			m_lastPublished = frame;
			m_havePublished = true;
		}
		outDone   = fs;
		completed = true;
		m_inflight.erase(frame);
	}

	// Garbage-collect frames that never completed (a lost/dropped command buffer),
	// keyed off the newest index we've seen.
	if (frame >= kStaleGap)
	{
		const uint64_t cutoff = frame - kStaleGap;
		for (auto it = m_inflight.begin(); it != m_inflight.end(); )
		{
			if (it->first < cutoff) it = m_inflight.erase(it);
			else                    ++it;
		}
	}
	return completed;
}

inline IRenderer::FrameGpuStats GpuPassAccumulator::latest() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_last;
}

inline size_t GpuPassAccumulator::inflightCount() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_inflight.size();
}
