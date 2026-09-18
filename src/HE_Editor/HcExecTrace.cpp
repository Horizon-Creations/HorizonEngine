#include "HcExecTrace.h"
#include "LevelScriptPanel.h"    // kTabPath — the Level Script tab's reserved path
#include "GameInstancePanel.h"   // kTabPath — the Game Instance tab's

#include <HorizonCode/HorizonCodeRuntime.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace HcExecTrace
{
namespace
{
	// Per graph (tab key): node id → when it last ran, in seconds on the
	// steady clock, plus the instance that ran it. Bounded by the nodes of the
	// graphs that ran, which is the size of the project's scripts.
	struct GraphHits
	{
		std::unordered_map<int, double> lastRun;
		uint32_t                        lastInstance = 0;
	};
	std::unordered_map<std::string, GraphHits> s_hits;

	// The pending reveal. `tab` empty = none. `tabTaken` is the shell having
	// acted on it: the node half stays until the panel takes it, however many
	// frames the tab needs to appear.
	std::string s_revealTab;
	int         s_revealNode = 0;
	bool        s_revealTabTaken = false;

	double nowSeconds()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	// Breakpoints per graph (tab key). A set, not a map to a flag: the canvas
	// asks "is there one" per node per frame, and an absent node costs a
	// lookup in a set that is as small as the number of breakpoints.
	std::unordered_map<std::string, std::set<int>> s_breakpoints;
	// The file the set is mirrored into after every change; empty = none.
	std::string s_breakpointStore;

	// Write the set to the store, if there is one. Best effort: a Saved/
	// directory that cannot be written costs the breakpoints of the next
	// session, not this one, and the set in memory stays authoritative.
	void persistBreakpoints()
	{
		if (s_breakpointStore.empty()) return;
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::create_directories(fs::path(s_breakpointStore).parent_path(), ec);
		std::ofstream out(s_breakpointStore, std::ios::binary | std::ios::trunc);
		if (out) out << saveBreakpointsJson();
	}

	// The stop: where the first stopped run stands. `tab` empty = none.
	std::string s_pausedTab;
	int         s_pausedNode     = 0;
	uint32_t    s_pausedInstance = 0;
	bool        s_breakHit       = false;
	// The runtime attach() was given, for refreshPaused. Null in tests.
	HorizonCode::Runtime* s_runtime = nullptr;
}

std::string tabKeyFor(const std::string& runtimeKey)
{
	if (runtimeKey.empty()) return {};
	// The GameInstance's key is fixed (HorizonCodeRuntime.cpp, kGameInstanceIdentity).
	if (runtimeKey == "__game_instance__") return GameInstancePanel::kTabPath;
	// A level script's key carries the scene's uuid (HorizonWorld::levelScriptKey);
	// the editor shows the current scene's, and there is one tab for it.
	if (runtimeKey.rfind("level:", 0) == 0) return LevelScriptPanel::kTabPath;
	// Already a tab key, or a content-relative asset path: as is.
	return runtimeKey;
}

void attach(HorizonCode::Runtime& rt)
{
	s_runtime = &rt;
	rt.setExecListener([](HorizonCode::InstanceId instance, const std::string& classKey,
	                      size_t /*level*/, int nodeId)
	{ recordHit(classKey, nodeId, instance); });
	rt.setBreakPredicate([](HorizonCode::InstanceId, const std::string& classKey,
	                        size_t /*level*/, int nodeId)
	{ return shouldBreakAt(classKey, nodeId); });
	rt.setSuspendListener([](const HorizonCode::Runtime::SuspendedSite& s)
	{ recordStop(s.classKey, s.nodeId, s.instance); });
}

void detach() { s_runtime = nullptr; }

HorizonCode::Runtime* attachedRuntime() { return s_runtime; }

void recordHit(const std::string& runtimeKey, int nodeId, uint32_t instance)
{
	recordHitAt(runtimeKey, nodeId, instance, nowSeconds());
}

void recordHitAt(const std::string& runtimeKey, int nodeId, uint32_t instance, double atSeconds)
{
	// An instance registered without a key (a bare test graph) has no tab to
	// light up; recording it would only grow a map nobody reads.
	if (runtimeKey.empty() || nodeId == 0) return;
	GraphHits& g = s_hits[tabKeyFor(runtimeKey)];
	g.lastRun[nodeId] = atSeconds;
	g.lastInstance    = instance;
}

float glowOf(const std::string& tabKey, int nodeId)
{
	return glowAt(tabKey, nodeId, nowSeconds());
}

float glowAt(const std::string& tabKey, int nodeId, double nowSeconds_)
{
	const auto g = s_hits.find(tabKey);
	if (g == s_hits.end()) return 0.0f;
	const auto n = g->second.lastRun.find(nodeId);
	if (n == g->second.lastRun.end()) return 0.0f;
	const double age = nowSeconds_ - n->second;
	if (age < 0.0) return 1.0f;                      // a clock that went backwards: still "just now"
	if (age >= kFadeSeconds) return 0.0f;
	return 1.0f - static_cast<float>(age / kFadeSeconds);
}

uint32_t lastInstanceOf(const std::string& tabKey)
{
	const auto g = s_hits.find(tabKey);
	return g == s_hits.end() ? 0u : g->second.lastInstance;
}

void clearHits() { s_hits.clear(); }

// ── Breakpoints ──────────────────────────────────────────────────────────────
void setBreakpoint(const std::string& tabKey, int nodeId, bool on)
{
	const std::string tab = tabKeyFor(tabKey);
	if (tab.empty() || nodeId == 0) return;
	if (on)
	{
		if (s_breakpoints[tab].insert(nodeId).second) persistBreakpoints();
		return;
	}
	const auto g = s_breakpoints.find(tab);
	if (g == s_breakpoints.end() || g->second.erase(nodeId) == 0) return;
	if (g->second.empty()) s_breakpoints.erase(g);
	persistBreakpoints();
}

void toggleBreakpoint(const std::string& tabKey, int nodeId)
{
	setBreakpoint(tabKey, nodeId, !hasBreakpoint(tabKey, nodeId));
}

bool hasBreakpoint(const std::string& tabKey, int nodeId)
{
	if (s_breakpoints.empty()) return false;          // the common case, one branch
	const auto g = s_breakpoints.find(tabKeyFor(tabKey));
	return g != s_breakpoints.end() && g->second.count(nodeId) != 0;
}

std::vector<int> breakpointsOf(const std::string& tabKey)
{
	const auto g = s_breakpoints.find(tabKeyFor(tabKey));
	if (g == s_breakpoints.end()) return {};
	return std::vector<int>(g->second.begin(), g->second.end());
}

size_t breakpointCount()
{
	size_t n = 0;
	for (const auto& [tab, ids] : s_breakpoints) n += ids.size();
	return n;
}

void clearBreakpoints(const std::string& tabKey)
{
	if (s_breakpoints.erase(tabKeyFor(tabKey)) != 0) persistBreakpoints();
}

void clearAllBreakpoints()
{
	if (s_breakpoints.empty()) return;
	s_breakpoints.clear();
	persistBreakpoints();
}

bool shouldBreakAt(const std::string& runtimeKey, int nodeId)
{
	return hasBreakpoint(runtimeKey, nodeId);
}

// ── The store ────────────────────────────────────────────────────────────────
std::string saveBreakpointsJson()
{
	// Sorted keys and sorted ids (the set is ordered): the same breakpoints
	// always write the same bytes, so the file only changes when they do.
	nlohmann::json bp = nlohmann::json::object();
	for (const auto& [tab, ids] : std::map<std::string, std::set<int>>(s_breakpoints.begin(), s_breakpoints.end()))
		bp[tab] = ids;
	nlohmann::json j;
	j["breakpoints"] = std::move(bp);
	return j.dump(2);
}

bool loadBreakpointsJson(const std::string& json)
{
	s_breakpoints.clear();
	const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) return false;
	const auto bp = j.find("breakpoints");
	if (bp == j.end() || !bp->is_object()) return false;
	for (const auto& [tab, ids] : bp->items())
	{
		if (!ids.is_array()) continue;
		const std::string key = tabKeyFor(tab);
		if (key.empty()) continue;
		for (const auto& id : ids)
			if (id.is_number_integer() && id.get<int>() != 0) s_breakpoints[key].insert(id.get<int>());
		if (s_breakpoints[key].empty()) s_breakpoints.erase(key);
	}
	return true;
}

