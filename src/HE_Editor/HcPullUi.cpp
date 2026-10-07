#include "HcPullUi.h"
#include "HcPull.h"
#include "HcEditorUtil.h"
#include "HcGraphHost.h"     // loadClassGraph (flattened), variableTypeLabel
#include "EditorHelp.h"
#include "EditorWidgets.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <vector>

namespace HcPullUi
{
namespace
{
	namespace HC = HorizonCode;
	Targets s_targets;

	constexpr ImU32 kOk  = IM_COL32(120, 200, 130, 255);
	constexpr ImU32 kBad = IM_COL32(235, 120, 90, 255);

	std::string stem(const std::string& path)
	{
		std::string s = path;
		if (const size_t p = s.find_last_of("/\\"); p != std::string::npos) s.erase(0, p + 1);
		if (const size_t d = s.rfind('.'); d != std::string::npos) s.erase(d);
		return s;
	}

	// The source's public variables, or false when the source class is not
	// known here (a creator without an expected class, or no Game Instance).
	// A creator's class is read flattened, so what it inherits is pullable too.
	bool sourceVariables(const HC::Variable& v, std::vector<HcPull::SourceVar>& out)
	{
		out.clear();
		if (v.pullSource == HC::kPullFromGameInstance)
		{
			if (!s_targets.gameInstance) return false;
			out = HcPull::publicVariables(*s_targets.gameInstance);
			return true;
		}
		if (v.pullSource == HC::kPullFromCreator && !v.pullClass.empty())
		{
			// One parse per class and frame is what the type picker already
			// pays for its whole list; this is one asset.
			static std::string s_path;
			static int s_frame = -1;
			static std::vector<HcPull::SourceVar> s_vars;
			static bool s_ok = false;
			if (s_path != v.pullClass || s_frame != ImGui::GetFrameCount())
			{
				HC::Graph g;
				s_ok = HcGraphHost::loadClassGraph(s_targets.content, v.pullClass, g);
				s_vars = s_ok ? HcPull::publicVariables(g) : std::vector<HcPull::SourceVar>{};
				s_path = v.pullClass;
				s_frame = ImGui::GetFrameCount();
			}
			out = s_vars;
			return s_ok;
		}
		return false;
	}

	// Write "Add to Target" into the source. Game Instance: the live graph,
	// then its commit (re-registers it and saves). A class asset: through the
	// ContentManager, refused while its tab holds unsaved changes.
	void addToTarget(const HC::Variable& v, const std::string& structType)
	{
		std::string what;
		if (v.pullSource == HC::kPullFromGameInstance)
		{
			if (!s_targets.gameInstance) return;
			const HcPull::Add r = HcPull::addToTarget(*s_targets.gameInstance, v, structType, &what);
			if (r == HcPull::Add::AddedVariable || r == HcPull::Add::AddedStructVar ||
			    r == HcPull::Add::MadePublic)
			{
				if (s_targets.commitGameInstance) s_targets.commitGameInstance();
				HE_LOG_INFO(Editor, "Add to Target: %s in the Game Instance.", what.c_str());
			}
			else if (!what.empty())
				HE_LOG_WARN(Editor, "Add to Target: %s.", what.c_str());
			return;
		}
		ContentManager* cm = s_targets.content;
		if (!cm || v.pullClass.empty()) return;
		if (s_targets.blockedBecause)
			if (const std::string why = s_targets.blockedBecause(v.pullClass); !why.empty())
			{
				HE_LOG_WARN(Editor, "Add to Target: %s is %s - save or close it first.",
				            stem(v.pullClass).c_str(), why.c_str());
				return;
			}
		// Load first, THEN take the pointer, and use it before anything else
		// can load: the getters point into a dense vector.
		const HE::UUID id = cm->loadAsset(v.pullClass);
		std::string* json = nullptr;
		RuntimeAsset* asset = nullptr;
		if (HorizonCodeClassAsset* a = cm->getHorizonCodeClassMutable(id)) { json = &a->graphJson; asset = a; }
		else if (UIWidgetAsset* w = cm->getWidgetMutable(id))             { json = &w->graphJson; asset = w; }
		if (!json) return;
		HC::Graph g;
		if (!json->empty() && !HC::fromJson(*json, g)) return;
		const HcPull::Add r = HcPull::addToTarget(g, v, structType, &what);
		if (r != HcPull::Add::AddedVariable && r != HcPull::Add::AddedStructVar &&
		    r != HcPull::Add::MadePublic)
		{
			if (!what.empty()) HE_LOG_WARN(Editor, "Add to Target: %s.", what.c_str());
			return;
		}
		*json = HC::toJson(g);
		if (cm->saveAsset(*asset))
			HE_LOG_INFO(Editor, "Add to Target: %s in %s.", what.c_str(), stem(v.pullClass).c_str());
	}

