#include "HcEditorUtil.h"
#include <cstdint>
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
#include <climits>
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
namespace
{
	// Scans one root directory for .hasset files of the given type, appending
	// ClassRefs whose path is prefixed exactly as toContentRelativePath() would
	// (pathPrefix is "" for the project Content root, "Engine/" for the engine
	// content root) so the result is directly usable with loadAsset()/pickers.
	void scanAssetsInto(std::vector<ClassRef>& out, const std::string& root,
	                     const std::string& pathPrefix, HE::AssetType type)
	{
		if (root.empty()) return;
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
				cr.path  = pathPrefix + std::filesystem::relative(it->path(), root, ec).generic_string();
				out.push_back(std::move(cr));
			}
		}
	}
}

std::vector<ClassRef> listAssets(ContentManager* cm, HE::AssetType type)
{
	std::vector<ClassRef> out;
	if (!cm) return out;
	scanAssetsInto(out, cm->contentRoot(), "", type);
	scanAssetsInto(out, cm->engineContentRoot(), "Engine/", type);
	return out;
}

std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm)
{ return listAssets(cm, HE::AssetType::HorizonCodeClass); }

std::vector<ClassRef> listScenes(ContentManager* cm)
{
	std::vector<ClassRef> out;
	if (!cm) return out;
	const std::string root = cm->contentRoot();
	if (root.empty()) return out;
	// Scenes live anywhere under the PROJECT root (parent of Content/), and the
	// path we store must be project-relative (e.g. "Content/123.hescene") — that
	// is exactly the key the exporter packs each scene under (sceneUuidForPath)
	// and what the game's loadSceneInto resolves. Typing a bare "123.hescene"
	// mismatches both → this dropdown removes that failure mode.
	const std::filesystem::path projectRoot = std::filesystem::path(root).parent_path();
	std::error_code ec;
	std::filesystem::recursive_directory_iterator it(
		projectRoot, std::filesystem::directory_options::skip_permission_denied, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		if (!it->is_regular_file(ec) || it->path().extension() != ".hescene") { ec.clear(); continue; }
		ClassRef cr;
		cr.label = it->path().stem().string();
		cr.path  = it->path().lexically_relative(projectRoot).generic_string();
		out.push_back(std::move(cr));
	}
	std::sort(out.begin(), out.end(),
	          [](const ClassRef& a, const ClassRef& b) { return a.path < b.path; });
	return out;
}

// ── Hover tooltips ────────────────────────────────────────────────────────────
namespace
{
	const char* tooltipTypeName(HorizonCode::PinType t)
	{
		using P = HorizonCode::PinType;
		switch (t)
		{
			case P::Float:     return "Float";
			case P::Bool:      return "Bool";
			case P::Int:       return "Int";
			case P::String:    return "String";
			case P::Vec2:      return "Vec2";
			case P::Color:     return "Color";
			case P::Ref:       return "Object";
			case P::Transform: return "Transform";
			default:           return "Exec";
		}
	}

	// One "  Name: Type[]" line per pin; exec pins draw as "  ▶ Name".
	void appendPinLines(std::string& out, const std::vector<HorizonCode::PinDesc>& execPins,
	                    const std::vector<HorizonCode::PinDesc>& dataPins, const char* execFallback)
	{
		for (const auto& p : execPins)
		{
			out += "  > ";
			out += (p.name && *p.name) ? p.name : execFallback;
			out += " (exec)\n";
		}
		for (const auto& p : dataPins)
		{
			out += "  ";
			if (p.name && *p.name) { out += p.name; out += ": "; }
			out += tooltipTypeName(p.type);
			if (p.isArray) out += "[]";
			out += '\n';
		}
	}
} // namespace

