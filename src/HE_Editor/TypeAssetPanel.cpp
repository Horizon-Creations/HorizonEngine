#include "TypeAssetPanel.h"
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"     // shared per-tab state map
#include "EditorToolbar.h"        // shared toolbar strip
#include "EditorWidgets.h"        // danger buttons for deletion
#include "HcEditorUtil.h"         // listAssets (Enum/Struct pickers for field types)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>
#include <Types/TypeRegistry.h>
#include <CppTypesHeaderGen.h>  // regenerate GameTypes.h on save (C++ projects)
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <string>
#include <vector>

// The panel edits a decoded HE::StructDef / HE::EnumDef and re-serializes on
// Save (TypeRegistry round-trip) — hand-edited/unknown JSON fields are NOT
// preserved, matching the editor-owned payload contract. Save also re-registers
// the definition in the TypeRegistry, so every type dropdown updates live.
namespace
{

using HorizonCode::PinType;

struct PanelState
{
	bool loaded = false;
	bool dirty  = false;
	bool isEnum = false;
	bool isTemplate = false;   // savegame template: struct-shaped fields, not a type
	std::string name;      // asset stem — the type's display name
	std::string relPath;   // content-relative path — the TypeRegistry key
	HE::UUID    assetId;
	HE::StructDef structDef;
	HE::EnumDef   enumDef;
	std::string lastSaveError; // shown under the toolbar until the next save
};
AssetPanelState<PanelState> s_states;

bool sniffType(const std::string& path, HE::AssetType type)
{
	return EditorAssetTypeCache::is(path, type);
}

// The field types a struct may declare. Exec is control flow and Ref is a
// runtime instance handle — neither is data a definition can hold.
constexpr PinType kFieldTypes[] = {
	PinType::Float, PinType::Int, PinType::Bool, PinType::String,
	PinType::Vec2, PinType::Color, PinType::Transform,
	PinType::Enum, PinType::Struct,
};

const char* fieldTypeLabel(PinType t)
{
	switch (t)
	{
	case PinType::Float:     return "Float";
	case PinType::Int:       return "Int";
	case PinType::Bool:      return "Bool";
	case PinType::String:    return "String";
	case PinType::Vec2:      return "Vec2";
	case PinType::Color:     return "Color";
	case PinType::Transform: return "Transform";
	case PinType::Enum:      return "Enum";
	case PinType::Struct:    return "Struct";
	default:                 return "?";
	}
}

// Persist a tab's decoded model back into its asset + the TypeRegistry. The
// header's Save button AND the close/quit prompt's "Save All" both come through
// here. A struct that would close a reference cycle refuses to save.
bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	st.lastSaveError.clear();
	auto& reg = HE::TypeRegistry::instance();
	if (st.isEnum)
	{
		EnumTypeAsset* a = ctx.contentManager->getEnumTypeMutable(st.assetId);
		if (!a) return false;
		st.enumDef.name = st.name;
		st.enumDef.assetPath = st.relPath;
		a->json = HE::TypeRegistry::enumToJson(st.enumDef);
		if (!ctx.contentManager->saveAsset(*a)) return false;
		reg.registerEnum(st.enumDef);
	}
	else if (st.isTemplate)
	{
		// A template is not a registered type — just its field schema on disk.
		SaveGameTemplateAsset* a = ctx.contentManager->getSaveGameTemplateMutable(st.assetId);
		if (!a) return false;
		st.structDef.name = st.name;
		st.structDef.assetPath = st.relPath;
		a->json = HE::TypeRegistry::structToJson(st.structDef);
		if (!ctx.contentManager->saveAsset(*a)) return false;
	}
	else
	{
		st.structDef.name = st.name;
		st.structDef.assetPath = st.relPath;
		if (reg.structWouldCycle(st.structDef))
		{
			st.lastSaveError =
				"Not saved: a Struct field refers back to this struct (directly or "
				"through another struct) — that cycle would never finish. Remove the "
				"circular field first.";
			return false;
		}
		StructTypeAsset* a = ctx.contentManager->getStructTypeMutable(st.assetId);
		if (!a) return false;
		a->json = HE::TypeRegistry::structToJson(st.structDef);
		if (!ctx.contentManager->saveAsset(*a)) return false;
		reg.registerStruct(st.structDef);
	}
	// C++ projects: the definitions ARE C++ types — regenerate the header so
	// gameplay code sees this save on its next compile.
	if (ctx.projectManager &&
	    ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Cpp)
	{
		std::filesystem::path projectPath = ctx.projectManager->currentProject().path;
		if (std::filesystem::is_regular_file(projectPath))
			projectPath = projectPath.parent_path();
		if (!projectPath.empty())
			HE::writeCppTypesHeader(projectPath);
	}
	st.dirty = false;
	return true;
}

