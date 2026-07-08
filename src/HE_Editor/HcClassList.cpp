#include "HcClassList.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>
#include <Types/Enums.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <string>
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
		int t = (int)type - 1; // PinType: Exec=0, Float=1, …, Ref=7, Transform=8
		if (t < 0) t = 0;
		ImGui::SetNextItemWidth(78.0f);
		if (ImGui::Combo(id, &t, "Float\0Bool\0Int\0String\0Vec2\0Color\0Object\0Transform\0"))
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

// ── Shared graph colors ──────────────────────────────────────────────────────
std::uint32_t pinTypeColor(HorizonCode::PinType t)
{
	using P = HorizonCode::PinType;
	switch (t)
	{
		case P::Exec:   return IM_COL32(235, 235, 235, 255);
		case P::Float:  return IM_COL32(160, 200, 120, 255);
		case P::Bool:   return IM_COL32(210,  90,  90, 255);
		case P::Int:    return IM_COL32(110, 200, 200, 255);
		case P::String: return IM_COL32(220, 130, 210, 255);
		case P::Vec2:   return IM_COL32(120, 200, 210, 255);
		case P::Color:  return IM_COL32(230, 210, 110, 255);
		case P::Ref:    return IM_COL32(180, 140, 240, 255);
		case P::Transform: return IM_COL32(240, 160, 100, 255);   // orange
	}
	return IM_COL32_WHITE;
}

namespace
{
	std::uint32_t darken(std::uint32_t c, float f)
	{
		const int r = (int)((c >> IM_COL32_R_SHIFT) & 0xFF);
		const int g = (int)((c >> IM_COL32_G_SHIFT) & 0xFF);
		const int b = (int)((c >> IM_COL32_B_SHIFT) & 0xFF);
		return IM_COL32((int)(r * f), (int)(g * f), (int)(b * f), 255);
	}
	// The value type a data node's color should reflect (Get/Set → its propType,
	// a literal → its own type, math/logic/string → their result type).
	HorizonCode::PinType nodeValueType(const HorizonCode::Node& n)
	{
		using T = HorizonCode::NodeType; using P = HorizonCode::PinType;
		switch (n.type)
		{
			case T::GetVariable: case T::SetVariable:
			case T::GetProperty: case T::SetProperty:
			case T::GetExternal: case T::SetExternal: return n.propType;
			case T::ConstFloat:  return P::Float;
			case T::ConstBool:   return P::Bool;
			case T::ConstInt:    return P::Int;
			case T::ConstString: return P::String;
			case T::ConstVec2:   return P::Vec2;
			case T::ConstColor:  return P::Color;
			case T::ConstTransform: return P::Transform;
			case T::Add: case T::Subtract: case T::Multiply: case T::Divide: return P::Float;
			case T::Greater: case T::Less: case T::Equals:
			case T::And: case T::Or: case T::Not: return P::Bool;
			case T::Concat: case T::ToString: return P::String;
			case T::ArrayMake: case T::ArrayGet: case T::ArrayAdd: return n.propType;
			case T::ArrayLength: return P::Int;
			default: return P::Ref;
		}
	}
}

// ── HC class registry ────────────────────────────────────────────────────────
ClassInfo classInfoFromGraph(const HorizonCode::Graph& g, const std::string& label,
                             const std::string& path, ClassInfo::Kind kind)
{
	using T = HorizonCode::NodeType;
	ClassInfo ci; ci.label = label; ci.path = path; ci.kind = kind;
	for (const auto& n : g.nodes)
		if (n.type == T::FunctionEntry && n.access == 0 && !n.s.empty())
		{
			MemberFn f; f.name = n.s; f.hasResult = !n.results.empty();
			for (const auto& p : n.params) f.paramTypes.push_back(p.type);
			ci.functions.push_back(std::move(f));
		}
	for (const auto& v : g.variables)
		if (v.access == 0) ci.variables.push_back({ v.name, v.type, v.className });
	return ci;
}