std::string nodeTooltipText(const HorizonCode::Node& n)
{
	using T = HorizonCode::NodeType;
	std::string out;

	// Description: the static per-type text; an Engine Call names its registry
	// entry instead (the generic EngineCall blurb says less than the id does).
	if (n.type == T::EngineCall && !n.s.empty())
	{
		if (const HE::api::ApiFn* fn = HE::api::find(n.s))
		{
			out += "Engine API call: ";
			out += fn->category; out += " - "; out += fn->id;
			out += fn->isExec ? "\nRuns when executed." : "\nPure — evaluated whenever an output is used.";
		}
		else { out += "Engine API call: "; out += n.s; out += " (unknown registry id)"; }
	}
	else
	{
		const char* d = HorizonCode::nodeTooltip(n.type);
		if (d && *d) out += d;
	}

	const HorizonCode::NodeSig sig = HorizonCode::signatureOf(n);
	std::string pins;
	if (!sig.execIns.empty() || !sig.dataIns.empty())
	{
		pins += "Inputs:\n";
		appendPinLines(pins, sig.execIns, sig.dataIns, "In");
	}
	if (!sig.execOuts.empty() || !sig.dataOuts.empty())
	{
		pins += "Outputs:\n";
		appendPinLines(pins, sig.execOuts, sig.dataOuts, "Out");
	}
	if (!pins.empty())
	{
		if (!out.empty()) out += "\n\n";
		out += pins;
	}
	if (!out.empty() && out.back() == '\n') out.pop_back();
	return out;
}

std::string nodeTooltipText(HorizonCode::NodeType t)
{
	HorizonCode::Node tmp;
	tmp.type = t;
	return nodeTooltipText(tmp);
}

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
			case T::ArrayMake: case T::ArrayGet: case T::ArrayAdd:
			case T::ArraySet: case T::ArrayInsert: case T::ArrayRemove: return n.propType;
			case T::ArrayLength: case T::ArrayIndexOf: return P::Int;
			case T::ArrayContains: return P::Bool;
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
		if (v.access == 0 && v.scope == 0) // function-locals are never part of the interface
			ci.variables.push_back({ v.name, v.type, v.className });
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
		case T::Branch: case T::Sequence: case T::ForEach:
			return IM_COL32(96, 96, 104, 255);   // flow → gray
		case T::EngineCall:
			return IM_COL32(56, 132, 132, 255);  // engine API → teal
		// Reference/object producers carry the Ref (purple) tint.
		case T::GetSelf: case T::GetGameInstance:
		case T::CreateObject: case T::DestroyObject:
		case T::CreateWidget: case T::ShowWidget: case T::HideWidget: case T::DestroyWidget:
		case T::ShowSelf: case T::HideSelf:
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

// ── Searchable menu: ranking + keyboard driving ──────────────────────────────
// One session at a time (the add-node palette and the drag-off menu are both
// popups and can never be open together), so the state is a single file-static.
//
// The ranking is settled at the END of a frame, once every entry has registered,
// and the highlight it produces is drawn on the NEXT one. That one-frame delay is
// what makes the whole thing work in immediate mode: an entry has to be drawn
// before we know whether something better comes after it — at 60 Hz the highlight
// simply appears to follow the typing.
//
// Enter normally picks within the same frame, EXCEPT when the query also changed
// in it (ImGui delivers a whole burst of queued keys at once, so a fast typer's
// last letter and their Enter can arrive together): the pick is then held over
// one frame, or it would insert whatever the PREVIOUS query had highlighted.
namespace
{
	struct SearchNav
	{
		std::string text;                 // raw search field contents
		std::string query;                // lowercased `text`, as ranked against
		std::string activeKey;            // the highlighted entry
		std::string bestKey;              // best-ranked entry seen this frame
		int         bestScore = INT_MAX;
		std::vector<std::string> keys;    // entries registered this frame, in order
		int  move        = 0;             // pending ↑/↓ steps from the search field
		bool enterQueued = false;         // Enter pressed; the highlight takes it next frame
		bool scrollTo    = false;         // bring the highlight into view
		bool refocus     = false;         // Enter deactivates the field — take it back
		bool queryDirty  = false;         // query edited this frame → re-pick the best match
	};
	SearchNav s_nav;