// Inline default-value editor for one scalar field. Struct fields carry no
// inline default (the nested definition supplies its own); ARRAY fields get the
// shared slot editor instead (see the isArray branch at the call site).
void defaultValueEditor(HE::StructField& f, bool& dirty)
{
	HorizonCode::Value& v = f.defaultValue;
	v.type = f.type;
	switch (f.type)
	{
	case PinType::Float:
		if (ImGui::DragFloat("##def", &v.f, 0.05f)) dirty = true;
		break;
	case PinType::Int:
		if (ImGui::DragInt("##def", &v.i, 0.25f)) dirty = true;
		break;
	case PinType::Bool:
		if (ImGui::Checkbox("##def", &v.b)) dirty = true;
		break;
	case PinType::String:
		if (ImGui::InputText("##def", &v.s)) dirty = true;
		break;
	case PinType::Vec2:
		if (ImGui::DragFloat2("##def", &v.v2.x, 0.05f)) dirty = true;
		break;
	case PinType::Color:
		if (ImGui::ColorEdit4("##def", &v.col.x,
		        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) dirty = true;
		break;
	case PinType::Transform:
	{
		// Compact: one row per part, labelled by tooltip.
		if (ImGui::DragFloat3("##defpos", &v.tpos.x, 0.05f)) dirty = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position");
		if (ImGui::DragFloat3("##defrot", &v.trot.x, 0.5f))  dirty = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation (euler degrees)");
		if (ImGui::DragFloat3("##defscl", &v.tscl.x, 0.05f)) dirty = true;
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale");
		break;
	}
	case PinType::Enum:
	{
		// The default is the entry NAME (robust across renumbering).
		HE::EnumDef ed;
		if (f.typeName.empty() ||
		    !HE::TypeRegistry::instance().getEnum(f.typeName, ed) || ed.entries.empty())
		{
			ImGui::TextDisabled("pick an enum first");
			break;
		}
		const char* shown = v.s.empty() ? ed.entries.front().name.c_str() : v.s.c_str();
		if (ImGui::BeginCombo("##def", shown))
		{
			for (const auto& e : ed.entries)
				if (ImGui::Selectable(e.name.c_str(), e.name == v.s))
				{ v.s = e.name; dirty = true; }
			ImGui::EndCombo();
		}
		break;
	}
	default:
		ImGui::TextDisabled("\xe2\x80\x94");
		break;
	}
}

// Combo over the project's Enum or Struct assets, for a field's typeName.
// `excludePath` hides the struct being edited (a direct self-reference can
// never save anyway — structWouldCycle — so don't even offer it).
void typeNamePicker(HE::StructField& f, bool& dirty, AppContext& ctx,
                    const std::string& excludePath)
{
	const bool wantEnum = f.type == PinType::Enum;
	const auto  assets  = HcEditorUtil::listAssets(ctx.contentManager,
		wantEnum ? HE::AssetType::EnumType : HE::AssetType::StructType);
	const std::string shown = f.typeName.empty()
		? std::string(wantEnum ? "Select an enum\xe2\x80\xa6" : "Select a struct\xe2\x80\xa6")
		: std::filesystem::path(f.typeName).stem().string();
	if (ImGui::BeginCombo("##typename", shown.c_str()))
	{
		for (const auto& a : assets)
		{
			if (!wantEnum && a.path == excludePath) continue;
			if (ImGui::Selectable(a.label.c_str(), a.path == f.typeName))
			{ f.typeName = a.path; f.defaultValue.s.clear(); dirty = true; }
		}
		if (assets.empty())
			ImGui::TextDisabled(wantEnum ? "No Enum assets in the project"
			                             : "No other Struct assets in the project");
		ImGui::EndCombo();
	}
}

} // namespace

bool TypeAssetPanel::isStructAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::StructType); }
bool TypeAssetPanel::isEnumAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::EnumType); }
bool TypeAssetPanel::isSaveTemplateAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::SaveGameTemplate); }
bool TypeAssetPanel::isTypeAsset(const std::string& path)
{ return isStructAsset(path) || isEnumAsset(path) || isSaveTemplateAsset(path); }

