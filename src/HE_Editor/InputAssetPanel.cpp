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
#include <SDL3/SDL.h>
#include <cstring>
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

// ── "Press a key to bind" capture ───────────────────────────────────────────
// At most one key field across all open tabs can be "listening" at a time.
// It's identified by (assetPath, entryIndex, subIndex, kind) rather than a
// pointer into the entry's vectors, since those can reallocate/shift while
// the capture is waiting (multiple frames) for a key press.
enum class CaptureKind { None, Key, AxisPositive, AxisNegative };
struct CaptureState
{
	std::string  assetPath;
	int          entryIndex = -1;
	int          subIndex   = -1;
	CaptureKind  kind       = CaptureKind::None;
	bool         primed     = false;                    // snapshot-only first frame
	bool         prevKeys[SDL_SCANCODE_COUNT] = {};      // last frame's held-key snapshot
};
CaptureState g_capture;

void beginCapture(const std::string& assetPath, int entryIndex, int subIndex, CaptureKind kind)
{
	g_capture.assetPath  = assetPath;
	g_capture.entryIndex = entryIndex;
	g_capture.subIndex   = subIndex;
	g_capture.kind       = kind;
	g_capture.primed     = false;
}

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

// A "(?)" marker that shows an explanatory tooltip on hover.
void helpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// One key-name field: a "<label> [text box] [Bind]" row. The text box still
// accepts free-typed SDL scancode names (and gets a red outline + tooltip if
// the typed name doesn't resolve), but "Bind" lets the user just press the
// physical key instead of having to know its exact name.
//
// `capturedName`/`captureCancelled` are this frame's poll result (computed
// once per render() call, see the call site) — applied here only if this
// field is the one g_capture is currently pointed at.
void keyBindField(const char* label, std::string& value, bool& dirty,
                   const std::string& assetPath, int entryIndex, int subIndex, CaptureKind kind,
                   const std::string& capturedName, bool captureCancelled)
{
	// Scope IDs by `label` too: an axis row calls this twice ("+" and "-")
	// under the same outer PushID, so the Bind/Press-a-key buttons would
	// otherwise collide (both just say "Bind").
	ImGui::PushID(label);

	const bool mine = g_capture.kind == kind && g_capture.assetPath == assetPath &&
	                   g_capture.entryIndex == entryIndex && g_capture.subIndex == subIndex;
	if (mine)
	{
		if (!capturedName.empty()) { value = capturedName; dirty = true; g_capture.kind = CaptureKind::None; }
		else if (captureCancelled)  { g_capture.kind = CaptureKind::None; }
	}

	ImGui::SetNextItemWidth(100.0f);
	if (ImGui::InputText(label, &value)) dirty = true;
	const bool known = value.empty() || SDL_GetScancodeFromName(value.c_str()) != SDL_SCANCODE_UNKNOWN;
	if (!known)
	{
		ImGui::GetWindowDrawList()->AddRect(
			ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(220, 70, 70, 255));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("\"%s\" isn't a recognized SDL key name — it won't bind to\n"
			                   "anything at runtime. Click Bind and press the key instead.", value.c_str());
	}
	ImGui::SameLine();
	if (mine)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(214, 122, 30, 255));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(230, 140, 40, 255));
		if (ImGui::SmallButton("Press a key\xE2\x80\xA6 (click to cancel)")) g_capture.kind = CaptureKind::None;
		ImGui::PopStyleColor(2);
	}
	else if (ImGui::SmallButton("Bind"))
	{
		beginCapture(assetPath, entryIndex, subIndex, kind);
	}

	ImGui::PopID();
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

void InputAssetPanel::forget(const std::string& path)
{
	g_states.erase(path);
	if (g_capture.assetPath == path) g_capture.kind = CaptureKind::None;
}