	// ↑/↓ inside a single-line InputText are only delivered (and only taken away
	// from ImGui's own nav, which would otherwise walk the focus out of the field)
	// when the history flag is set — that is exactly what this callback is for.
	int searchNavCallback(ImGuiInputTextCallbackData* d)
	{
		if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory)
			s_nav.move += (d->EventKey == ImGuiKey_UpArrow) ? -1 : 1;
		return 0;
	}

	// How well `lowerLabel` answers `q` — lower is better, ties go to whichever
	// entry the menu drew first. Entries that don't contain the query at all are
	// still rankable (a menu may list a whole category because the CATEGORY
	// matched), they just lose to every real match.
	int rankMatch(const std::string& lowerLabel, const std::string& q)
	{
		if (q.empty()) return 0;                       // no query → the first entry leads
		const size_t pos = lowerLabel.find(q);
		if (pos == std::string::npos) return 9000;
		if (lowerLabel.size() == q.size()) return 0;   // exact name
		const int extra = static_cast<int>(lowerLabel.size() - q.size()); // prefer the tighter fit
		if (pos == 0) return 100 + extra;                                 // "add" → "Add"
		const bool wordStart = !std::isalnum(static_cast<unsigned char>(lowerLabel[pos - 1]));
		if (wordStart) return 300 + static_cast<int>(pos) + extra;        // "vec" → "Make Vec2"
		return 600 + static_cast<int>(pos) + extra;                       // anywhere inside a word
	}
} // namespace

std::string searchMenuBegin(const char* id, const char* hint, float width)
{
	if (ImGui::IsWindowAppearing())
	{
		s_nav = SearchNav{};
		s_nav.scrollTo = true;
		ImGui::SetKeyboardFocusHere();
	}
	// EnterReturnsTrue deactivates the field, so an Enter that picks nothing (no
	// match) would leave the user typing into a dead box — take the focus back.
	if (s_nav.refocus) { ImGui::SetKeyboardFocusHere(); s_nav.refocus = false; }

	ImGui::SetNextItemWidth(width);
	if (ImGui::InputTextWithHint(id, hint, &s_nav.text,
	        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
	        searchNavCallback))
	{ s_nav.enterQueued = true; s_nav.refocus = true; }

	std::string q = s_nav.text;
	std::transform(q.begin(), q.end(), q.begin(),
		[](unsigned char c){ return (char)std::tolower(c); });
	s_nav.queryDirty = (q != s_nav.query);
	s_nav.query      = q;

	s_nav.keys.clear();
	s_nav.bestKey.clear();
	s_nav.bestScore = INT_MAX;
	return q;
}

bool searchMenuItem(const std::string& label, bool disabled)
{
	if (disabled) // never highlighted, never reachable by ↑/↓ or Enter
	{
		ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_Disabled);
		return false;
	}

	// Rank on the VISIBLE text: an "##id" suffix disambiguates the ImGui id, it is
	// not part of what the user is searching for.
	std::string shown = label.substr(0, label.find("##"));
	std::transform(shown.begin(), shown.end(), shown.begin(),
		[](unsigned char c){ return (char)std::tolower(c); });
	const int score = rankMatch(shown, s_nav.query);
	if (score < s_nav.bestScore) { s_nav.bestScore = score; s_nav.bestKey = label; }
	s_nav.keys.push_back(label);

	// The highlight still belongs to the previous query while the query is being
	// edited this frame, so an Enter that arrived with the keystroke waits.
	const bool highlighted = (label == s_nav.activeKey);
	bool picked = false;
	if (highlighted && s_nav.enterQueued && !s_nav.queryDirty)
	{ s_nav.enterQueued = false; picked = true; }

	if (highlighted) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.90f, 0.55f, 0.10f, 0.55f));
	const bool clicked = ImGui::Selectable(label.c_str(), highlighted);
	if (highlighted) ImGui::PopStyleColor();
	if (highlighted && s_nav.scrollTo) ImGui::SetScrollHereY(0.5f);

	return clicked || picked;
}