bool classInfoForPath(ContentManager* cm, const std::string& path, ClassInfo& out)
{
	if (!cm || path.empty()) return false;
	const HE::UUID id = cm->loadAsset(path);
	const std::string label = std::filesystem::path(path).stem().string();
	HorizonCode::Graph g;
	if (const HorizonCodeClassAsset* a = cm->getHorizonCodeClass(id);
	    a && !a->graphJson.empty() && HorizonCode::fromJson(a->graphJson, g))
		{ out = classInfoFromGraph(g, label, path, ClassInfo::Class); return true; }
	if (const UIWidgetAsset* w = cm->getWidget(id);
	    w && !w->graphJson.empty() && HorizonCode::fromJson(w->graphJson, g))
		{ out = classInfoFromGraph(g, label, path, ClassInfo::Widget); return true; }
	return false;
}

std::vector<ClassInfo> listClasses(ContentManager* cm,
                                   const HorizonCode::Graph* levelGraph,
                                   const HorizonCode::Graph* giGraph)
{
	std::vector<ClassInfo> out;
	for (const auto& c : listAssets(cm, HE::AssetType::HorizonCodeClass))
	{ ClassInfo ci; if (classInfoForPath(cm, c.path, ci)) out.push_back(std::move(ci)); }
	for (const auto& c : listAssets(cm, HE::AssetType::Widget))
	{ ClassInfo ci; if (classInfoForPath(cm, c.path, ci)) out.push_back(std::move(ci)); }
	if (levelGraph) out.push_back(classInfoFromGraph(*levelGraph, "Level", "", ClassInfo::Level));
	if (giGraph)    out.push_back(classInfoFromGraph(*giGraph, "Game Instance", "", ClassInfo::GameInstance));
	return out;
}