bool TypeAssetPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool TypeAssetPanel::reloadFromDisk(const std::string& assetPath)
{
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty = false;
	return true;
}

void TypeAssetPanel::appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }

bool TypeAssetPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	if (!st || !st->dirty) return true; // "not mine" reads as success (see InputAssetPanel)
	return saveState(*st, ctx);
}

void TypeAssetPanel::forget(const std::string& path) { s_states.forget(path); }

void TypeAssetPanel::render(AppContext& ctx, const std::string& assetPath,
                            const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		st.relPath  = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId  = ctx.contentManager->loadAsset(st.relPath);
		st.isEnum   = isEnumAsset(assetPath);
		st.isTemplate = isSaveTemplateAsset(assetPath);
		st.name     = std::filesystem::path(assetPath).stem().string();
		st.structDef = {};
		st.enumDef   = {};
		if (const EnumTypeAsset* a = ctx.contentManager->getEnumType(st.assetId))
			HE::TypeRegistry::enumFromJson(a->json, st.enumDef);
		else if (const StructTypeAsset* a2 = ctx.contentManager->getStructType(st.assetId))
			HE::TypeRegistry::structFromJson(a2->json, st.structDef);
		else if (const SaveGameTemplateAsset* a3 = ctx.contentManager->getSaveGameTemplate(st.assetId))
			HE::TypeRegistry::structFromJson(a3->json, st.structDef); // same field shape
		st.loaded = true;
	}

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##typeasset_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconGear, st.dirty);
		bar.group();
		bar.readout(nullptr, st.isEnum ? "Enum" : st.isTemplate ? "SaveGame Template" : "Struct", T::kFgDim);
		bar.endGroup();
		if (T::saveButton(bar, true)) saveState(st, ctx);
	}
	if (!st.lastSaveError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 80, 80, 255));
		ImGui::TextWrapped("%s", st.lastSaveError.c_str());
		ImGui::PopStyleColor();
	}
	if (!st.isTemplate && HE::TypeRegistry::instance().nameCollides(st.name, st.relPath))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(214, 122, 30, 255));
		ImGui::TextWrapped("Another type in this project is also named \"%s\" — the "
			"generated horizon.enums/horizon.structs entries and C++ symbols will "
			"collide. Rename one of them.", st.name.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();

	if (st.isEnum)
	{
		// ── Enum: one row per entry (name + value) ──────────────────────────
		ImGui::TextDisabled("Named constants backed by an Int. Scripts read them as");
		ImGui::TextDisabled("horizon.enums.%s.<Entry>; graphs get a dropdown.", st.name.c_str());
		ImGui::Spacing();

		int removeAt = -1;
		if (ImGui::BeginTable("##entries", 3, ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##value",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
			for (int i = 0; i < static_cast<int>(st.enumDef.entries.size()); ++i)
			{
				HE::EnumEntry& e = st.enumDef.entries[i];
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputTextWithHint("##name", "Entry name", &e.name)) st.dirty = true;
				// Duplicate names would make the generated constants ambiguous.
				bool dup = false;
				for (int k = 0; k < static_cast<int>(st.enumDef.entries.size()); ++k)
					if (k != i && st.enumDef.entries[k].name == e.name && !e.name.empty()) dup = true;
				if (dup)
				{
					ImGui::GetWindowDrawList()->AddRect(
						ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
						IM_COL32(220, 70, 70, 255), ImGui::GetStyle().FrameRounding);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Duplicate entry name.");
				}
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragInt("##value", &e.value, 0.25f)) st.dirty = true;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Value");
				ImGui::TableNextColumn();
				if (EditorWidgets::dangerButton("\xc3\x97", ImVec2(-FLT_MIN, 0.0f))) removeAt = i;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this entry");
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (removeAt >= 0)
		{ st.enumDef.entries.erase(st.enumDef.entries.begin() + removeAt); st.dirty = true; }

		if (ImGui::Button("+ Add Entry", ImVec2(160.0f, 0.0f)))
		{
			// Next free value: max + 1 (0 for the first entry).
			int next = 0;
			for (const auto& e : st.enumDef.entries) next = std::max(next, e.value + 1);
			st.enumDef.entries.push_back({ "Entry" + std::to_string(next), next });
			st.dirty = true;
		}
		ImGui::End();
		return;
	}

	// ── Struct / SaveGame Template: one card per field ──────────────────────
	if (st.isTemplate)
	{
		ImGui::TextDisabled("The fields a save of this template carries. save.create()");
		ImGui::TextDisabled("seeds them from the defaults; get/set validate against them.");
		if (ctx.projectManager)
		{
			ProjectData& proj = ctx.projectManager->currentProject();
			const bool isDefault = proj.defaultSaveTemplate == st.relPath;
			if (isDefault)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 210, 140, 255));
				ImGui::TextUnformatted("Project default â save.create() uses this template.");
				ImGui::PopStyleColor();
			}
			else if (ImGui::Button("Set as Project Default", ImVec2(220.0f, 0.0f)))
			{
				proj.defaultSaveTemplate = st.relPath;
				ctx.projectManager->saveProject(proj.path);
			}
		}
	}
	else
	{
		ImGui::TextDisabled("Named, typed fields with defaults. Scripts build one with");
		ImGui::TextDisabled("horizon.structs.%s(); graphs get Make/Break nodes.", st.name.c_str());
	}
	ImGui::Spacing();

	int removeAt = -1;
	for (int i = 0; i < static_cast<int>(st.structDef.fields.size()); ++i)
	{
		HE::StructField& f = st.structDef.fields[i];
		ImGui::PushID(i);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		ImGui::BeginChild("##field", ImVec2(0.0f, 0.0f),
		                  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
		                  ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();

		// Row 1: name + remove.
		ImGui::SetNextItemWidth(std::max(160.0f,
			ImGui::GetContentRegionAvail().x - 100.0f - ImGui::GetStyle().ItemSpacing.x));
		if (ImGui::InputTextWithHint("##name", "Field name", &f.name)) st.dirty = true;
		bool dup = false;
		for (int k = 0; k < static_cast<int>(st.structDef.fields.size()); ++k)
			if (k != i && st.structDef.fields[k].name == f.name && !f.name.empty()) dup = true;
		if (dup)
		{
			ImGui::GetWindowDrawList()->AddRect(
				ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
				IM_COL32(220, 70, 70, 255), ImGui::GetStyle().FrameRounding);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate field name.");
		}
		ImGui::SameLine();
		if (EditorWidgets::dangerButton("Remove", ImVec2(100.0f, 0.0f))) removeAt = i;

		// Row 2: type + (Enum/Struct) definition picker + array toggle.
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Type");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::BeginCombo("##type", fieldTypeLabel(f.type)))
		{
			for (PinType t : kFieldTypes)
				if (ImGui::Selectable(fieldTypeLabel(t), t == f.type) && t != f.type)
				{
					f.type = t;
					f.typeName.clear();
					f.defaultValue = {};
					f.defaultValue.type = t;
					st.dirty = true;
				}
			ImGui::EndCombo();
		}
		if (f.type == PinType::Enum || f.type == PinType::Struct)
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth(std::max(140.0f,
				ImGui::GetContentRegionAvail().x - 110.0f - ImGui::GetStyle().ItemSpacing.x));
			typeNamePicker(f, st.dirty, ctx, st.relPath);
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("Array", &f.isArray))
		{
			// The payload changes shape: a scalar default and a slot list can't
			// both live in one Value, so switching starts the new one clean.
			f.defaultValue = {};
			f.defaultValue.type = f.type;
			f.defaultValue.isArray = f.isArray;
			f.defaultValue.typeName = f.typeName;
			st.dirty = true;
		}

		// Row 3: default value.
		if (f.isArray)
		{
			ImGui::TextDisabled("Default elements");
			if (HcEditorUtil::drawArraySlotsEditor(f.defaultValue.items, f.type, f.typeName))
				st.dirty = true;
		}
		else if (f.type == PinType::Struct)
			ImGui::TextDisabled("Default: the struct's own field defaults");
		else
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("Default");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.5f));
			defaultValueEditor(f, st.dirty);
		}

		ImGui::EndChild();
		ImGui::Spacing();
		ImGui::PopID();
	}
	if (removeAt >= 0)
	{ st.structDef.fields.erase(st.structDef.fields.begin() + removeAt); st.dirty = true; }

	if (ImGui::Button("+ Add Field", ImVec2(160.0f, 0.0f)))
	{
		HE::StructField f;
		f.name = "Field" + std::to_string(st.structDef.fields.size() + 1);
		st.structDef.fields.push_back(std::move(f));
		st.dirty = true;
	}

	ImGui::End();
}
