#include "HcExecTrace.h"
#include "LevelScriptPanel.h"    // kTabPath — the Level Script tab's reserved path
#include "GameInstancePanel.h"   // kTabPath — the Game Instance tab's

#include <HorizonCode/HorizonCodeRuntime.h>

#include <chrono>
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
	rt.setExecListener([](HorizonCode::InstanceId instance, const std::string& classKey,
	                      size_t /*level*/, int nodeId)
	{ recordHit(classKey, nodeId, instance); });
}

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