void searchMenuEnd()
{
	// Settle the highlight for the next frame. Editing the query re-picks the
	// best match (that IS the point); otherwise the highlight stays where it is
	// — including where ↑/↓ put it — as long as that entry is still listed.
	auto indexOf = [](const std::string& k) -> int {
		for (size_t i = 0; i < s_nav.keys.size(); ++i)
			if (s_nav.keys[i] == k) return static_cast<int>(i);
		return -1; };

	bool scroll = false;
	int  idx    = indexOf(s_nav.activeKey);
	if (idx < 0 || s_nav.queryDirty)
	{
		s_nav.activeKey = s_nav.bestKey;
		idx             = indexOf(s_nav.activeKey);
		scroll          = true;
	}
	if (s_nav.move != 0 && !s_nav.keys.empty())
	{
		idx = std::clamp(idx + s_nav.move, 0, static_cast<int>(s_nav.keys.size()) - 1);
		s_nav.activeKey = s_nav.keys[idx];
		scroll          = true;
	}
	s_nav.scrollTo = scroll;
	s_nav.move     = 0;
	// An Enter held over for the re-ranked highlight survives into the next frame;
	// one that simply found nothing to pick (empty list) must not linger.
	if (!s_nav.queryDirty) s_nav.enterQueued = false;
}

std::string drawEngineApiMenu(const std::string& lowerQuery)
{
	auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(),
		[](unsigned char c){ return (char)std::tolower(c); }); return v; };
	std::string picked;
	const char* header = nullptr; // current category header, drawn lazily
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		const char* shown = fn.displayName ? fn.displayName : fn.id; // readable name
		const bool match = lowerQuery.empty()
			|| lower(shown).find(lowerQuery) != std::string::npos
			|| lower(fn.id).find(lowerQuery) != std::string::npos
			|| lower(fn.category).find(lowerQuery) != std::string::npos;
		if (!match) continue;
		if (!header || std::string(header) != fn.category)
		{ ImGui::TextDisabled("Engine · %s", fn.category); header = fn.category; }
		// Unique ImGui id via the stable api id (display names may repeat later).
		if (searchMenuItem(std::string(shown) + "##" + fn.id)) picked = fn.id;
		if (ImGui::IsItemHovered())
		{
			// Same self-documentation as the canvas hover: build the EngineCall
			// node this pick would create and show its full tooltip.
			HorizonCode::Node tmp;
			tmp.type = HorizonCode::NodeType::EngineCall;
			tmp.s = fn.id; tmp.hasArg = fn.isExec;
			for (const auto& p : fn.params)  tmp.params.push_back({ p.name, p.type, p.isArray });
			for (const auto& r : fn.results) tmp.results.push_back({ r.name, r.type, r.isArray });
			ImGui::SetTooltip("%s", nodeTooltipText(tmp).c_str());
		}
	}
	return picked;
}

std::string engineCallTitle(const std::string& apiId)
{
	if (const HE::api::ApiFn* fn = HE::api::find(apiId))
		if (fn->displayName) return fn->displayName;
	return apiId.empty() ? std::string("Engine Call") : apiId;
}

