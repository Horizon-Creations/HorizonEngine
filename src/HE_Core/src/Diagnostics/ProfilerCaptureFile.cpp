#include "Diagnostics/ProfilerCaptureFile.h"
#include "Diagnostics/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace HE::Prof
{

const char* LoadedCapture::intern(const std::string& s)
{
	auto it = m_interned.find(s);
	if (it != m_interned.end()) return it->second;
	m_strings.push_back(s);
	const char* p = m_strings.back().c_str();
	m_interned.emplace(s, p);
	return p;
}

double LoadedCapture::captureMs() const
{
	// Prefer the frame marks (they are the capture's own clock); fall back to the
	// summed frame times for a version-1 dump that has no marks.
	if (!frameMarks.empty()) return static_cast<double>(frameMarks.back().endNs) * 1e-6;
	double total = 0.0;
	for (const ProfFrameRecord& f : frames) total += f.deltaMs;
	return total;
}

void LoadedCapture::clear()
{
	path.clear(); fileName.clear(); captureNote.clear();
	session = ProfSessionInfo{};
	version = 0;
	// Records first, interner last: the records hold pointers into it.
	frames.clear(); threads.clear(); frameMarks.clear();
	aggregates.clear(); hitches.clear(); occupancy.clear();
	deltaStats = cpuStats = gpuStats = Percentiles{};
	fps = FpsLows{};
	haveGpu = false;
	timelineCutoffNs = timelineOmitted = 0;
	timelineTruncated = false;
	timelineCutoffKnown = false;
	gpuTimingModes.clear();
	m_interned.clear();
	m_strings.clear();
}

bool LoadedCapture::load(const std::string& file, std::string& error)
{
	clear();   // a failed load must leave nothing half-parsed behind
	error.clear();

	std::ifstream in(file);
	if (!in.is_open()) { error = "cannot open " + file; return false; }

	json j;
	try
	{
		in >> j;
	}
	catch (const std::exception& e)
	{
		error = std::string("not a readable profiler dump: ") + e.what();
		return false;
	}

	if (!j.is_object() || !j.contains("frames"))
	{
		error = "not a profiler dump (no frames array)";
		return false;
	}

	path     = file;
	fileName = fs::path(file).filename().string();
	version  = j.value("version", 1);

	// ── Session ─────────────────────────────────────────────────────────────
	// Everything read with .value() defaults: a version-1 dump is missing whole
	// blocks, and the viewer should open it rather than reject it.
	if (j.contains("session"))
	{
		const json& s = j["session"];
		session.backend = s.value("backend", std::string("unknown"));
		session.gpuName = s.value("gpu",     std::string("unknown"));
		session.os      = s.value("os",      std::string("unknown"));
		session.width   = s.value("width",  0u);
		session.height  = s.value("height", 0u);
		session.vsync   = s.value("vsync", true);
		session.note    = s.value("note", std::string());
		captureNote     = s.value("captureNote", std::string());
	}

	// ── Frames ──────────────────────────────────────────────────────────────
	frames.reserve(j["frames"].size());
	for (const json& jf : j["frames"])
	{
		ProfFrameRecord f;
		f.index      = jf.value("i", 0ull);
		f.wallMs     = jf.value("wallMs", 0.0);
		f.deltaMs    = jf.value("deltaMs", 0.0);
		f.cpuFrameMs = jf.value("cpuMs", 0.0);
		f.gpuFrameMs = jf.value("gpuMs", -1.0);
		if (jf.contains("gpuMode")) f.gpuTimingMode = intern(jf["gpuMode"].get<std::string>());

		// Scope order is NOT touched: the samples are stored in close order, and
		// both the self-time reconstruction and the Frame Detail tree read that
		// order. Sorting them here would quietly break both.
		if (jf.contains("cpu"))
			for (const json& js : jf["cpu"])
				f.scopes.push_back({ intern(js.value("n", std::string("?"))),
				                     js.value("ms", 0.0),
				                     js.value("d", 0u) });

		if (jf.contains("gpu"))
			for (const json& jg : jf["gpu"])
				f.gpuPasses.push_back({ intern(jg.value("n", std::string("?"))),
				                        jg.value("ms", 0.0),
				                        jg.value("approx", false) });

		if (jf.contains("stats"))
		{
			const json& st = jf["stats"];
			f.stats.drawCalls      = st.value("draws", 0u);
			f.stats.triangles      = st.value("tris", 0u);
			f.stats.visibleObjects = st.value("visible", 0u);
			f.stats.totalObjects   = st.value("total", 0u);
			f.stats.vramUsedMB     = st.value("vramUsedMB", 0.0);
			f.stats.vramBudgetMB   = st.value("vramBudgetMB", 0.0);
			f.stats.scene.entities      = st.value("entities", 0u);
			f.stats.scene.lights        = st.value("lights", 0u);
			f.stats.scene.particles     = st.value("particles", 0u);
			f.stats.scene.emitters      = st.value("emitters", 0u);
			f.stats.scene.gpuParticles  = st.value("gpuParticles", 0u);
			f.stats.scene.rigidBodies   = st.value("rigidBodies", 0u);
			f.stats.scene.audioSources  = st.value("audioSources", 0u);
			f.stats.scene.scripts       = st.value("scripts", 0u);
			f.stats.scene.streamingInFlight = st.value("streamingInFlight", 0u);
		}
		frames.push_back(std::move(f));
	}

	// ── Threads + frame marks (absent in version 1) ─────────────────────────
	if (j.contains("threads"))
	{
		for (const json& jt : j["threads"])
		{
			ProfThreadTimeline lane;
			lane.label  = jt.value("thread", std::string("?"));
			lane.isMain = jt.value("main", false);
			if (jt.contains("spans"))
				for (const json& js : jt["spans"])
					lane.spans.push_back({ intern(js.value("n", std::string("?"))),
					                       js.value("s", 0ull), js.value("e", 0ull),
					                       js.value("d", 0u) });
			threads.push_back(std::move(lane));
		}
	}
	if (j.contains("frameMarks"))
		for (const json& jm : j["frameMarks"])
			frameMarks.push_back({ jm.value("i", 0ull), jm.value("s", 0ull), jm.value("e", 0ull) });

	// ── What the writer recorded about its own limits ───────────────────────
	if (j.contains("summary"))
	{
		const json& s = j["summary"];
		timelineOmitted     = s.value("timelineTruncated", 0ull);
		timelineCutoffNs    = s.value("timelineCutoffNs", 0ull);
		timelineTruncated   = timelineOmitted > 0;
		timelineCutoffKnown = s.contains("timelineCutoffNs");
		if (s.contains("gpuTimingModes") && s["gpuTimingModes"].is_object())
			for (auto it = s["gpuTimingModes"].begin(); it != s["gpuTimingModes"].end(); ++it)
				gpuTimingModes[it.key()] = it.value().get<uint64_t>();
		// summary.threads carries the FULL per-lane totals even when threads[] is a
		// truncated prefix. Where a lane has no spans in the file at all, keep the
		// lane so the viewer still lists the thread instead of pretending it never ran.
		if (s.contains("threads"))
			for (const json& jt : s["threads"])
			{
				const std::string label = jt.value("thread", std::string());
				if (label.empty()) continue;
				const bool known = std::any_of(threads.begin(), threads.end(),
				                               [&](const ProfThreadTimeline& l) { return l.label == label; });
				if (!known)
				{
					ProfThreadTimeline lane;
					lane.label  = label;
					lane.isMain = jt.value("main", false);
					threads.push_back(std::move(lane));
				}
			}
	}

	// ── Derived views ───────────────────────────────────────────────────────
	// Recomputed rather than read from summary: a version-1 dump has no stats at
	// all, and recomputing guarantees the panel and the file cannot disagree.
	std::vector<double> deltas, cpus, gpus;
	deltas.reserve(frames.size()); cpus.reserve(frames.size());
	for (const ProfFrameRecord& f : frames)
	{
		deltas.push_back(f.deltaMs);
		cpus.push_back(f.cpuFrameMs);
		if (f.gpuFrameMs >= 0.0) gpus.push_back(f.gpuFrameMs);
	}
	deltaStats = computePercentiles(deltas);
	cpuStats   = computePercentiles(cpus);
	haveGpu    = !gpus.empty();
	if (haveGpu) gpuStats = computePercentiles(gpus);
	fps        = computeFpsLows(deltas);
	aggregates = aggregateScopes(frames);
	hitches    = findHitches(frames);
	occupancy  = laneOccupancy(threads, captureMs());

	if (frames.empty() && threads.empty())
	{
		error = "dump contains no frames and no thread timeline";
		return false;
	}

	HE_LOG_INFO(Profiler, "%s",
	            ("Profiler: loaded capture " + fileName + " (" + std::to_string(frames.size()) +
	             " frames, " + std::to_string(threads.size()) + " lanes)").c_str());
	return true;
}

} // namespace HE::Prof
