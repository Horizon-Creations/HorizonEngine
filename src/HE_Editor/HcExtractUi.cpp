#include "HcExtractUi.h"
#include "HcExtract.h"
#include "HcGraphHost.h"     // loadClassGraph, variableTypeLabel
#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "EditorHelp.h"
#include "EditorWidgets.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <HorizonCode/HcClassResolve.h>
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <cstring>
#include <filesystem>
#include <set>
#include <vector>

namespace HcExtractUi
{
namespace
{
	namespace HC = HorizonCode;
	Targets s_targets;

	constexpr ImU32 kOk   = IM_COL32(120, 200, 130, 255);
	constexpr ImU32 kBad  = IM_COL32(235, 120, 90, 255);
	constexpr ImU32 kGrey = IM_COL32(150, 150, 150, 255);

	// The "New Struct from Variables…" popup's scratch, keyed to the graph it
	// was opened for so a tab switch does not carry ticks over.
	struct NewStructState
	{
		const HC::Graph*      forGraph = nullptr;
		std::string           name = "NewStruct";
		std::string           folder = "Types";
		std::set<std::string> ticked;
		std::string           error;
	};
	NewStructState s_new;

	std::string fromLabel(const std::string& var)
	{
		if (var.empty()) return {};
		return var == HC::kExtractSelf ? std::string("Self") : var;
	}

	// Write the struct asset and register it — TypeAssetPanel::saveState's
	// order (cycle gate, file, registry), with the asset made the way the
	// content browser's Create menu makes one.
	bool createStructAsset(const HE::StructDef& def, std::string& error)
	{
		ContentManager* cm = s_targets.content;
		if (!cm) { error = "no project is open"; return false; }
		if (HE::TypeRegistry::instance().structWouldCycle(def))
		{ error = "a field of that type would make the struct contain itself"; return false; }
		const std::string abs = cm->resolveAbsolutePath(def.assetPath);
		std::error_code ec;
		if (abs.empty()) { error = "the folder is outside the project"; return false; }
		if (std::filesystem::exists(abs, ec)) { error = "'" + def.assetPath + "' already exists"; return false; }
		std::filesystem::create_directories(std::filesystem::path(abs).parent_path(), ec);
		if (!HE::Ed::writeAssetStub(abs, def.assetPath, def.name, HE::AssetType::StructType))
		{ error = "could not write '" + def.assetPath + "'"; return false; }
		EditorAssetTypeCache::invalidate(abs);
		// Load, THEN take the pointer, and use it before anything else can load:
		// the getters point into a dense vector.
		const HE::UUID id = cm->loadAsset(def.assetPath);
		StructTypeAsset* a = cm->getStructTypeMutable(id);
		if (!a) { error = "the new asset could not be loaded"; return false; }
		a->json = HE::TypeRegistry::structToJson(def);
		if (!cm->saveAsset(*a)) { error = "could not save '" + def.assetPath + "'"; return false; }
		HE::TypeRegistry::instance().registerStruct(def);
		if (s_targets.onAssetCreated) s_targets.onAssetCreated(def.assetPath, abs);
		return true;
	}

	// The struct a class extracts, its own spec or the nearest ancestor's —
	// what its instances send. "" when it extracts nothing or is unknown.
	std::string extractStructOf(const std::string& classPath)
	{
		ContentManager* cm = s_targets.content;
		if (!cm || classPath.empty()) return {};
		// One parse per class and frame, the price HcPullUi pays for a creator.
		static std::string s_path, s_struct;
		static int s_frame = -1;
		if (s_path == classPath && s_frame == ImGui::GetFrameCount()) return s_struct;
		s_path = classPath;
		s_frame = ImGui::GetFrameCount();
		s_struct.clear();
		const HE::UUID id = cm->loadAsset(classPath);
		if (cm->assetType(id) == HE::AssetType::HorizonCodeClass)
		{
			const HC::ResolvedClass rc = HC::resolveClassAsset(*cm, classPath);
			for (auto lv = rc.levels.rbegin(); lv != rc.levels.rend() && s_struct.empty(); ++lv)
				s_struct = lv->extract.structPath;
		}
		else if (const UIWidgetAsset* w = cm->getWidget(id); w && !w->graphJson.empty())
		{
			HC::Graph g;
			if (HC::fromJson(w->graphJson, g)) s_struct = g.extract.structPath;
		}
		return s_struct;
	}

