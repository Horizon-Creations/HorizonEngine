#include "HcClassList.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <HorizonCode/HorizonCode.h>
#include <Types/Enums.h>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace HcEditorUtil
{
namespace
{
	// Data-pin count check helper: how many total pins a function node has now
	// (params/results already synced). Exec pins come first, so links on pins at
	// or beyond this index are stale after an interface change.
	int functionPinCount(const HorizonCode::Node& n)
	{
		using T = HorizonCode::NodeType;
		switch (n.type)
		{
			case T::FunctionEntry:  return 1 + (int)n.params.size();                      // execOut + params
			case T::FunctionCall:   return 2 + (int)n.params.size() + (int)n.results.size(); // exec in/out + ins + outs
			case T::FunctionReturn: return 1 + (int)n.results.size();                     // execIn + results
			default:                return 1 << 30;
		}
	}

	// After an interface edit: re-sync every call/return, then drop links pointing
	// at pins that no longer exist on this function's nodes (exec links, at fixed
	// low indices, always survive).
	void syncAndPrune(HorizonCode::Graph& g, const std::string& fn)
	{
		HorizonCode::syncFunctionSignatures(g);
		using T = HorizonCode::NodeType;
		for (const HorizonCode::Node& n : g.nodes)
		{
			const bool isFnNode = (n.type == T::FunctionEntry || n.type == T::FunctionCall ||
			                       n.type == T::FunctionReturn);
			if (!isFnNode || n.s != fn) continue;
			const int total = functionPinCount(n);
			const int id = n.id;
			g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
				[&](const HorizonCode::Link& l)
				{
					return (l.srcNode == id && l.srcPin >= total) ||
					       (l.dstNode == id && l.dstPin >= total);
				}), g.links.end());
		}
	}

	// Type combo over the value pin types (Exec excluded). Index maps to PinType
	// starting at Float (=1). Returns true when changed.
	bool pinTypeCombo(const char* id, HorizonCode::PinType& type)
	{
		int t = (int)type - 1; // PinType: Exec=0, Float=1, …, Ref=7
		if (t < 0) t = 0;
		ImGui::SetNextItemWidth(78.0f);
		if (ImGui::Combo(id, &t, "Float\0Bool\0Int\0String\0Vec2\0Color\0Object\0"))
		{ type = (HorizonCode::PinType)(t + 1); return true; }
		return false;
	}
}
std::vector<ClassRef> listAssets(ContentManager* cm, HE::AssetType type)
{
	std::vector<ClassRef> out;
	if (!cm) return out;
	const std::string root = cm->contentRoot();
	if (root.empty()) return out;

	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(root, ec), end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break;
		if (!it->is_regular_file(ec)) continue;
		if (it->path().extension() != ".hasset") continue;
		HAsset::Reader r;
		if (r.open(it->path().string()) &&
		    r.assetType() == static_cast<uint16_t>(type))
		{
			ClassRef cr;
			cr.label = it->path().stem().string();
			cr.path  = std::filesystem::relative(it->path(), root, ec).generic_string();
			out.push_back(std::move(cr));
		}
	}
	return out;
}

std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm)
{ return listAssets(cm, HE::AssetType::HorizonCodeClass); }

void drawFunctionInterface(HorizonCode::Graph& g, HorizonCode::Node& entry, bool& edited)
{
	using namespace HorizonCode;
	bool changed = false;

	auto editList = [&](const char* title, const char* prefix, std::vector<FuncParam>& list)
	{
		ImGui::SeparatorText(title);
		int removeIdx = -1;
		for (size_t i = 0; i < list.size(); ++i)
		{
			ImGui::PushID((int)(title[0]) * 4096 + (int)i);
			ImGui::SetNextItemWidth(110.0f);
			ImGui::InputText("##nm", &list[i].name);
			if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
			ImGui::SameLine();
			if (pinTypeCombo("##ty", list[i].type)) changed = true;
			ImGui::SameLine();
			if (ImGui::SmallButton("X")) removeIdx = (int)i;
			ImGui::PopID();
		}
		if (removeIdx >= 0) { list.erase(list.begin() + removeIdx); changed = true; }
		char btn[24]; std::snprintf(btn, sizeof btn, "+ Add##%s", title);
		if (ImGui::SmallButton(btn))
		{
			FuncParam p;
			p.name = std::string(prefix) + std::to_string(list.size() + 1);
			list.push_back(p);
			changed = true;
		}
	};

	editList("Inputs",  "in",  entry.params);
	editList("Outputs", "out", entry.results);

	if (changed)
	{
		syncAndPrune(g, entry.s);
		edited = true;
	}
}

bool drawReturnFunctionPicker(HorizonCode::Graph& g, HorizonCode::Node& ret)
{
	using namespace HorizonCode;
	bool changed = false;
	if (ImGui::BeginCombo("Function", ret.s.empty() ? "(none)" : ret.s.c_str()))
	{
		for (const Node& e : g.nodes)
			if (e.type == NodeType::FunctionEntry && !e.s.empty())
				if (ImGui::Selectable(e.s.c_str(), ret.s == e.s))
				{
					ret.s = e.s;
					ret.results = e.results; // mirror this function's outputs onto the Return
					changed = true;
				}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("Feeds this function's return values.");
	return changed;
}
}