void InputAssetPanel::render(AppContext& ctx, const std::string& assetPath,
                             const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = g_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		const std::string rel = ctx.contentManager->toContentRelativePath(assetPath);
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
	ImGui::TextDisabled("Binds keys to Input Actions.");
	ImGui::SameLine();
	helpMarker(
		"Click \"Bind\" next to a key field, then press the physical key you want "
		"to use \xe2\x80\x94 it fills in the exact name for you. Press Esc, or click "
		"the button again, to cancel.\n\n"
		"You can still type a name by hand; they're SDL key names (e.g. \"W\", "
		"\"Space\", \"Left Shift\"). A red outline means the typed name isn't "
		"recognized and won't bind to anything at runtime.\n\n"
		"An Axis reads -1..+1 each frame: holding the \"+\" key drives it toward "
		"+1, the \"-\" key toward -1. Either can be left blank for a one-sided "
		"axis (e.g. a trigger). Scale multiplies that raw value \xe2\x80\x94 try "
		"-1 to invert, or a higher value for a faster response.");
	ImGui::Spacing();

	// Poll SDL's live keyboard state once per frame while a field on THIS tab
	// is listening for the next key press. g_capture is global across all open
	// tabs; only the tab it targets does anything with the poll result below.
	std::string capturedKeyName;
	bool captureWasCancelled = false;
	if (g_capture.kind != CaptureKind::None && g_capture.assetPath == assetPath)
	{
		int numKeys = 0;
		const bool* keyState = SDL_GetKeyboardState(&numKeys);
		numKeys = std::min(numKeys, static_cast<int>(SDL_SCANCODE_COUNT));
		if (!g_capture.primed)
		{
			// First active frame is snapshot-only, so a key still held down from
			// the click that opened the capture isn't mistaken for a fresh press.
			std::memcpy(g_capture.prevKeys, keyState, sizeof(bool) * numKeys);
			g_capture.primed = true;
		}
		else
		{
			for (int sc = 0; sc < numKeys; ++sc)
			{
				if (keyState[sc] && !g_capture.prevKeys[sc])
				{
					if (sc == SDL_SCANCODE_ESCAPE) captureWasCancelled = true;
					else if (const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc)); n && n[0])
						capturedKeyName = n;
					break;
				}
			}
			std::memcpy(g_capture.prevKeys, keyState, sizeof(bool) * numKeys);
		}
	}
	// Any structural edit below (add/remove key, axis or entry) can shift the
	// indices g_capture is pointed at — just drop an in-flight capture rather
	// than risk it landing on the wrong field.
	auto cancelCaptureForThisAsset = [&]() { if (g_capture.assetPath == assetPath) g_capture.kind = CaptureKind::None; };

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
			keyBindField("Key", e.keys[k], st.dirty, assetPath, i, k, CaptureKind::Key,
			             capturedKeyName, captureWasCancelled);
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) removeKey = k;
			ImGui::PopID();
		}
		if (removeKey >= 0) { e.keys.erase(e.keys.begin() + removeKey); st.dirty = true; cancelCaptureForThisAsset(); }
		if (ImGui::SmallButton("+ Key")) { e.keys.emplace_back(); st.dirty = true; }

		// Axis bindings
		int removeAxis = -1;
		for (int k = 0; k < static_cast<int>(e.axes.size()); ++k)
		{
			AxisRow& a = e.axes[k];
			ImGui::PushID(2000 + k);
			keyBindField("+", a.positive, st.dirty, assetPath, i, k, CaptureKind::AxisPositive,
			             capturedKeyName, captureWasCancelled);
			ImGui::SameLine();
			keyBindField("-", a.negative, st.dirty, assetPath, i, k, CaptureKind::AxisNegative,
			             capturedKeyName, captureWasCancelled);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(70.0f);
			if (ImGui::DragFloat("Scale", &a.scale, 0.05f)) st.dirty = true;
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) removeAxis = k;
			ImGui::PopID();
		}
		if (removeAxis >= 0) { e.axes.erase(e.axes.begin() + removeAxis); st.dirty = true; cancelCaptureForThisAsset(); }
		ImGui::SameLine();
		if (ImGui::SmallButton("+ Axis")) { e.axes.emplace_back(); st.dirty = true; }

		ImGui::PopID();
	}
	if (removeEntry >= 0) { st.entries.erase(st.entries.begin() + removeEntry); st.dirty = true; cancelCaptureForThisAsset(); }

	ImGui::Separator();
	if (ImGui::Button("+ Add Action Entry")) { st.entries.emplace_back(); st.dirty = true; }

	ImGui::End();
}