	bool drawNewStructPopup(HC::Graph& g)
	{
		bool edited = false;
		if (!ImGui::BeginPopup("##newExtractStruct")) return false;
		HE::Ed::Help::Scope helpScope("Extract on Destruct");
		if (s_new.forGraph != &g) { s_new = {}; s_new.forGraph = &g; }

		ImGui::TextUnformatted("New Struct from Variables");
		ImGui::TextDisabled("Tick what a listener should get.");
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("Name", &s_new.name);
		EditorWidgets::helpForLabel("Name");
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputText("Folder", &s_new.folder);
		EditorWidgets::helpForLabel("Folder");
		ImGui::Separator();
		const std::vector<HcExtract::ClassVar> vars = HcExtract::classVariables(g);
		if (vars.empty()) ImGui::TextDisabled("This class has no variables yet.");
		for (const HcExtract::ClassVar& cv : vars)
		{
			bool on = s_new.ticked.count(cv.decl.name) != 0;
			const std::string label = cv.decl.name + "  (" + HcGraphHost::variableTypeLabel(cv.decl) +
			                          (cv.inherited ? ", inherited" : "") + ")";
			if (ImGui::Checkbox(label.c_str(), &on))
			{
				if (on) s_new.ticked.insert(cv.decl.name);
				else    s_new.ticked.erase(cv.decl.name);
			}
		}
		ImGui::Separator();
		const bool nameOk = HcExtract::isValidStructName(s_new.name);
		const bool canCreate = nameOk && !s_new.ticked.empty();
		if (!canCreate) ImGui::BeginDisabled();
		if (EditorWidgets::button("Create"))
		{
			std::string folder = s_new.folder;
			while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
			const std::string rel = (folder.empty() ? std::string() : folder + "/") + s_new.name + ".hasset";
			// In the table's order, which is the class's: own first, inherited after.
			std::vector<std::string> picked;
			for (const HcExtract::ClassVar& cv : vars)
				if (s_new.ticked.count(cv.decl.name)) picked.push_back(cv.decl.name);
			const HE::StructDef def = HcExtract::structFromVariables(g, picked, s_new.name, rel);
			s_new.error.clear();
			if (createStructAsset(def, s_new.error))
			{
				// One struct per class: picking the new one replaces the spec and
				// maps every field to the variable it was made from.
				g.extract.structPath.clear();
				HcExtract::chooseStruct(g, def);
				HE_LOG_INFO(Editor, "Extract on Destruct: created %s with %zu members.",
				            rel.c_str(), def.fields.size());
				edited = true;
				s_new = {};
				ImGui::CloseCurrentPopup();
			}
		}
		if (!canCreate) ImGui::EndDisabled();
		ImGui::SameLine();
		if (EditorWidgets::button("Cancel")) { s_new = {}; ImGui::CloseCurrentPopup(); }
		if (!nameOk && !s_new.name.empty())
			ImGui::TextColored(ImColor(kBad), "Letters, digits and _ only, not starting with a digit.");
		if (!s_new.error.empty())
			ImGui::TextColored(ImColor(kBad), "%s", s_new.error.c_str());
		ImGui::EndPopup();
		return edited;
	}
}

void setTargets(Targets t) { s_targets = std::move(t); }

bool drawSidebarEntry(const HC::Graph& g, bool selected, const char* unavailableWhy)
{
	HE::Ed::Help::Scope helpScope("Extract on Destruct");
	ImGui::SeparatorText("Extract on Destruct");
	EditorWidgets::helpForLabel("Extract on Destruct");
	if (unavailableWhy)
	{
		ImGui::BeginDisabled();
		ImGui::Selectable("(not available)", false);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", unavailableWhy);
		return false;
	}
	const std::string label = g.extract.empty()
		? std::string("(none)##extract")
		: HcExtract::structLabel(g.extract.structPath) + "##extract";
	const bool clicked = ImGui::Selectable(label.c_str(), selected);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", g.extract.empty()
			? "Nothing is handed out when an instance is destroyed. Click to choose a struct."
			: "Handed to everything bound to an instance when it is destroyed, as OnDestroyed.");
	return clicked;
}

