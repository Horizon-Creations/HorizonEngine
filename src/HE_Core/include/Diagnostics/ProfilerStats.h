#pragma once
// ─── ProfilerStats ───────────────────────────────────────────────────────────
// Pure analysis over captured EngineProfiler data: percentiles, low-percentile
// FPS, hitch detection, per-scope self time, histogram binning.
//
// Header-only and free of SDL/ImGui/filesystem on purpose — every function here
// is a value-in/value-out transform, so the dump writer, the editor panel and the
// unit tests all call the SAME code, and a machine without a display (this one)
// can still verify the numbers a graph will later draw.
//
// Why this exists at all: v1/v2 summarised each series as min/avg/max. That
// answers "how fast on average", which is the question nobody has. Stutter lives
// in the tail — a 11 ms mean over a bimodal 8/22 ms distribution feels like 22 ms
// — so the tail is what gets computed here.

#include "Diagnostics/EngineProfiler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE::Prof
{

// ─── Percentiles ─────────────────────────────────────────────────────────────

struct Percentiles
{
	double   min    = 0.0;
	double   max    = 0.0;
	double   mean   = 0.0;
	double   stddev = 0.0;
	double   p50    = 0.0;   // median
	double   p95    = 0.0;
	double   p99    = 0.0;
	size_t   count  = 0;
};

// Nearest-rank percentile on a sorted copy: p_k = v[ceil(k/100 * n) - 1]. No
// interpolation — with frame times the ranked sample IS a frame that happened,
// and an interpolated 16.4 ms that no frame ever took is harder to act on.
inline double percentileOfSorted(const std::vector<double>& sorted, double pct)
{
	if (sorted.empty()) return 0.0;
	const double rank = std::ceil(pct / 100.0 * static_cast<double>(sorted.size()));
	size_t idx = rank <= 1.0 ? 0 : static_cast<size_t>(rank) - 1;
	if (idx >= sorted.size()) idx = sorted.size() - 1;
	return sorted[idx];
}

inline Percentiles computePercentiles(std::vector<double> values)
{
	Percentiles out;
	if (values.empty()) return out;
	std::sort(values.begin(), values.end());
	out.count = values.size();
	out.min   = values.front();
	out.max   = values.back();

	double sum = 0.0;
	for (double v : values) sum += v;
	out.mean = sum / static_cast<double>(values.size());

	double sq = 0.0;
	for (double v : values) { const double d = v - out.mean; sq += d * d; }
	out.stddev = std::sqrt(sq / static_cast<double>(values.size()));

	out.p50 = percentileOfSorted(values, 50.0);
	out.p95 = percentileOfSorted(values, 95.0);
	out.p99 = percentileOfSorted(values, 99.0);
	return out;
}

// ─── Low-percentile FPS ──────────────────────────────────────────────────────

struct FpsLows
{
	double avgFps   = 0.0;
	double low1Fps  = 0.0;   // "1% low"
	double low01Fps = 0.0;   // "0.1% low"
	size_t frames   = 0;
};

// The benchmark convention (CapFrameX / GamersNexus style): the 1% low is the
// AVERAGE of the slowest 1% of frames expressed as FPS — not the 99th-percentile
// frame time. The two differ, and mixing them is how two people compare numbers
// and disagree, so this file commits to the averaged-worst-bucket definition and
// says so. avgFps is derived from the MEAN frame time (harmonic w.r.t. per-frame
// FPS), which is the only average that a frame count divided by a duration agrees
// with; averaging per-frame FPS values instead over-weights the fast frames.
inline FpsLows computeFpsLows(std::vector<double> frameMs)
{
	FpsLows out;
	frameMs.erase(std::remove_if(frameMs.begin(), frameMs.end(),
	                             [](double v) { return !(v > 0.0); }),
	              frameMs.end());
	if (frameMs.empty()) return out;

	std::sort(frameMs.begin(), frameMs.end());   // ascending: slowest frames last
	out.frames = frameMs.size();

	double sum = 0.0;
	for (double v : frameMs) sum += v;
	out.avgFps = 1000.0 / (sum / static_cast<double>(frameMs.size()));

	// At least one frame in each bucket, so a short capture reports the worst
	// frame rather than 0 — an empty bucket read as "0 FPS" would be a lie.
	auto worstBucketFps = [&frameMs](double fraction) {
		const size_t n = std::max<size_t>(1, static_cast<size_t>(
			                    static_cast<double>(frameMs.size()) * fraction));
		double s = 0.0;
		for (size_t i = frameMs.size() - n; i < frameMs.size(); ++i) s += frameMs[i];
		const double avgMs = s / static_cast<double>(n);
		return avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
	};
	out.low1Fps  = worstBucketFps(0.01);
	out.low01Fps = worstBucketFps(0.001);
	return out;
}

// ─── Hitches ─────────────────────────────────────────────────────────────────

struct Hitch
{
	uint64_t    frameIndex   = 0;
	double      deltaMs      = 0.0;
	double      ratio        = 0.0;   // deltaMs / median
	const char* worstScope   = "";    // costliest top-level CPU scope in that frame
	double      worstScopeMs = 0.0;
	double      gpuMs        = -1.0;  // so a reader can tell a GPU spike from a CPU one
};

// A hitch is a frame whose wall time exceeds `factor` x the capture median. The
// median (not the mean) is the baseline on purpose: a handful of 200 ms stalls
// drags a mean up far enough to hide the very frames being looked for.
//
// Each hitch carries the costliest DEPTH-0 scope of that frame. Depth 0 because a
// nested scope is already inside its parent's time — reporting the deepest one
// would name a leaf while the actual stall sits in a sibling subtree.
inline std::vector<Hitch> findHitches(const std::vector<ProfFrameRecord>& frames,
                                      double factor = 2.0, size_t maxReported = 64)
{
	std::vector<Hitch> out;
	if (frames.empty()) return out;

	std::vector<double> deltas;
	deltas.reserve(frames.size());
	for (const ProfFrameRecord& f : frames)
		if (f.deltaMs > 0.0) deltas.push_back(f.deltaMs);
	if (deltas.empty()) return out;
	std::sort(deltas.begin(), deltas.end());
	const double median = percentileOfSorted(deltas, 50.0);
	if (!(median > 0.0)) return out;

	const double threshold = median * factor;
	for (const ProfFrameRecord& f : frames)
	{
		if (!(f.deltaMs > threshold)) continue;
		Hitch h;
		h.frameIndex = f.index;
		h.deltaMs    = f.deltaMs;
		h.ratio      = f.deltaMs / median;
		h.gpuMs      = f.gpuFrameMs;
		for (const ProfScopeSample& s : f.scopes)
			if (s.depth == 0 && s.ms > h.worstScopeMs) { h.worstScopeMs = s.ms; h.worstScope = s.name; }
		out.push_back(h);
	}
	// Worst first, then cap: with a pathological capture every frame can be a
	// hitch, and a report that lists 20 000 of them reports nothing.
	std::sort(out.begin(), out.end(),
	          [](const Hitch& a, const Hitch& b) { return a.deltaMs > b.deltaMs; });
	if (out.size() > maxReported) out.resize(maxReported);
	return out;
}

// ─── Per-scope aggregate with SELF time ──────────────────────────────────────

struct ScopeAggregate
{
	std::string name;
	double      totalMs = 0.0;   // inclusive, summed over the capture
	double      selfMs  = 0.0;   // inclusive minus direct children
	uint64_t    count   = 0;     // times entered
	double      minMs   = 0.0;   // per-call inclusive
	double      maxMs   = 0.0;
	double      avgMs   = 0.0;
	double      p95Ms   = 0.0;
	uint32_t    minDepth = 0;    // shallowest depth this scope was ever seen at
};

// Self time from the recorded stream alone — no extra instrumentation.
//
// ProfFrameRecord::scopes is in CLOSE order with each entry's nesting depth, so a
// scope's DIRECT children are exactly the entries immediately preceding it that
// sit at depth+1, scanning backwards until an entry at depth <= its own (that is
// the previous sibling, or the parent's other subtree — either way the boundary).
// Deeper entries encountered on the way are grandchildren and are skipped, since
// their time is already inside a direct child.
//
// Self time is the number that points at the culprit: "Render 9 ms" only says the
// renderer is in the call chain, while "Render self 8.6 ms" says the cost is in
// Render's own body and not in anything it called.
inline void accumulateFrameScopes(const std::vector<ProfScopeSample>& scopes,
                                  std::unordered_map<std::string, ScopeAggregate>& agg,
                                  std::unordered_map<std::string, std::vector<double>>& samples)
{
	for (size_t i = 0; i < scopes.size(); ++i)
	{
		const ProfScopeSample& s = scopes[i];
		double childMs = 0.0;
		for (size_t k = i; k-- > 0;)
		{
			if (scopes[k].depth <= s.depth) break;              // sibling / boundary
			if (scopes[k].depth == s.depth + 1) childMs += scopes[k].ms;   // direct child
			// deeper than depth+1 → grandchild, already inside a direct child
		}

		const std::string key(s.name ? s.name : "?");
		ScopeAggregate& a = agg[key];
		if (a.count == 0) { a.name = key; a.minMs = s.ms; a.maxMs = s.ms; a.minDepth = s.depth; }
		else              { a.minMs = std::min(a.minMs, s.ms); a.maxMs = std::max(a.maxMs, s.ms);
		                    a.minDepth = std::min(a.minDepth, s.depth); }
		a.totalMs += s.ms;
		// Clamp at zero: timer noise on sub-microsecond scopes can make the
		// children sum a hair above the parent, and a negative "self time" in a
		// sorted table looks like a bug in the profiler rather than in the frame.
		a.selfMs  += std::max(0.0, s.ms - childMs);
		++a.count;
		samples[key].push_back(s.ms);
	}
}

// Aggregate every frame of a capture, sorted by self time descending — the order
// that puts the thing worth fixing on the first row.
inline std::vector<ScopeAggregate> aggregateScopes(const std::vector<ProfFrameRecord>& frames)
{
	std::unordered_map<std::string, ScopeAggregate>     agg;
	std::unordered_map<std::string, std::vector<double>> samples;
	for (const ProfFrameRecord& f : frames)
		accumulateFrameScopes(f.scopes, agg, samples);

	std::vector<ScopeAggregate> out;
	out.reserve(agg.size());
	for (auto& [key, a] : agg)
	{
		a.avgMs = a.count ? a.totalMs / static_cast<double>(a.count) : 0.0;
		std::vector<double>& v = samples[key];
		std::sort(v.begin(), v.end());
		a.p95Ms = percentileOfSorted(v, 95.0);
		out.push_back(a);
	}
	std::sort(out.begin(), out.end(), [](const ScopeAggregate& a, const ScopeAggregate& b) {
		if (a.selfMs != b.selfMs) return a.selfMs > b.selfMs;
		return a.name < b.name;               // stable, so the table never jitters
	});
	return out;
}

// ─── Histogram ───────────────────────────────────────────────────────────────

struct Histogram
{
	double                lo       = 0.0;   // left edge of bin 0
	double                hi       = 0.0;   // right edge of the last bin
	double                binWidth = 0.0;
	uint32_t              maxCount = 0;     // tallest bin (the graph's Y scale)
	std::vector<uint32_t> bins;

	// Left edge of bin i — the axis labels a renderer needs.
	double binStart(size_t i) const { return lo + binWidth * static_cast<double>(i); }
};

// Equal-width binning over [min, max]. Values landing exactly on `hi` go into the
// last bin rather than off the end. A degenerate range (all values equal) yields a
// single populated bin instead of a division by zero.
inline Histogram computeHistogram(const std::vector<double>& values, size_t binCount = 32)
{
	Histogram h;
	if (values.empty() || binCount == 0) return h;
	h.bins.assign(binCount, 0u);
	h.lo = h.hi = values.front();
	for (double v : values) { h.lo = std::min(h.lo, v); h.hi = std::max(h.hi, v); }

	if (!(h.hi > h.lo))
	{
		h.binWidth = 0.0;
		h.bins[0]  = static_cast<uint32_t>(values.size());
		h.maxCount = h.bins[0];
		return h;
	}

	h.binWidth = (h.hi - h.lo) / static_cast<double>(binCount);
	for (double v : values)
	{
		size_t idx = static_cast<size_t>((v - h.lo) / h.binWidth);
		if (idx >= binCount) idx = binCount - 1;   // v == hi
		++h.bins[idx];
	}
	for (uint32_t c : h.bins) h.maxCount = std::max(h.maxCount, c);
	return h;
}

// ─── Timeline lane occupancy ─────────────────────────────────────────────────

struct LaneOccupancy
{
	std::string label;
	bool        isMain      = false;
	double      busyMs      = 0.0;   // union of depth-0 spans
	double      spanMs      = 0.0;   // first start → last end on this lane
	double      utilisation = 0.0;   // busyMs / captureMs, 0..1
	size_t      spanCount   = 0;
	uint32_t    maxDepth    = 0;
};

// Only depth-0 spans count toward busy time: nested spans are inside their parent
// already, and summing all depths reports several hundred percent occupancy on a
// deeply instrumented thread. `captureMs` is the wall time of the whole capture,
// so utilisation answers "how much of the capture was this worker actually doing
// something" — the number that turns an eight-lane timeline into a verdict.
inline std::vector<LaneOccupancy> laneOccupancy(const std::vector<ProfThreadTimeline>& lanes,
                                                double captureMs)
{
	std::vector<LaneOccupancy> out;
	out.reserve(lanes.size());
	for (const ProfThreadTimeline& lane : lanes)
	{
		LaneOccupancy o;
		o.label     = lane.label;
		o.isMain    = lane.isMain;
		o.spanCount = lane.spans.size();
		uint64_t first = UINT64_MAX, last = 0;
		for (const ProfThreadSpan& s : lane.spans)
		{
			if (s.depth == 0) o.busyMs += static_cast<double>(s.endNs - s.startNs) * 1e-6;
			o.maxDepth = std::max(o.maxDepth, s.depth);
			first = std::min(first, s.startNs);
			last  = std::max(last, s.endNs);
		}
		if (first != UINT64_MAX) o.spanMs = static_cast<double>(last - first) * 1e-6;
		o.utilisation = captureMs > 0.0 ? o.busyMs / captureMs : 0.0;
		out.push_back(std::move(o));
	}
	return out;
}

} // namespace HE::Prof