bool drawArrayDefaultEditor(HorizonCode::Variable& v)
{
	using P = HorizonCode::PinType; using V = HorizonCode::Value;
	bool changed = false;
	ImGui::SeparatorText("Default");
	ImGui::TextDisabled("%d element%s seed the array on creation.",
	                    (int)v.defaultItems.size(), v.defaultItems.size() == 1 ? "" : "s");
	int removeIdx = -1;
	for (size_t i = 0; i < v.defaultItems.size(); ++i)
	{
		V& it = v.defaultItems[i];
		ImGui::PushID((int)i);
		ImGui::Text("%d", (int)i);
		ImGui::SameLine(28.0f);
		// Leave room for the remove button on the right.
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
		switch (v.type)
		{
			case P::Float:  if (ImGui::DragFloat("##el", &it.f, 0.1f)) changed = true; break;
			case P::Int:  { int tmp = it.i; if (ImGui::DragInt("##el", &tmp)) { it.i = tmp; changed = true; } break; }
			case P::Bool: { bool b = it.b; if (ImGui::Checkbox("##el", &b)) { it.b = b; changed = true; } break; }
			case P::String: ImGui::InputText("##el", &it.s);
			                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true; break;
			case P::Vec2:   if (ImGui::DragFloat2("##el", &it.v2.x, 0.1f)) changed = true; break;
			case P::Color:  if (ImGui::ColorEdit4("##el", &it.col.x)) changed = true; break;
			case P::Transform:
				if (ImGui::DragFloat3("Pos##el", &it.tpos.x, 0.1f))  changed = true;
				if (ImGui::DragFloat3("Rot##el", &it.trot.x, 0.5f))  changed = true;
				if (ImGui::DragFloat3("Scl##el", &it.tscl.x, 0.05f)) changed = true;
				break;
			case P::Ref:    ImGui::TextDisabled("(object — resolved at runtime)"); break;
			default: break;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("X")) removeIdx = (int)i;
		ImGui::PopID();
	}
	if (removeIdx >= 0)
	{
		v.defaultItems.erase(v.defaultItems.begin() + removeIdx);
		changed = true;
	}
	if (ImGui::Button("+ Add Slot"))
	{
		V nv; nv.type = v.type;   // Value defaults are the per-type zeros (scale 1)
		v.defaultItems.push_back(std::move(nv));
		changed = true;
	}
	return changed;
}

namespace
{
	// Resolve a unified pin to its data-in index + descriptor (or -1 when the
	// pin isn't a data input).
	int dataInIndexOf(const HorizonCode::Node& n, int unifiedPin, HorizonCode::PinDesc& outDesc)
	{
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		const int dataIn0 = (int)(s.execIns.size() + s.execOuts.size());
		const int di = unifiedPin - dataIn0;
		if (di < 0 || di >= (int)s.dataIns.size()) return -1;
		outDesc = s.dataIns[di];
		return di;
	}
}

bool pinSupportsInlineDefault(const HorizonCode::Node& n, int unifiedPin)
{
	using P = HorizonCode::PinType;
	HorizonCode::PinDesc pd{};
	const int di = dataInIndexOf(n, unifiedPin, pd);
	if (di < 0 || pd.isArray) return false;
	return pd.type == P::Bool || pd.type == P::Int ||
	       pd.type == P::Float || pd.type == P::String;
}

void drawPinDefaultEditor(HorizonCode::Node& n, int unifiedPin, bool& committed)
{
	using P = HorizonCode::PinType; using V = HorizonCode::Value;
	HorizonCode::PinDesc pd{};
	const int di = dataInIndexOf(n, unifiedPin, pd);
	if (di < 0) return;
	// The stored default keeps the PIN's type (retypes re-seed on next edit).
	V& v = n.pinDefaults[di];
	if (v.type != pd.type) { v = V{}; v.type = pd.type; }
	ImGui::SetNextItemWidth(-FLT_MIN);
	switch (pd.type)
	{
		case P::Bool:
			if (ImGui::Checkbox("##pd", &v.b)) committed = true;
			break;
		case P::Int:
		{
			int tmp = v.i;
			if (ImGui::DragInt("##pd", &tmp)) v.i = tmp;
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		}
		case P::Float:
			ImGui::DragFloat("##pd", &v.f, 0.1f, 0.0f, 0.0f, "%.3g");
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		case P::String:
			ImGui::InputText("##pd", &v.s);
			committed |= ImGui::IsItemDeactivatedAfterEdit();
			break;
		default: break;
	}
}