bool drawDetails(HC::Graph& g, const char* varPayload)
{
	HE::Ed::Help::Scope helpScope("Extract on Destruct");
	bool edited = false;
	ImGui::PushID("extract");
	ImGui::TextUnformatted("Extract on Destruct");
	ImGui::TextDisabled("When an instance is destroyed, this struct\ngoes to everything bound to it.");

	// ── the struct ───────────────────────────────────────────────────────────
	const std::vector<HE::StructDef> all = HE::TypeRegistry::instance().structs();
	const std::string preview = g.extract.empty() ? std::string("(none)")
	                                              : HcExtract::structLabel(g.extract.structPath);
	const bool open = ImGui::BeginCombo("Struct", preview.c_str());
	if (!open) EditorWidgets::helpForLabel("Struct");
	if (open)
	{
		if (ImGui::Selectable("(none)", g.extract.empty()) && !g.extract.empty())
		{ HcExtract::clear(g); edited = true; }
		for (const HE::StructDef& d : all)
			if (ImGui::Selectable((d.name + "##" + d.assetPath).c_str(), g.extract.structPath == d.assetPath))
				if (HcExtract::chooseStruct(g, d)) edited = true;   // pre-filled by name
		ImGui::EndCombo();
	}
	if (EditorWidgets::button("New Struct from Variables..."))
	{
		s_new = {};
		s_new.forGraph = &g;
		for (const HcExtract::ClassVar& cv : HcExtract::classVariables(g))
			if (!cv.inherited) s_new.ticked.insert(cv.decl.name);
		ImGui::OpenPopup("##newExtractStruct");
	}
	if (drawNewStructPopup(g)) edited = true;

	if (g.extract.empty())
	{
		ImGui::PopID();
		return edited;
	}

	HE::StructDef def;
	if (!HE::TypeRegistry::instance().getStruct(g.extract.structPath, def))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, kBad);
		ImGui::TextWrapped("'%s' is not a registered struct (deleted or moved?). OnDestroyed "
		                   "goes out without data until another one is chosen.",
		                   g.extract.structPath.c_str());
		ImGui::PopStyleColor();
		ImGui::PopID();
		return edited;
	}

	// ── one row per member ───────────────────────────────────────────────────
	ImGui::SeparatorText("Members");
	const std::vector<HcExtract::ClassVar> vars = HcExtract::classVariables(g);
	const std::vector<HcExtract::Row> rows = HcExtract::rows(g, def);
	std::string assignMember, assignVar;
	bool doAssign = false;
	for (const HcExtract::Row& r : rows)
	{
		ImGui::PushID(r.member.c_str());
		if (r.removed)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kBad);
			ImGui::TextUnformatted(r.member.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (EditorWidgets::dangerSmallButton("Remove"))
			{ assignMember = r.member; assignVar.clear(); doAssign = true; }
			ImGui::TextDisabled("removed from struct");
			ImGui::PopID();
			continue;
		}
		const HE::StructField& f = def.fields[(size_t)r.field];
		ImGui::TextUnformatted(r.member.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", r.type.c_str());

		const std::string cur = r.var.empty() ? "- " + r.defaultText : fromLabel(r.var);
		ImGui::SetNextItemWidth(-1.0f);
		if (!r.error.empty()) ImGui::PushStyleColor(ImGuiCol_Text, kBad);
		else if (r.var.empty()) ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
		const bool comboOpen = ImGui::BeginCombo("##from", cur.c_str());
		if (!r.error.empty() || r.var.empty()) ImGui::PopStyleColor();
		if (!comboOpen) EditorWidgets::helpForLabel("From");
		if (comboOpen)
		{
			if (ImGui::Selectable(("- " + r.defaultText).c_str(), r.var.empty()) && !r.var.empty())
			{ assignMember = r.member; assignVar.clear(); doAssign = true; }
			// Self only where it can land: a scalar Ref member.
			if (HcExtract::selfFit(f).empty() &&
			    ImGui::Selectable("Self", r.var == HC::kExtractSelf))
			{ assignMember = r.member; assignVar = HC::kExtractSelf; doAssign = true; }
			ImGui::Separator();
			for (const HcExtract::ClassVar& cv : vars)
			{
				const std::string why = HcExtract::fitOf(f, cv.decl);
				const std::string label = cv.decl.name + (cv.inherited ? "  (inherited)" : "");
				// Greyed with the reason rather than missing: whoever looks for a
				// variable here sees why it does not go in.
				if (!why.empty()) ImGui::BeginDisabled();
				if (ImGui::Selectable(label.c_str(), r.var == cv.decl.name))
				{ assignMember = r.member; assignVar = cv.decl.name; doAssign = true; }
				if (!why.empty())
				{
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s", why.c_str());
				}
			}
			ImGui::EndCombo();
		}
		// A variable dragged from the list onto the row maps it.
		if (varPayload && ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(varPayload))
			{
				std::string name(static_cast<const char*>(p->Data),
				                 strnlen(static_cast<const char*>(p->Data), (size_t)p->DataSize));
				for (const HcExtract::ClassVar& cv : vars)
					if (cv.decl.name == name)
					{
						if (const std::string why = HcExtract::fitOf(f, cv.decl); why.empty())
						{ assignMember = r.member; assignVar = name; doAssign = true; }
						else
							HE_LOG_WARN(Editor, "Extract on Destruct: '%s' does not fit '%s' (%s).",
							            name.c_str(), r.member.c_str(), why.c_str());
					}
			}
			ImGui::EndDragDropTarget();
		}
		if (!r.error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kBad);
			ImGui::TextWrapped("%s", r.error.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::PopID();
	}
	if (doAssign && HcExtract::assign(g, assignMember, assignVar)) edited = true;

	ImGui::Spacing();
	if (EditorWidgets::button("Auto-Map by Name"))
	{
		const int n = HcExtract::autoMapByName(g, def);
		if (n > 0) edited = true;
		HE_LOG_INFO(Editor, "Extract on Destruct: Auto-Map filled %d member(s).", n);
	}
	ImGui::SameLine();
	if (EditorWidgets::button("Clear")) { if (HcExtract::clearMap(g)) edited = true; }

	ImGui::Spacing();
	ImGui::TextDisabled("Listeners receive this as");
	ImGui::TextColored(ImColor(kOk), "OnDestroyed (%s)", def.name.c_str());
	ImGui::PopID();
	return edited;
}

