#pragma once
// ─── Where each graph was last looked at ─────────────────────────────────────
// The pan and zoom of every graph canvas (HorizonCode graphs, material graphs,
// particle graphs, animator graphs, widget graphs), keyed by asset, so a graph
// opens where it was left — without the user saving anything. It is VIEW state,
// not asset state: nothing here touches the asset, marks a tab dirty or ends up
// in a commit, which is why it lives next to the editor's other per-project
// memory (open tabs) and not in the file.
//
// This header is the in-memory half and is deliberately dependency-free, so the
// shared GraphEditor (also compiled into he_tests) can use it. The persistent half
// — loading the project's table, writing it back a moment after the view stops
// moving — is EditorUI::tickGraphViews / flushGraphViews.
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace GraphViewStore
{
	struct View { float panX = 0.0f, panY = 0.0f, zoom = 1.0f; };

	// Which graph tabs a HorizonCode editor had open: the graph on screen (0 = the
	// event graph) and the function ids that had a tab, in tab order.
	struct OpenTabs { int active = 0; std::vector<int> open; };

	struct Table
	{
		std::unordered_map<std::string, View> views;
		std::unordered_map<std::string, OpenTabs> tabs;
		bool   dirty     = false;   // something moved since the last write
		double changedAt = 0.0;     // when, in ImGui time — the write waits for quiet
	};

	inline Table& table() { static Table t; return t; }

	inline bool get(const std::string& key, View& out)
	{
		const auto& v = table().views;
		const auto it = v.find(key);
		if (it == v.end()) return false;
		out = it->second;
		return true;
	}

	inline bool getTabs(const std::string& key, OpenTabs& out)
	{
		const auto& t = table().tabs;
		const auto it = t.find(key);
		if (it == t.end()) return false;
		out = it->second;
		return true;
	}

	// Every frame, like put(): a change in the set or the active graph marks the
	// table for writing, an unchanged one costs a compare.
	inline void putTabs(const std::string& key, int active, const std::vector<int>& open, double now)
	{
		Table& t = table();
		const auto it = t.tabs.find(key);
		if (it == t.tabs.end() ? (active == 0 && open.empty())
		                       : (it->second.active == active && it->second.open == open))
			return;
		t.tabs[key] = { active, open };
		t.dirty = true;
		t.changedAt = now;
	}

	// Called every frame by a canvas that has a key: only a real move (half a pixel,
	// a thousandth of zoom) counts, so a still view costs one lookup.
	inline void put(const std::string& key, float px, float py, float zoom, double now)
	{
		Table& t = table();
		View& v = t.views[key];
		if (std::fabs(v.panX - px) < 0.5f && std::fabs(v.panY - py) < 0.5f &&
		    std::fabs(v.zoom - zoom) < 0.001f)
			return;
		v.panX = px; v.panY = py; v.zoom = zoom;
		t.dirty = true;
		t.changedAt = now;
	}
}
