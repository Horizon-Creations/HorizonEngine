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
	// known here (a creator without an expected class, a reference of no
	// declared class, or no Game Instance). A class is read flattened, so what
	// it inherits is pullable too. `classPath` = HcPull::sourceClassOf.
	bool sourceVariables(const HC::Variable& v, const std::string& classPath,
	                     std::vector<HcPull::SourceVar>& out)
	{
		out.clear();
		if (v.pullSource == HC::kPullFromGameInstance)
		{
			if (!s_targets.gameInstance) return false;
			out = HcPull::publicVariables(*s_targets.gameInstance);
			return true;
		}
		if (!classPath.empty())
		{
			// One parse per class and frame is what the type picker already
			// pays for its whole list; this is one asset.
			static std::string s_path;
			static int s_frame = -1;
			static std::vector<HcPull::SourceVar> s_vars;
			static bool s_ok = false;
			if (s_path != classPath || s_frame != ImGui::GetFrameCount())
			{
				HC::Graph g;
				s_ok = HcGraphHost::loadClassGraph(s_targets.content, classPath, g);
				s_vars = s_ok ? HcPull::publicVariables(g) : std::vector<HcPull::SourceVar>{};
				s_path = classPath;
				s_frame = ImGui::GetFrameCount();
			}
			out = s_vars;
			return s_ok;
		}
		return false;
	}

	// Write "Add to Target" into the source. Game Instance: the live graph,
	// then its commit (re-registers it and saves). A class asset (`classPath`:
	// the creator class or the reference's class): through the
	// ContentManager, refused while its tab holds unsaved changes.
	void addToTarget(const HC::Variable& v, const std::string& classPath,
	                 const std::string& structType)
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
		if (!cm || classPath.empty()) return;
		if (s_targets.blockedBecause)
			if (const std::string why = s_targets.blockedBecause(classPath); !why.empty())
			{
				HE_LOG_WARN(Editor, "Add to Target: %s is %s - save or close it first.",
				            stem(classPath).c_str(), why.c_str());
				return;
			}
		// Load first, THEN take the pointer, and use it before anything else
		// can load: the getters point into a dense vector.
		const HE::UUID id = cm->loadAsset(classPath);
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
			HE_LOG_INFO(Editor, "Add to Target: %s in %s.", what.c_str(), stem(classPath).c_str());
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
	if (v.pullSource.empty()) return "Default";
	// Bound through a reference: the default is what it holds until the
	// reference first points somewhere — a start, not a fallback.
	return v.bindTo && v.pullSource == HC::kPullFromRef ? "Initial Value" : "Fallback";
}

