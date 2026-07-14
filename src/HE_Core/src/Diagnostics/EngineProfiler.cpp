#include "Diagnostics/EngineProfiler.h"
#include <cstdint>
#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Logger.h"

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
	m_frameCounter   = 0;
	m_sessionStartNs = nowNs();
	m_stack.clear();
	m_dumpsDir       = GlobalState::getInstance().getDumpsDir();
	// The thread that runs doStart (the frame loop / main thread) owns the scope
	// stack; scopes from any other thread are ignored. Set before publishing
	// m_recording so worker threads that observe recording also see this id.
	m_captureThread  = std::this_thread::get_id();
	m_recording.store(true, std::memory_order_release);
	Logger::Log(Logger::LogLevel::Info, "Profiler: capture started");
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
	m_lastDeltaMs  = deltaMs;
	if (!m_recording) return;

	m_current           = ProfFrameRecord{};
	m_current.index     = m_frameCounter;
	m_current.deltaMs   = deltaMs;
	m_current.wallMs    = nsToMs(m_frameStartNs - m_sessionStartNs);
	m_stack.clear();
}

void EngineProfiler::endFrame()
{
	m_lastCpuFrameMs = nsToMs(nowNs() - m_frameStartNs);   // always (live HUD)
	if (!m_recording) return;

	// Close any scopes the caller forgot to (defensive; should not happen).
	while (!m_stack.empty()) endScope();

	m_current.cpuFrameMs = m_lastCpuFrameMs;

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
		Logger::Log(Logger::LogLevel::Info, "Profiler: single-frame capture taken");
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

// Only the capture (main) thread records — scopes opened on worker threads (e.g.
// the JobSystem pool's "Job::Execute") are ignored so the scope stack stays
// single-writer. Without this guard, concurrent push_back corrupts the heap.
void EngineProfiler::beginScope(const char* name)
{
	if (!m_recording.load(std::memory_order_acquire)) return;
	if (std::this_thread::get_id() != m_captureThread)  return;
	m_stack.push_back({ name, nowNs(), static_cast<uint32_t>(m_stack.size()) });
}

void EngineProfiler::endScope()
{
	if (!m_recording.load(std::memory_order_acquire)) return;
	if (std::this_thread::get_id() != m_captureThread)  return;
	if (m_stack.empty()) return;
	const LiveScope s = m_stack.back();
	m_stack.pop_back();
	m_current.scopes.push_back({ s.name, nsToMs(nowNs() - s.startNs), s.depth });
}

// ─── Per-frame data ──────────────────────────────────────────────────────────

void EngineProfiler::setRenderStats(const ProfRenderStats& s)
{
	if (m_recording) m_current.stats = s;
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
	Logger::Log(Logger::LogLevel::Info,
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
	std::map<std::string, Stat> cpuSummary, gpuSummary;
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
		for (const auto& s : f.scopes) cpuSummary[s.name].add(s.ms);
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
	j["version"] = 1;
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
	json cpuScopes = json::object();
	for (const auto& [name, st] : cpuSummary) cpuScopes[name] = st.toJson();
	summary["cpuScopes"] = cpuScopes;
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
	j["summary"] = summary;

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
			{"entities", s.entities}, {"lights", s.lights},
			{"particles", s.particles}, {"gpuParticles", s.gpuParticles},
			{"streamingInFlight", s.streamingInFlight},
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
		Logger::Log(Logger::LogLevel::Error,
		            ("Profiler: failed to open dump file " + out.string()).c_str());
		return "";
	}
	f << j.dump(1, '\t');
	f.close();
	Logger::Log(Logger::LogLevel::Info, ("Profiler: wrote " + out.string()).c_str());
	return out.string();
}