std::string listNote(const HC::Graph& g, const HC::Variable& v, const std::string& pullNote)
{
	if (v.scope != 0 || HcExtract::extractedTo(g, v.name).empty() || g.extract.empty())
		return pullNote;
	if (!pullNote.empty())
	{
		// "Int  (pulled)" → "Int  (pulled, extracted)"
		if (!pullNote.empty() && pullNote.back() == ')')
			return pullNote.substr(0, pullNote.size() - 1) + ", extracted)";
		return pullNote + "  (extracted)";
	}
	return HcGraphHost::variableTypeLabel(v) + "  (extracted)";
}

void listTooltip(const HC::Graph& g, const HC::Variable& v)
{
	if (!ImGui::IsItemHovered()) return;
	const std::string d = HcExtract::describe(g, v.name);
	if (!d.empty()) ImGui::SetTooltip("%s", d.c_str());
}

bool drawVariableLine(const HC::Graph& g, const HC::Variable& v)
{
	if (v.scope != 0) return false;
	const std::string d = HcExtract::describe(g, v.name);
	if (d.empty()) return false;
	HE::Ed::Help::Scope helpScope("Extract on Destruct");
	ImGui::TextDisabled("%s", d.c_str());
	return EditorWidgets::button("Open Extract Table");
}

bool drawBindEventHelper(HC::Graph& g, const HC::Node& bind, const std::string& targetClassPath)
{
	HE::Ed::Help::Scope helpScope("Extract on Destruct");
	if (bind.type != HC::NodeType::BindEvent) return false;
	bool edited = false;
	ImGui::PushID("ondestroyed");
	ImGui::SeparatorText("OnDestroyed");

	// What the target sends: read from its class when the panel knows it.
	const std::string sent = extractStructOf(targetClassPath);
	static std::string s_pick;            // the struct chosen by hand when the target is unknown
	static int s_pickFor = -1;
	if (s_pickFor != bind.id) { s_pick.clear(); s_pickFor = bind.id; }
	std::string want = sent;
	if (targetClassPath.empty() || sent.empty())
	{
		if (!targetClassPath.empty())
			ImGui::TextDisabled("%s extracts nothing:\nOnDestroyed comes without data.",
			                    HcExtract::structLabel(targetClassPath).c_str());
		else
		{
			const std::string prev = s_pick.empty() ? std::string("(no data)") : HcExtract::structLabel(s_pick);
			const bool open = ImGui::BeginCombo("Data", prev.c_str());
			if (!open) EditorWidgets::helpForLabel("Data");
			if (open)
			{
				if (ImGui::Selectable("(no data)", s_pick.empty())) s_pick.clear();
				for (const HE::StructDef& d : HE::TypeRegistry::instance().structs())
					if (ImGui::Selectable((d.name + "##" + d.assetPath).c_str(), s_pick == d.assetPath))
						s_pick = d.assetPath;
				ImGui::EndCombo();
			}
		}
		want = targetClassPath.empty() ? s_pick : std::string();
	}
	else
		ImGui::TextDisabled("%s sends %s.", HcExtract::structLabel(targetClassPath).c_str(),
		                    HcExtract::structLabel(sent).c_str());

	std::string have;
	const bool hasOne = HcExtract::onDestroyedArg(g, have);
	if (hasOne && have == want)
		ImGui::TextColored(ImColor(kOk), "This class has OnDestroyed (%s).",
		                   want.empty() ? "no data" : HcExtract::structLabel(want).c_str());
	else
	{
		if (hasOne)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kBad);
			ImGui::TextWrapped("This class's OnDestroyed takes %s; this target sends %s. "
			                   "One OnDestroyed per class: give the senders the same struct.",
			                   have.empty() ? "no data" : HcExtract::structLabel(have).c_str(),
			                   want.empty() ? "no data" : HcExtract::structLabel(want).c_str());
			ImGui::PopStyleColor();
		}
		if (hasOne) ImGui::BeginDisabled();
		if (EditorWidgets::button("Create OnDestroyed Event"))
		{
			std::string what;
			const HcExtract::Listener r = HcExtract::createOnDestroyedEvent(g, want, &what);
			if (r == HcExtract::Listener::Created || r == HcExtract::Listener::HandlerAdded)
				edited = true;
			HE_LOG_INFO(Editor, "Extract on Destruct: %s.", what.c_str());
		}
		if (hasOne) ImGui::EndDisabled();
	}
	ImGui::PopID();
	return edited;
}
}