bool setBreakpointStore(const std::string& file)
{
	s_breakpointStore = file;
	s_breakpoints.clear();
	if (file.empty()) return false;
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return loadBreakpointsJson(text);
}

std::string breakpointStore() { return s_breakpointStore; }

std::string breakpointStoreForProject(const std::string& projectPath)
{
	namespace fs = std::filesystem;
	if (projectPath.empty()) return {};
	fs::path root(projectPath);
	std::error_code ec;
	if (fs::is_regular_file(root, ec)) root = root.parent_path();
	return (root / "Saved" / "Breakpoints.json").string();
}

// ── The stop ─────────────────────────────────────────────────────────────────
void recordStop(const std::string& runtimeKey, int nodeId, uint32_t instance)
{
	const std::string tab = tabKeyFor(runtimeKey);
	if (tab.empty() || nodeId == 0) return;
	// The FIRST stop is the one shown: it is the run Step acts on. A second
	// run stopping while one is already shown queues behind it in the runtime
	// and comes to the front when the first one is continued.
	if (s_pausedTab.empty())
	{
		s_pausedTab      = tab;
		s_pausedNode     = nodeId;
		s_pausedInstance = instance;
		requestReveal(runtimeKey, nodeId);
	}
	s_breakHit = true;
}

int pausedNodeOf(const std::string& tabKey)
{
	return (!s_pausedTab.empty() && tabKey == s_pausedTab) ? s_pausedNode : 0;
}