bool drawSceneParamPicker(HorizonCode::Node& n, ContentManager* cm)
{
	using P = HorizonCode::PinType; using V = HorizonCode::Value;
	if (n.type != HorizonCode::NodeType::EngineCall) return false;
	// The scene-path param on scene.load / scene.loadAdditive (a String named
	// "scene"). Its pin-default key is the data-in index == the param index (an
	// EngineCall's data-ins are exactly its params, in order).
	int di = -1;
	for (size_t i = 0; i < n.params.size(); ++i)
		if (n.params[i].type == P::String && !n.params[i].isArray &&
		    (n.params[i].name == "scene" || n.params[i].name == "path"))
			{ di = (int)i; break; }
	if (di < 0) return false;

	std::string cur;
	if (auto it = n.pinDefaults.find(di); it != n.pinDefaults.end() && it->second.type == P::String)
		cur = it->second.s;

	bool changed = false;
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("Scene", cur.empty() ? "(pick a scene)" : cur.c_str()))
	{
		for (const auto& s : listScenes(cm))
			if (ImGui::Selectable((s.label + "##" + s.path).c_str(), cur == s.path))
			{
				V v; v.type = P::String; v.s = s.path;
				n.pinDefaults[di] = std::move(v);
				changed = true;
			}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("Project-relative scene path — packed + resolved\nby this exact string (no manual typing).");
	return changed;
}

int dragMatchPin(HorizonCode::NodeType t, HorizonCode::PinType dragType,
                 bool dragArray, bool srcIsInput, bool srcIsExec)
{
	// Probe a fresh node's signature. propType is seeded with the dragged type so
	// type-parametric nodes (array ops, Print, …) match — the host seeds the real
	// node the same way, keeping the computed pin index valid.
	HorizonCode::Node tpl;
	tpl.type = t; tpl.propType = dragType; tpl.isArray = dragArray;
	const HorizonCode::NodeSig s = HorizonCode::signatureOf(tpl);
	const int eIn = (int)s.execIns.size(), eOut = (int)s.execOuts.size();
	const int dIn = (int)s.dataIns.size();
	if (srcIsExec)
	{
		if (srcIsInput) return eOut ? eIn : -1;   // feed the dragged exec-in ← first exec-out
		return eIn ? 0 : -1;                       // dragged exec-out → first exec-in
	}
	if (srcIsInput)                                // dragged data-in ← a matching data-out
	{
		for (size_t i = 0; i < s.dataOuts.size(); ++i)
			if (s.dataOuts[i].type == dragType && s.dataOuts[i].isArray == dragArray)
				return eIn + eOut + dIn + (int)i;
		return -1;
	}
	for (size_t i = 0; i < s.dataIns.size(); ++i)  // dragged data-out → a matching data-in
		if (s.dataIns[i].type == dragType && s.dataIns[i].isArray == dragArray)
			return eIn + eOut + (int)i;
	return -1;
}

int dragMatchApiPin(const HE::api::ApiFn& fn, HorizonCode::PinType dragType,
                    bool dragArray, bool srcIsInput, bool srcIsExec)
{
	// EngineCall unified pins: [execIn?][execOut?][params…][results…].
	const int e = fn.isExec ? 1 : 0;
	if (srcIsExec) return fn.isExec ? (srcIsInput ? e : 0) : -1;
	if (srcIsInput)
	{
		for (size_t i = 0; i < fn.results.size(); ++i)
			if (fn.results[i].type == dragType && fn.results[i].isArray == dragArray)
				return e + e + (int)fn.params.size() + (int)i;
		return -1;
	}
	for (size_t i = 0; i < fn.params.size(); ++i)
		if (fn.params[i].type == dragType && fn.params[i].isArray == dragArray)
			return e + e + (int)i;
	return -1;
}
}
