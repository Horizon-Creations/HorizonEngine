#pragma once
// ─── ProfilerCaptureFile ─────────────────────────────────────────────────────
// Reads a profiler dump (<deploy>/dumps/profile_*.json) back into the same
// structures a live capture produces, so a recorded run can be inspected in the
// editor with the views that already exist — no separate viewer, no second set of
// drawing code.
//
// In HE_Core rather than HE_Editor on purpose: HE_Core already links nlohmann,
// the unit tests link HorizonCore (so the dump→load round trip is testable
// headlessly), and a game-side viewer could reuse this untouched.
//
// Lifetime, the one thing to get right: ProfScopeSample/ProfGpuPass/ProfThreadSpan
// all store `const char*` and assume static storage, because a live capture only
// ever passes string literals. A loaded file has no literals, so LoadedCapture owns
// an interner and hands out pointers into it. Those pointers are valid exactly as
// long as the LoadedCapture is — hence the deleted copy, and hence loading in place
// rather than returning by value.

#include "Types/Defines.h"
#include "Diagnostics/EngineProfiler.h"
#include "Diagnostics/ProfilerStats.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE::Prof
{

class HE_API LoadedCapture
{
public:
	LoadedCapture() = default;
	LoadedCapture(const LoadedCapture&)            = delete;   // interned pointers
	LoadedCapture& operator=(const LoadedCapture&) = delete;

	// ── Contents (empty until load() succeeds) ──────────────────────────────
	std::string     path;          // file it came from
	std::string     fileName;      // basename, for the banner
	ProfSessionInfo session;
	std::string     captureNote;   // session.captureNote from the dump
	int             version = 0;   // dump format version (1 = pre-timeline)

	std::vector<ProfFrameRecord>    frames;
	std::vector<ProfThreadTimeline> threads;
	std::vector<ProfFrameMark>      frameMarks;

	// Recomputed on load from `frames`/`threads` with the same pure functions the
	// live views use. The dump's own `summary` is deliberately IGNORED: recomputing
	// makes a version-1 dump (no stats block at all) display identically to a
	// current one, and removes any chance of the file and the panel disagreeing.
	std::vector<ScopeAggregate> aggregates;
	std::vector<Hitch>          hitches;
	std::vector<LaneOccupancy>  occupancy;
	Percentiles                 deltaStats, cpuStats, gpuStats;
	FpsLows                     fps;
	bool                        haveGpu = false;

	// Truncation the WRITER recorded, carried through so the viewer can say the
	// timeline is only a prefix instead of implying the threads went quiet.
	uint64_t timelineCutoffNs   = 0;
	uint64_t timelineOmitted    = 0;
	bool     timelineTruncated  = false;
	// False for dumps written before the cutoff was recorded: those were truncated
	// lane by lane, so there IS no shared timestamp and the worker lanes can be
	// empty while the thread was busy. The viewer has to say that differently —
	// quoting a cutoff of 0.0 ms would be worse than saying nothing.
	bool     timelineCutoffKnown = false;
	// Which GPU-timing path produced the capture (frames per mode). Drives the same
	// honesty rules as live: "counter" spans overlap and must not be summed.
	std::unordered_map<std::string, uint64_t> gpuTimingModes;

	// Parse `file` into this object, replacing whatever it held. On failure returns
	// false and fills `error`; the object is left empty rather than half-populated.
	bool load(const std::string& file, std::string& error);

	bool   empty() const { return frames.empty() && threads.empty(); }
	double captureMs() const;
	// Drop everything, including the interned strings. Assignment cannot do this
	// (the deleted copy takes move-assign with it, and moving the interner would be
	// a trap anyway), so a reload clears explicitly.
	void clear();

private:
	// Pointer-stable string storage: deque never relocates existing elements on
	// push_back, so every c_str() handed out stays valid for the object's life.
	std::deque<std::string>                        m_strings;
	std::unordered_map<std::string, const char*>   m_interned;
	const char* intern(const std::string& s);
};

} // namespace HE::Prof