bool     isPaused()       { return !s_pausedTab.empty(); }
uint32_t pausedInstance() { return s_pausedInstance; }

void refreshPaused()
{
	if (!s_runtime) return;
	const HorizonCode::SuspendedRun* run = s_runtime->suspendedRun(0);
	if (!run) { clearPaused(); return; }
	const std::string tab = tabKeyFor(run->classKey);
	// Moved on (a Step, or the first run continued and the next came to the
	// front): show the new site, and reveal it the way the first was.
	if (tab != s_pausedTab || run->nodeId != s_pausedNode)
	{
		s_pausedTab      = tab;
		s_pausedNode     = run->nodeId;
		s_pausedInstance = run->instance;
		requestReveal(run->classKey, run->nodeId);
	}
}

void clearPaused()
{
	s_pausedTab.clear();
	s_pausedNode     = 0;
	s_pausedInstance = 0;
}

bool takeBreakHit()
{
	const bool hit = s_breakHit;
	s_breakHit = false;
	return hit;
}

void requestReveal(const std::string& runtimeKey, int nodeId)
{
	const std::string tab = tabKeyFor(runtimeKey);
	if (tab.empty() || nodeId == 0) return;
	s_revealTab      = tab;
	s_revealNode     = nodeId;
	s_revealTabTaken = false;
}

bool takeRevealTab(std::string& tabKey)
{
	if (s_revealTab.empty() || s_revealTabTaken) return false;
	tabKey           = s_revealTab;
	s_revealTabTaken = true;
	return true;
}

bool takeRevealNode(const std::string& tabKey, int& nodeId)
{
	if (s_revealTab.empty() || tabKey != s_revealTab) return false;
	nodeId = s_revealNode;
	s_revealTab.clear();
	s_revealNode     = 0;
	s_revealTabTaken = false;
	return true;
}

bool revealPendingFor(const std::string& tabKey)
{
	return !s_revealTab.empty() && tabKey == s_revealTab;
}

void cancelReveal()
{
	s_revealTab.clear();
	s_revealNode     = 0;
	s_revealTabTaken = false;
}

} // namespace HcExecTrace
