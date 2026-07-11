#include "InputAssetPanel.h"
#include "EditorApplication.h"   // AppContext
#include "HcClassList.h"         // HcEditorUtil::listAssets (action picker)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Application/InputAssets.h>
#include <Types/Enums.h>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// The panel edits a decoded model of the asset's JSON payload and re-serializes
// on Save — so hand-edited/unknown JSON fields are NOT preserved. That matches
// the format's contract: these payloads are editor-owned (see Assets.h).
namespace
{

// ── Decoded models ────────────────────────────────────────────────────────────
struct AxisRow { std::string positive, negative; float scale = 1.0f; };
struct MapEntry
{
	std::string actionPath;            // content-relative InputAction path
	std::vector<std::string> keys;     // Button bindings (SDL scancode names)
	std::vector<AxisRow>     axes;     // Axis bindings
};
struct PanelState
{
	bool  loaded = false;
	bool  dirty  = false;
	bool  isMapping = false;
	std::string name;
	HE::UUID    assetId;
	// Action payload
	bool  isAxis = false;
	// Mapping payload
	std::vector<MapEntry> entries;
};
std::map<std::string, PanelState> g_states;

bool sniffType(const std::string& path, HE::AssetType type)
{
	static std::map<std::string, uint16_t> cache;
	auto it = cache.find(path);
	if (it == cache.end())
	{
		HAsset::Reader r;
		cache[path] = r.open(path) ? r.assetType() : 0;
		it = cache.find(path);
	}
	return it->second == static_cast<uint16_t>(type);
}

void decodeMapping(const std::string& json, std::vector<MapEntry>& out)
{
	out.clear();
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) return;
	for (const auto& e : j["entries"])
	{
		if (!e.is_object()) continue;
		MapEntry me;
		me.actionPath = e.value("action", "");
		if (e.contains("keys") && e["keys"].is_array())
			for (const auto& k : e["keys"])
				if (k.is_string()) me.keys.push_back(k.get<std::string>());
		if (e.contains("axes") && e["axes"].is_array())
			for (const auto& a : e["axes"])
				if (a.is_object())
					me.axes.push_back({ a.value("positive", ""), a.value("negative", ""),
					                    a.value("scale", 1.0f) });
		out.push_back(std::move(me));
	}
}

std::string encodeMapping(const std::vector<MapEntry>& entries)
{
	nlohmann::json j; j["entries"] = nlohmann::json::array();
	for (const auto& e : entries)
	{
		nlohmann::json je; je["action"] = e.actionPath;
		if (!e.keys.empty()) je["keys"] = e.keys;
		if (!e.axes.empty())
		{
			je["axes"] = nlohmann::json::array();
			for (const auto& a : e.axes)
				je["axes"].push_back({ {"positive", a.positive}, {"negative", a.negative},
				                       {"scale", a.scale} });
		}
		j["entries"].push_back(std::move(je));
	}
	return j.dump();
}

} // namespace

bool InputAssetPanel::isInputActionAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::InputAction); }
bool InputAssetPanel::isInputMappingAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::InputMappingContext); }
bool InputAssetPanel::isInputAsset(const std::string& path)
{ return isInputActionAsset(path) || isInputMappingAsset(path); }

bool InputAssetPanel::isDirty(const std::string& path)
{
	auto it = g_states.find(path);
	return it != g_states.end() && it->second.dirty;
}

void InputAssetPanel::forget(const std::string& path) { g_states.erase(path); }