	// The member case where the struct variable exists but its definition
	// lacks the field: append it to the struct asset and re-register it.
	void addMemberToStruct(const HC::Variable& v, const std::string& structPath)
	{
		ContentManager* cm = s_targets.content;
		if (!cm || structPath.empty()) return;
		HE::StructDef def;
		if (!HE::TypeRegistry::instance().getStruct(structPath, def)) return;
		if (!HcPull::addMember(def, v, v.pullMember)) return;
		if (HE::TypeRegistry::instance().structWouldCycle(def))
		{
			HE_LOG_WARN(Editor, "Add to Target: a field of that type would make %s refer to itself.",
			            stem(structPath).c_str());
			return;
		}
		const HE::UUID id = cm->loadAsset(structPath);
		StructTypeAsset* a = cm->getStructTypeMutable(id);
		if (!a) return;
		a->json = HE::TypeRegistry::structToJson(def);
		if (!cm->saveAsset(*a)) return;
		HE::TypeRegistry::instance().registerStruct(def);
		HE_LOG_INFO(Editor, "Add to Target: added member '%s' to %s.", v.pullMember.c_str(),
		            stem(structPath).c_str());
	}
}

void setTargets(Targets t) { s_targets = std::move(t); }

const char* defaultSectionLabel(const HC::Variable& v)
{
	return v.pullSource.empty() ? "Default" : "Fallback";
}

std::string listNote(const HC::Variable& v)
{
	if (v.pullSource.empty()) return {};
	return HcGraphHost::variableTypeLabel(v) + "  (pulled)";
}

void listTooltip(const HC::Variable& v)
{
	if (!v.pullSource.empty() && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", HcPull::describe(v).c_str());
}

bool drawSection(HC::Variable& v, const HC::Graph& owner)
{
	if (v.scope != 0) return false;
	// The Game Instance never pulls (design §2.3): it would be its own source.
	if (s_targets.gameInstance && &owner == s_targets.gameInstance) return false;

	bool edited = false;
	bool on = !v.pullSource.empty();
	if (EditorWidgets::checkbox("Pull on Construct", &on))
	{
		if (on) v.pullSource = HC::kPullFromGameInstance;
		else    { v.pullSource.clear(); v.pullVar.clear(); v.pullMember.clear(); v.pullClass.clear(); }
		edited = true;
	}
	EditorWidgets::helpForLabel("Pull on Construct");
	if (!on) return edited;

	ImGui::PushID("pull");
	ImGui::Indent();

	// ── Source ───────────────────────────────────────────────────────────────
	{
		const char* items[] = { "Game Instance", "Creator" };
		int cur = v.pullSource == HC::kPullFromCreator ? 1 : 0;
		if (ImGui::Combo("Source", &cur, items, 2))
		{
			v.pullSource = cur == 1 ? HC::kPullFromCreator : HC::kPullFromGameInstance;
			if (cur != 1) v.pullClass.clear();
			edited = true;
		}
		EditorWidgets::helpForLabel("Source");
	}

	// ── Expected creator class ───────────────────────────────────────────────
	if (v.pullSource == HC::kPullFromCreator)
	{
		const std::string shown = v.pullClass.empty() ? std::string("(any creator)") : stem(v.pullClass);
		if (ImGui::BeginCombo("Creator Class", shown.c_str()))
		{
			if (ImGui::Selectable("(any creator)", v.pullClass.empty()))
			{ v.pullClass.clear(); edited = true; }
			for (HE::AssetType t : { HE::AssetType::HorizonCodeClass, HE::AssetType::Widget })
				for (const HcEditorUtil::ClassRef& c : HcEditorUtil::listAssets(s_targets.content, t))
					if (ImGui::Selectable((c.label + "##" + c.path).c_str(), c.path == v.pullClass))
					{ v.pullClass = c.path; edited = true; }
			ImGui::EndCombo();
		}
		EditorWidgets::helpForLabel("Creator Class");
	}

	// ── Variable / Member ────────────────────────────────────────────────────
	std::vector<HcPull::SourceVar> vars;
	const bool known = sourceVariables(v, vars);
	const HcPull::SourceVar* chosen = nullptr;
	for (const HcPull::SourceVar& sv : vars)
		if (sv.name == v.pullVar) chosen = &sv;
	if (known)
	{
		const std::string shown = v.pullVar.empty() ? std::string("(pick one)") : v.pullVar;
		if (ImGui::BeginCombo("Variable", shown.c_str()))
		{
			for (const HcPull::SourceVar& sv : vars)
			{
				// Everything is listed; what cannot land here is greyed with the
				// reason, so a variable that is "missing" explains itself.
				const std::string why = HcPull::fitOf(v, sv);
				const bool viaMember = !why.empty() && !HcPull::fittingMembers(v, sv).empty();
				ImGui::BeginDisabled(!why.empty() && !viaMember);
				const std::string label = sv.name + "  " +
					HcGraphHost::variableTypeLabel([&] {
						HC::Variable t; t.name = sv.name; t.type = sv.type; t.typeName = sv.typeName;
						t.className = sv.className; t.isArray = sv.kind != HC::ContainerKind::None;
						t.container = sv.kind; t.keyType = sv.keyType; return t; }());
				if (ImGui::Selectable((label + "##" + sv.name).c_str(), sv.name == v.pullVar))
				{
					v.pullVar = sv.name;
					// A whole-value fit pulls the whole value; a struct that only
					// fits through a member starts at its first fitting member.
					v.pullMember = viaMember ? HcPull::fittingMembers(v, sv).front() : std::string();
					edited = true;
				}
				ImGui::EndDisabled();
				if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", why.c_str());
			}
			ImGui::EndCombo();
		}
	}
	else
	{
		// Nothing to list from: the name is typed, and the runtime checks it.
		ImGui::InputText("Variable", &v.pullVar);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
	}
	EditorWidgets::helpForLabel("Variable");

	const bool structSource = chosen && chosen->type == HC::PinType::Struct &&
	                          chosen->kind == HC::ContainerKind::None;
	if (structSource)
	{
		HE::StructDef def;
		HE::TypeRegistry::instance().getStruct(chosen->typeName, def);
		const std::string shown = v.pullMember.empty() ? std::string("(whole struct)") : v.pullMember;
		if (ImGui::BeginCombo("Member", shown.c_str()))
		{
			const std::string wholeWhy = HcPull::fitOf(v, *chosen);
			ImGui::BeginDisabled(!wholeWhy.empty());
			if (ImGui::Selectable("(whole struct)", v.pullMember.empty()))
			{ v.pullMember.clear(); edited = true; }
			ImGui::EndDisabled();
			for (const HE::StructField& f : def.fields)
			{
				const std::string why = HcPull::fitOf(v, *chosen, f.name);
				ImGui::BeginDisabled(!why.empty());
				if (ImGui::Selectable(f.name.c_str(), f.name == v.pullMember))
				{ v.pullMember = f.name; edited = true; }
				ImGui::EndDisabled();
				if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", why.c_str());
			}
			ImGui::EndCombo();
		}
		EditorWidgets::helpForLabel("Member");
	}
	else if (!known || (!chosen && !v.pullVar.empty()))
	{
		// No struct to pick from: a member can still be named (the source is
		// unknown here, or the variable does not exist there yet).
		ImGui::InputText("Member", &v.pullMember);
		if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
		EditorWidgets::helpForLabel("Member");
	}

	// ── Status, the same sentence the runtime warns with ─────────────────────
	const HcPull::Status st = HcPull::check(v, known ? &vars : nullptr);
	if (!st.liveMember.empty())
	{
		// Resolved under a former name: write the live one, once.
		v.pullMember = st.liveMember;
		edited = true;
	}
	if (!st.text.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, st.ok ? kOk : kBad);
		ImGui::TextWrapped("%s %s", st.ok ? "OK:" : "Default used:", st.text.c_str());
		ImGui::PopStyleColor();
	}

	// ── Add to Target ────────────────────────────────────────────────────────
	// Only where there IS a target to write into: the Game Instance, or the
	// creator's expected class. And only for what is missing — a type clash
	// is the author's to settle, not something to paper over.
	const bool canWrite = known && !v.pullVar.empty() &&
	                      (st.why == HC::PullFailure::NoPublicVariable ||
	                       st.why == HC::PullFailure::NoSuchMember);
	if (canWrite)
	{
		static std::string s_structType;   // the member case's pick, per session
		const bool memberMissingVar = !v.pullMember.empty() && !chosen;
		const bool memberMissingField = !v.pullMember.empty() && structSource;
		if (memberMissingVar)
		{
			const std::vector<std::string> fits = HcPull::structsWithMember(v, v.pullMember);
			if (fits.empty())
				ImGui::TextDisabled("No struct has a fitting member '%s'.", v.pullMember.c_str());
			else
			{
				if (std::find(fits.begin(), fits.end(), s_structType) == fits.end())
					s_structType = fits.front();
				if (ImGui::BeginCombo("Struct Type", stem(s_structType).c_str()))
				{
					for (const std::string& p : fits)
						if (ImGui::Selectable((stem(p) + "##" + p).c_str(), p == s_structType))
							s_structType = p;
					ImGui::EndCombo();
				}
				EditorWidgets::helpForLabel("Struct Type");
			}
		}
		const bool ready = !memberMissingVar || !s_structType.empty();
		ImGui::BeginDisabled(!ready);
		if (EditorWidgets::button("Add to Target"))
		{
			if (memberMissingField) addMemberToStruct(v, chosen->typeName);
			else                    addToTarget(v, memberMissingVar ? s_structType : std::string());
		}
		ImGui::EndDisabled();
		EditorWidgets::helpForLabel("Add to Target");
	}

	ImGui::Unindent();
	ImGui::PopID();
	return edited;
}

} // namespace HcPullUi