std::string listNote(const HC::Variable& v)
{
	if (v.pullSource.empty()) return {};
	return HcGraphHost::variableTypeLabel(v) + (v.bindTo ? "  (bound)" : "  (pulled)");
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
	// Bind To and Replicated exclude each other (plan §0.1): on a client the
	// replicator and the binding would both write. The loader drops the
	// binding anyway; here it simply falls back to the pull it extends.
	if (v.replicated && v.bindTo) { v.bindTo = false; v.pullRef.clear(); edited = true; }
	if (v.replicated && v.pullSource == HC::kPullFromRef)
	{ v.pullSource = HC::kPullFromGameInstance; v.pullVar.clear(); v.pullMember.clear(); edited = true; }

	// ── Source Mode: Off | Pull on Construct | Bind To (plan §5.1) ───────────
	// One declaration with two lifetimes: switching between the two keeps
	// every source field, only Bind To's reference source has no Pull form.
	{
		const char* items[] = { "Off", "Pull on Construct", "Bind To" };
		int cur = v.pullSource.empty() ? 0 : v.bindTo ? 2 : 1;
		const int was = cur;
		if (ImGui::BeginCombo("Source Mode", items[cur]))
		{
			for (int i = 0; i < 3; ++i)
			{
				const bool blocked = i == 2 && v.replicated;
				ImGui::BeginDisabled(blocked);
				if (ImGui::Selectable(items[i], i == cur)) cur = i;
				ImGui::EndDisabled();
				if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Not on a Replicated variable: the replicator and the "
					                  "binding would both write it.");
			}
			ImGui::EndCombo();
		}
		EditorWidgets::helpForLabel("Source Mode");
		if (cur != was)
		{
			if (cur == 0)
			{
				v.pullSource.clear(); v.pullVar.clear(); v.pullMember.clear(); v.pullClass.clear();
				v.bindTo = false; v.pullRef.clear();
			}
			else
			{
				if (v.pullSource.empty()) v.pullSource = HC::kPullFromGameInstance;
				v.bindTo = cur == 2;
				if (!v.bindTo && v.pullSource == HC::kPullFromRef)
				{
					v.pullSource = HC::kPullFromGameInstance;
					v.pullVar.clear(); v.pullMember.clear();
				}
				if (!v.bindTo) v.pullRef.clear();
			}
			edited = true;
		}
	}
	if (v.pullSource.empty()) return edited;

	ImGui::PushID("pull");
	ImGui::Indent();

	// ── Source ───────────────────────────────────────────────────────────────
	{
		// Reference only for Bind To: at construction the reference is still
		// null, so a pull through it would only ever see the default.
		const char* items[] = { "Game Instance", "Creator", "Reference" };
		int cur = v.pullSource == HC::kPullFromCreator ? 1 : v.pullSource == HC::kPullFromRef ? 2 : 0;
		if (ImGui::Combo("Source", &cur, items, v.bindTo ? 3 : 2))
		{
			v.pullSource = cur == 1 ? HC::kPullFromCreator
			             : cur == 2 ? HC::kPullFromRef : HC::kPullFromGameInstance;
			if (cur != 1) v.pullClass.clear();
			if (cur != 2) v.pullRef.clear();
			edited = true;
		}
		EditorWidgets::helpForLabel("Source");
	}

	// ── The reference to bind through ────────────────────────────────────────
	if (v.pullSource == HC::kPullFromRef)
	{
		const std::vector<std::string> refs = HcPull::refVariables(owner);
		const std::string shown = v.pullRef.empty() ? std::string("(pick one)") : v.pullRef;
		if (ImGui::BeginCombo("Reference", shown.c_str()))
		{
			for (const std::string& r : refs)
			{
				const std::string cls = HcPull::refClassOf(owner, r);
				const std::string label = r + (cls.empty() ? std::string("  (any object)")
				                                           : "  " + stem(cls));
				if (ImGui::Selectable((label + "##" + r).c_str(), r == v.pullRef))
				{
					if (r != v.pullRef) { v.pullVar.clear(); v.pullMember.clear(); }
					v.pullRef = r;
					edited = true;
				}
			}
			if (refs.empty())
				ImGui::TextDisabled("This class has no object reference variable.");
			ImGui::EndCombo();
		}
		EditorWidgets::helpForLabel("Reference");
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
	const std::string classPath = HcPull::sourceClassOf(v, owner);
	const bool known = sourceVariables(v, classPath, vars);
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
		ImGui::TextWrapped("%s %s", st.ok ? "OK:" : v.bindTo ? "Not bound:" : "Default used:",
		                   st.text.c_str());
		ImGui::PopStyleColor();
	}

	// ── Add to Target ────────────────────────────────────────────────────────
	// Only where there IS a target to write into: the Game Instance, the
	// creator's expected class or the reference's class. And only for what is
	// missing — a type clash is the author's to settle, not something to paper
	// over.
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
			else addToTarget(v, classPath, memberMissingVar ? s_structType : std::string());
		}
		ImGui::EndDisabled();
		EditorWidgets::helpForLabel("Add to Target");
	}

	ImGui::Unindent();
	ImGui::PopID();
	return edited;
}

} // namespace HcPullUi