namespace
{
	const char* valueTypeName(HorizonCode::PinType t)
	{
		using P = HorizonCode::PinType;
		switch (t)
		{
			case P::Float:  return "Float";  case P::Bool:  return "Bool";
			case P::Int:    return "Int";    case P::String:return "String";
			case P::Vec2:   return "Vec2";   case P::Color: return "Color";
			case P::Transform: return "Transform";
			case P::Ref:    return "Object"; default:       return "Exec";
		}
	}
	std::string lc(std::string s)
	{ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
}

bool drawTypePicker(const char* label, ContentManager* cm,
                    HorizonCode::PinType& type, std::string* className)
{
	using P = HorizonCode::PinType;
	bool changed = false;
	std::string cur = (type == P::Ref && className && !className->empty())
		? std::filesystem::path(*className).stem().string()
		: valueTypeName(type);
	if (ImGui::BeginCombo(label, cur.c_str()))
	{
		static std::string search;
		if (ImGui::IsWindowAppearing()) { search.clear(); ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputTextWithHint("##typesearch", "Search…", &search);
		const std::string q = lc(search);
		auto hit = [&](const std::string& s){ return q.empty() || lc(s).find(q) != std::string::npos; };

		ImGui::TextDisabled("Default");
		const P defs[] = { P::Float, P::Bool, P::Int, P::String, P::Vec2, P::Color, P::Transform };
		for (P d : defs)
			if (hit(valueTypeName(d)) && ImGui::Selectable(valueTypeName(d), type == d && (!className || className->empty())))
			{ type = d; if (className) className->clear(); changed = true; }

		if (className) // object types only where a class binding is allowed
		{
			ImGui::Separator();
			ImGui::TextDisabled("Objects");
			for (const auto& c : listClasses(cm, nullptr, nullptr))
				if (hit(c.label) && ImGui::Selectable(c.label.c_str(), type == P::Ref && *className == c.path))
				{ type = P::Ref; *className = c.path; changed = true; }
		}
		ImGui::EndCombo();
	}
	return changed;
}

std::uint32_t nodeHeaderColor(const HorizonCode::Node& n)
{
	using T = HorizonCode::NodeType;
	switch (n.type)
	{
		// Entry/exit + delegation are the "control" families with fixed colors.
		case T::Event: case T::BindEvent: case T::EmitEvent:
			return IM_COL32(172, 62, 62, 255);   // events → red
		case T::FunctionEntry: case T::FunctionCall: case T::FunctionReturn:
		case T::CallExternal:
			return IM_COL32(140, 88, 184, 255);  // functions → purple
		case T::Branch: case T::Sequence:
			return IM_COL32(96, 96, 104, 255);   // flow → gray
		case T::EngineCall:
			return IM_COL32(56, 132, 132, 255);  // engine API → teal
		// Reference/object producers carry the Ref (purple) tint.
		case T::GetSelf: case T::GetGameInstance:
		case T::CreateObject: case T::DestroyObject:
		case T::CreateWidget: case T::ShowWidgetId: case T::HideWidgetId: case T::DestroyWidget:
		case T::ShowWidget: case T::HideWidget:
			return darken(pinTypeColor(HorizonCode::PinType::Ref), 0.78f);
		// Everything data-ish is colored by its value type (Bool getter always red…).
		default:
			return darken(pinTypeColor(nodeValueType(n)), 0.72f);
	}
}

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

float literalNodeBodyHeight(const HorizonCode::Node& n)
{
	using T = HorizonCode::NodeType;
	switch (n.type)
	{
		case T::ConstBool:   return 22.0f;
		case T::ConstInt:
		case T::ConstFloat:
		case T::ConstVec2:   return 24.0f;
		case T::ConstColor:  return 22.0f;
		case T::ConstTransform: return 72.0f;   // three rows: position / rotation / scale
		case T::ConstString:
		{
			// Grow with the line count up to a cap; past that the field scrolls.
			int lines = 1;
			for (char c : n.s) if (c == '\n') ++lines;
			if (lines > 5) lines = 5;
			return 8.0f + (float)lines * 17.0f;
		}
		default: return 0.0f;
	}
}

bool drawLiteralNodeBody(HorizonCode::Node& n, bool& committed)
{
	using T = HorizonCode::NodeType;
	bool changed = false;
	ImGui::SetNextItemWidth(-FLT_MIN);   // fill the body width
	switch (n.type)
	{
		case T::ConstBool:
		{
			bool b = n.f[0] != 0.0f;
			if (ImGui::Checkbox("##litv", &b)) { n.f[0] = b ? 1.0f : 0.0f; changed = true; committed = true; }
			break;
		}
		case T::ConstInt:
		{
			int v = (int)n.f[0];
			if (ImGui::InputInt("##litv", &v, 0, 0)) { n.f[0] = (float)v; changed = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case T::ConstFloat:
		{
			float v = n.f[0];
			if (ImGui::InputFloat("##litv", &v, 0.0f, 0.0f, "%.4g")) { n.f[0] = v; changed = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case T::ConstVec2:
		{
			float v[2] = { n.f[0], n.f[1] };
			if (ImGui::InputFloat2("##litv", v, "%.3g")) { n.f[0] = v[0]; n.f[1] = v[1]; changed = true; }
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case T::ConstColor:
		{
			if (ImGui::ColorEdit4("##litv", n.f,
			        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar))
				changed = true;
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case T::ConstString:
		{
			if (ImGui::InputTextMultiline("##litv", &n.s, ImVec2(-FLT_MIN, -FLT_MIN),
			        ImGuiInputTextFlags_None))
				changed = true;
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case T::ConstTransform:
		{
			auto row = [&](const char* lbl, glm::vec3& v)
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputFloat3(lbl, &v.x, "%.3g")) changed = true;
				committed |= ImGui::IsItemDeactivatedAfterEdit();
			};
			row("##pos", n.tpos);   // position
			row("##rot", n.trot);   // rotation (euler degrees)
			row("##scl", n.tscl);   // scale
			break;
		}
		default: break;
	}
	return changed;
}

std::string drawEngineApiMenu(const std::string& lowerQuery)
{
	auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(),
		[](unsigned char c){ return (char)std::tolower(c); }); return v; };
	std::string picked;
	const char* header = nullptr; // current category header, drawn lazily
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		const bool match = lowerQuery.empty()
			|| lower(fn.id).find(lowerQuery) != std::string::npos
			|| lower(fn.category).find(lowerQuery) != std::string::npos;
		if (!match) continue;
		if (!header || std::string(header) != fn.category)
		{ ImGui::TextDisabled("Engine · %s", fn.category); header = fn.category; }
		if (ImGui::Selectable(fn.id)) picked = fn.id;
	}
	return picked;
}
}