void InputAssetPanel::render(AppContext& ctx, const std::string& assetPath,
                             const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = g_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		std::error_code ec;
		const std::string rel = std::filesystem::relative(
			assetPath, ctx.contentManager->contentRoot(), ec).generic_string();
		st.assetId   = ctx.contentManager->loadAsset(rel);
		st.isMapping = isInputMappingAsset(assetPath);
		if (const InputActionAsset* a = ctx.contentManager->getInputAction(st.assetId))
		{
			st.name   = a->name;
			st.isAxis = HE::inputActionIsAxis(a->json);
		}
		else if (const InputMappingContextAsset* m = ctx.contentManager->getInputMappingContext(st.assetId))
		{
			st.name = m->name;
			decodeMapping(m->json, st.entries);
		}
		st.loaded = true;
	}

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin(("##inputasset_" + assetPath).c_str(), nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	ImGui::AlignTextToFramePadding();
	ImGui::Text("%s", st.name.c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s%s", st.isMapping ? "Input Mapping Context" : "Input Action",
	                    st.dirty ? "  (unsaved)" : "");
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
	if (ImGui::Button("Save", ImVec2(56.0f, 0.0f)) && ctx.contentManager)
	{
		if (st.isMapping)
		{
			if (InputMappingContextAsset* m = ctx.contentManager->getInputMappingContextMutable(st.assetId))
			{
				m->json = encodeMapping(st.entries);
				if (ctx.contentManager->saveAsset(*m)) st.dirty = false;
			}
		}
		else if (InputActionAsset* a = ctx.contentManager->getInputActionMutable(st.assetId))
		{
			a->json = st.isAxis ? "{\"valueType\":\"Axis\"}" : "{\"valueType\":\"Button\"}";
			if (ctx.contentManager->saveAsset(*a)) st.dirty = false;
		}
	}
	ImGui::Separator();

	if (!st.isMapping)
	{
		// ── Input Action: just the value type ──────────────────────────────
		ImGui::TextDisabled("A Button action fires Pressed/Released events; an Axis");
		ImGui::TextDisabled("action fires a per-frame Axis event with a Float value.");
		ImGui::Spacing();
		int vt = st.isAxis ? 1 : 0;
		if (ImGui::RadioButton("Button", &vt, 0)) { st.isAxis = false; st.dirty = true; }
		ImGui::SameLine();
		if (ImGui::RadioButton("Axis", &vt, 1))   { st.isAxis = true;  st.dirty = true; }
		ImGui::End();
		return;
	}

	// ── Input Mapping Context: one block per action entry ──────────────────
	ImGui::TextDisabled("Binds keys to Input Actions. Key names are SDL scancode names");
	ImGui::TextDisabled("(\"W\", \"Space\", \"Left Shift\", ...).");
	ImGui::Spacing();

	const auto actions = HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::InputAction);
	int removeEntry = -1;
	for (int i = 0; i < static_cast<int>(st.entries.size()); ++i)
	{
		MapEntry& e = st.entries[i];
		ImGui::PushID(i);
		ImGui::Separator();

		// Action picker (content-relative path; stem = logical action name).
		const std::string shown = e.actionPath.empty()
			? std::string("<select action>")
			: HE::inputActionNameFromPath(e.actionPath);
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::BeginCombo("Action", shown.c_str()))
		{
			for (const auto& a : actions)
				if (ImGui::Selectable(a.label.c_str(), a.path == e.actionPath))
				{ e.actionPath = a.path; st.dirty = true; }
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove Entry")) removeEntry = i;

		// Button bindings
		int removeKey = -1;
		for (int k = 0; k < static_cast<int>(e.keys.size()); ++k)
		{
			ImGui::PushID(1000 + k);
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::InputText("Key", &e.keys[k])) st.dirty = true;
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) removeKey = k;
			ImGui::PopID();
		}
		if (removeKey >= 0) { e.keys.erase(e.keys.begin() + removeKey); st.dirty = true; }
		if (ImGui::SmallButton("+ Key")) { e.keys.emplace_back(); st.dirty = true; }

		// Axis bindings
		int removeAxis = -1;
		for (int k = 0; k < static_cast<int>(e.axes.size()); ++k)
		{
			AxisRow& a = e.axes[k];
			ImGui::PushID(2000 + k);
			ImGui::SetNextItemWidth(100.0f);
			if (ImGui::InputText("+", &a.positive)) st.dirty = true;
			ImGui::SameLine();
			ImGui::SetNextItemWidth(100.0f);
			if (ImGui::InputText("-", &a.negative)) st.dirty = true;
			ImGui::SameLine();
			ImGui::SetNextItemWidth(70.0f);
			if (ImGui::DragFloat("Scale", &a.scale, 0.05f)) st.dirty = true;
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) removeAxis = k;
			ImGui::PopID();
		}
		if (removeAxis >= 0) { e.axes.erase(e.axes.begin() + removeAxis); st.dirty = true; }
		ImGui::SameLine();
		if (ImGui::SmallButton("+ Axis")) { e.axes.emplace_back(); st.dirty = true; }

		ImGui::PopID();
	}
	if (removeEntry >= 0) { st.entries.erase(st.entries.begin() + removeEntry); st.dirty = true; }

	ImGui::Separator();
	if (ImGui::Button("+ Add Action Entry")) { st.entries.emplace_back(); st.dirty = true; }

	ImGui::End();
}
