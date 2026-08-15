#include "InputAssetPanel.h"
#include <algorithm>
#include <cstdint>
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"
#include "EditorToolbar.h"       // shared toolbar strip
#include "EditorWidgets.h"    // danger buttons for deletion     // shared per-tab state map
#include "HcEditorUtil.h"         // HcEditorUtil::listAssets (action picker)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Application/InputAssets.h>
#include <Types/Enums.h>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <SDL3/SDL.h>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// The panel edits a decoded model of the asset's JSON payload and re-serializes
// on Save — so hand-edited/unknown JSON fields are NOT preserved. That matches
// the format's contract: these payloads are editor-owned (see Assets.h).
namespace
{

// ── Decoded models ────────────────────────────────────────────────────────────
// One axis contribution. `source` decides whether the key columns mean
// anything: a mouse row has no keys, it takes the frame's movement instead.
struct AxisRow
{
	std::string positive, negative;
	float       scale = 1.0f;
	AxisSource  source = AxisSource::Key;
};
struct MapEntry
{
	std::string actionPath;            // content-relative InputAction path
	std::vector<std::string> keys;     // Button bindings (SDL scancode names)
	std::vector<AxisRow>     axes;     // Axis bindings (a 2D action's X)
	std::vector<AxisRow>     axesY;    // a 2D action's Y — its own list, so the
	                                   // 1D reader can never half-read one
};
struct PanelState
{
	bool  loaded = false;
	bool  dirty  = false;
	bool  isMapping = false;
	std::string name;
	HE::UUID    assetId;
	// Action payload
	// 0 Button, 1 Axis, 2 Axis 2D — three now, so a bool no longer says it.
	int   valueType = 0;
	// Mapping payload
	std::vector<MapEntry> entries;
};
AssetPanelState<PanelState> s_states;

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
CaptureState s_capture;

void beginCapture(const std::string& assetPath, int entryIndex, int subIndex, CaptureKind kind)
{
	s_capture.assetPath  = assetPath;
	s_capture.entryIndex = entryIndex;
	s_capture.subIndex   = subIndex;
	s_capture.kind       = kind;
	s_capture.primed     = false;
}

bool sniffType(const std::string& path, HE::AssetType type)
{
	return EditorAssetTypeCache::is(path, type);
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
		auto readAxes = [](const nlohmann::json& arr, std::vector<AxisRow>& into)
		{
			for (const auto& a : arr)
				if (a.is_object())
					into.push_back({ a.value("positive", ""), a.value("negative", ""),
					                 a.value("scale", 1.0f),
					                 HE::axisSourceFromName(a.value("source", "Key")) });
		};
		if (e.contains("axes")  && e["axes"].is_array())  readAxes(e["axes"],  me.axes);
		if (e.contains("axesX") && e["axesX"].is_array()) readAxes(e["axesX"], me.axes);
		if (e.contains("axesY") && e["axesY"].is_array()) readAxes(e["axesY"], me.axesY);
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
		auto writeAxes = [](const std::vector<AxisRow>& rows)
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& a : rows)
				arr.push_back({ {"positive", a.positive}, {"negative", a.negative},
				                {"scale", a.scale},
				                {"source", HE::axisSourceName(a.source)} });
			return arr;
		};
		// A Y list is what makes this a 2D entry, so the X list is written under
		// "axesX" then — a loader reading "axes" must not pick up half of one.
		if (!e.axesY.empty())
		{
			if (!e.axes.empty()) je["axesX"] = writeAxes(e.axes);
			je["axesY"] = writeAxes(e.axesY);
		}
		else if (!e.axes.empty())
			je["axes"] = writeAxes(e.axes);
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
		// BeginTooltip is asked as a question here and at every other hand-written
		// tooltip in the editor, because imgui.h spells the contract out: EndTooltip
		// is only to be called when BeginTooltip returned true. The vendored
		// BeginTooltipEx currently ends in a hard `return true`, so calling it bare
		// happens to work — which is precisely why the unguarded shape survives
		// review right up until the frame that return value starts meaning
		// something, and every bare site then ends a window it never began.
		if (ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				ImGui::TextUnformatted(desc);
			}
			ImGui::EndTooltip();
		}
	}
}

// One key binding as TWO table cells — the name field in the current column,
// the Bind button in the next — so every field and every button in a section
// lines up on the same two rails instead of each row finding its own width.
// The field stretches to its column; the old fixed 100 px is what made the
// whole panel read as a cramped heap.
//
// The text box still accepts free-typed SDL scancode names (red outline +
// tooltip when the name doesn't resolve); "Bind" captures the next physical
// key press instead. `capturedName`/`captureCancelled` are this frame's poll
// result (computed once per render(), see the call site) — applied here only
// if this field is the one s_capture is currently pointed at.
void keyBindCells(const char* id, const char* hintText, std::string& value, bool& dirty,
                  const std::string& assetPath, int entryIndex, int subIndex, CaptureKind kind,
                  const std::string& capturedName, bool captureCancelled)
{
	ImGui::PushID(id);

	const bool mine = s_capture.kind == kind && s_capture.assetPath == assetPath &&
	                   s_capture.entryIndex == entryIndex && s_capture.subIndex == subIndex;
	if (mine)
	{
		if (!capturedName.empty()) { value = capturedName; dirty = true; s_capture.kind = CaptureKind::None; }
		else if (captureCancelled)  { s_capture.kind = CaptureKind::None; }
	}

	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##name", hintText, &value)) dirty = true;
	const bool known = value.empty() || SDL_GetScancodeFromName(value.c_str()) != SDL_SCANCODE_UNKNOWN;
	if (!known)
	{
		ImGui::GetWindowDrawList()->AddRect(
			ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(220, 70, 70, 255),
			ImGui::GetStyle().FrameRounding);
		if (ImGui::IsItemHovered())
		{
			// The "%s" is whatever the user typed into the field, so left to itself
			// this tooltip is as wide as their worst paste — a bar stretched across
			// the screen, which is exactly the cheap look a clipped line has. The
			// wrap column is spelled out rather than left at the window edge because
			// a tooltip auto-sizes to its content: "the right edge" is a measurement
			// of the frame that has not been laid out yet.
			// Asked as a question for the reason spelled out at helpMarker above.
			if (ImGui::BeginTooltip())
			{
				{
					EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
					ImGui::Text("\"%s\" isn't a recognized SDL key name — it won't bind to\n"
					            "anything at runtime. Click Bind and press the key instead.", value.c_str());
				}
				ImGui::EndTooltip();
			}
		}
	}

	ImGui::TableNextColumn();
	if (mine)
	{
		// The armed state in the editor's warn amber, full cell width, and the
		// how-to-cancel in the tooltip rather than crammed into the label.
		ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(214, 122, 30, 255));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(230, 140, 40, 255));
		if (ImGui::Button("Press a key\xE2\x80\xA6", ImVec2(-FLT_MIN, 0.0f)))
			s_capture.kind = CaptureKind::None;
		ImGui::PopStyleColor(2);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Press the key you want to bind.\nClick again or press Esc to cancel.");
	}
	else if (ImGui::Button("Bind", ImVec2(-FLT_MIN, 0.0f)))
	{
		beginCapture(assetPath, entryIndex, subIndex, kind);
	}

	ImGui::PopID();
}

// Persist a tab's decoded model back into its asset. The header's Save button AND
// the close/quit prompt's "Save All" both come through here, so the two can never
// drift apart.
bool saveState(PanelState& st, AppContext& ctx)
{
	if (!ctx.contentManager) return false;
	if (st.isMapping)
	{
		InputMappingContextAsset* m = ctx.contentManager->getInputMappingContextMutable(st.assetId);
		if (!m) return false;
		m->json = encodeMapping(st.entries);
		if (!ctx.contentManager->saveAsset(*m)) return false;
	}
	else
	{
		InputActionAsset* a = ctx.contentManager->getInputActionMutable(st.assetId);
		if (!a) return false;
		a->json = st.valueType == 2 ? "{\"valueType\":\"Axis2D\"}"
		        : st.valueType == 1 ? "{\"valueType\":\"Axis\"}"
		                            : "{\"valueType\":\"Button\"}";
		if (!ctx.contentManager->saveAsset(*a)) return false;
	}
	st.dirty = false;
	return true;
}

} // namespace

bool InputAssetPanel::isInputActionAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::InputAction); }
bool InputAssetPanel::isInputMappingAsset(const std::string& path)
{ return sniffType(path, HE::AssetType::InputMappingContext); }
bool InputAssetPanel::isInputAsset(const std::string& path)
{ return isInputActionAsset(path) || isInputMappingAsset(path); }

bool InputAssetPanel::isDirty(const std::string& path) { return s_states.dirty(path); }

bool InputAssetPanel::reloadFromDisk(const std::string& assetPath)
{
	// A collaboration peer's change just landed in the file. Dropping `loaded`
	// makes the next frame re-read it while the rest of the State survives.
	// Dirty is cleared deliberately: while a peer holds the asset's lock this
	// panel is read-only anyway, so anything "unsaved" here is stale.
	auto* st = s_states.find(assetPath);
	if (!st) return false;
	st->loaded = false;
	st->dirty = false;
	return true;
}


void InputAssetPanel::appendDirtyPaths(std::vector<std::string>& out) { s_states.appendDirtyPaths(out); }

bool InputAssetPanel::save(AppContext& ctx, const std::string& path)
{
	PanelState* st = s_states.find(path);
	// A tab this panel never opened has nothing to write — the caller asks every
	// panel about every path, so "not mine" must read as success.
	if (!st || !st->dirty) return true;
	return saveState(*st, ctx);
}

void InputAssetPanel::forget(const std::string& path)
{
	s_states.forget(path);
	// A key-capture aimed at the closing tab would otherwise stay armed and bind
	// the next keypress into a state that no longer exists.
	if (s_capture.assetPath == path) s_capture.kind = CaptureKind::None;
}

void InputAssetPanel::render(AppContext& ctx, const std::string& assetPath,
                             const ImVec2& pos, const ImVec2& size)
{
	PanelState& st = s_states[assetPath];
	if (!st.loaded && ctx.contentManager)
	{
		const std::string rel = ctx.contentManager->toContentRelativePath(assetPath);
		st.assetId   = ctx.contentManager->loadAsset(rel);
		st.isMapping = isInputMappingAsset(assetPath);
		if (const InputActionAsset* a = ctx.contentManager->getInputAction(st.assetId))
		{
			st.name   = a->name;
			st.valueType = HE::inputActionIsAxis2D(a->json) ? 2
			             : HE::inputActionIsAxis(a->json)   ? 1 : 0;
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

	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, st.name.c_str(), T::iconGear, st.dirty);
		bar.group();
		bar.readout(nullptr, st.isMapping ? "Input Mapping Context" : "Input Action",
		            T::kFgDim);
		bar.endGroup();
		if (T::saveButton(bar, true)) saveState(st, ctx);
	}
	ImGui::Spacing();

	if (!st.isMapping)
	{
		// ── Input Action: just the value type ──────────────────────────────
		// The sentence that explains what the two radio buttons below actually
		// choose between, in a tab whose width is whatever the user left it. With
		// no wrap position ImGui runs each line past the right edge and clips it,
		// so the explanation stops at "an Axis" and nothing on screen says a word
		// was cut. Scoped tightly rather than opened once for the whole window on
		// purpose: this branch ends in its own ImGui::End(), and a wrap still
		// pushed at that point would be popped off a window that no longer exists.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled(
				"A Button action fires Pressed and Released events. An Axis fires "
				"once a frame with a Float value; an Axis 2D fires once a frame "
				"with both components at once \xe2\x80\x94 which is what mouse look "
				"is, one movement rather than two.");
		}
		ImGui::Spacing();
		int vt = st.valueType;
		if (ImGui::RadioButton("Button", &vt, 0))  { st.valueType = 0; st.dirty = true; }
		ImGui::SameLine();
		if (ImGui::RadioButton("Axis", &vt, 1))    { st.valueType = 1; st.dirty = true; }
		ImGui::SameLine();
		if (ImGui::RadioButton("Axis 2D", &vt, 2)) { st.valueType = 2; st.dirty = true; }
		{
			// Retyping is not free, and saying so beats letting it be discovered.
			EditorWidgets::WrapText wrap;
			ImGui::Spacing();
			ImGui::TextDisabled(
				"Each type fires its own event, so changing this leaves any node "
				"already placed for the old one silent. Re-add it from the graph's "
				"Input section.");
		}
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
	// is listening for the next key press. s_capture is global across all open
	// tabs; only the tab it targets does anything with the poll result below.
	std::string capturedKeyName;
	bool captureWasCancelled = false;
	if (s_capture.kind != CaptureKind::None && s_capture.assetPath == assetPath)
	{
		int numKeys = 0;
		const bool* keyState = SDL_GetKeyboardState(&numKeys);
		numKeys = std::min(numKeys, static_cast<int>(SDL_SCANCODE_COUNT));
		if (!s_capture.primed)
		{
			// First active frame is snapshot-only, so a key still held down from
			// the click that opened the capture isn't mistaken for a fresh press.
			std::memcpy(s_capture.prevKeys, keyState, sizeof(bool) * numKeys);
			s_capture.primed = true;
		}
		else
		{
			for (int sc = 0; sc < numKeys; ++sc)
			{
				if (keyState[sc] && !s_capture.prevKeys[sc])
				{
					if (sc == SDL_SCANCODE_ESCAPE) captureWasCancelled = true;
					else if (const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc)); n && n[0])
						capturedKeyName = n;
					break;
				}
			}
			std::memcpy(s_capture.prevKeys, keyState, sizeof(bool) * numKeys);
		}
	}
	// Any structural edit below (add/remove key, axis or entry) can shift the
	// indices s_capture is pointed at — just drop an in-flight capture rather
	// than risk it landing on the wrong field.
	auto cancelCaptureForThisAsset = [&]() { if (s_capture.assetPath == assetPath) s_capture.kind = CaptureKind::None; };

	const auto actions = HcEditorUtil::listAssets(ctx.contentManager, HE::AssetType::InputAction);
	int removeEntry = -1;

	// Column rails shared by every card, so a Bind button in entry three sits
	// exactly under the one in entry one. The remove column is square-ish; the
	// bind column fits its longest label.
	const float bindW   = ImGui::CalcTextSize("Press a key\xE2\x80\xA6").x
	                    + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
	const float removeW = ImGui::GetFrameHeight();

	for (int i = 0; i < static_cast<int>(st.entries.size()); ++i)
	{
		MapEntry& e = st.entries[i];
		ImGui::PushID(i);

		// One entry, one card: its own bordered, padded, auto-sized region. The
		// old layout ran every entry into the next with a hairline between two
		// heaps of SameLine'd controls — the card is what turns "a heap" into
		// "a list of things".
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
		ImGui::BeginChild("##entry", ImVec2(0.0f, 0.0f),
		                  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
		                  ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();

		// ── Card header: which action, and the way out ───────────────────────
		const std::string shown = e.actionPath.empty()
			? std::string("Select an action\xE2\x80\xA6")
			: HE::inputActionNameFromPath(e.actionPath);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Action");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(160.0f,
			ImGui::GetContentRegionAvail().x - 100.0f - ImGui::GetStyle().ItemSpacing.x));
		if (ImGui::BeginCombo("##action", shown.c_str()))
		{
			for (const auto& a : actions)
				if (ImGui::Selectable(a.label.c_str(), a.path == e.actionPath))
				{ e.actionPath = a.path; st.dirty = true; }
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (EditorWidgets::dangerButton("Remove", ImVec2(100.0f, 0.0f))) removeEntry = i;

		// ── Keys ─────────────────────────────────────────────────────────────
		int removeKey = -1;
		if (!e.keys.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Keys");
			if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("##bind",   ImGuiTableColumnFlags_WidthFixed, bindW);
				ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, removeW);
				for (int k = 0; k < static_cast<int>(e.keys.size()); ++k)
				{
					ImGui::PushID(1000 + k);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					keyBindCells("key", "Key name", e.keys[k], st.dirty, assetPath, i, k,
					             CaptureKind::Key, capturedKeyName, captureWasCancelled);
					ImGui::TableNextColumn();
					if (EditorWidgets::dangerButton("\xc3\x97", ImVec2(-FLT_MIN, 0.0f))) removeKey = k;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this key");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		if (removeKey >= 0) { e.keys.erase(e.keys.begin() + removeKey); st.dirty = true; cancelCaptureForThisAsset(); }

		// ── Axes ─────────────────────────────────────────────────────────────
		// One table per component list. A 2D action binds X and Y separately, so
		// the same renderer runs twice; `idBase` and `rowBase` keep the two
		// apart for ImGui and for the key-capture identity, which is keyed by
		// (asset, entry, ROW, kind) and would otherwise have one row answering
		// for both lists.
		auto drawAxisTable = [&](std::vector<AxisRow>& rows, const char* tableId,
		                         int idBase, int rowBase) -> int
		{
			int remove = -1;
			if (rows.empty()) return remove;
			if (ImGui::BeginTable(tableId, 7, ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("##source",  ImGuiTableColumnFlags_WidthFixed, 108.0f);
				ImGui::TableSetupColumn("##pos",     ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("##posbind", ImGuiTableColumnFlags_WidthFixed, bindW);
				ImGui::TableSetupColumn("##neg",     ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("##negbind", ImGuiTableColumnFlags_WidthFixed, bindW);
				ImGui::TableSetupColumn("##scale",   ImGuiTableColumnFlags_WidthFixed, 74.0f);
				ImGui::TableSetupColumn("##remove",  ImGuiTableColumnFlags_WidthFixed, removeW);
				for (int k = 0; k < static_cast<int>(rows.size()); ++k)
				{
					AxisRow& a = rows[k];
					ImGui::PushID(idBase + k);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					{
						static const char* kNames[] = { "Keys", "Mouse X", "Mouse Y", "Wheel" };
						int src = static_cast<int>(a.source);
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::Combo("##src", &src, kNames, 4))
						{ a.source = static_cast<AxisSource>(src); st.dirty = true; }
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(
								"Where this row's value comes from.\n"
								"Keys: the two key fields, -1..+1 while held.\n"
								"Mouse / Wheel: how far it moved THIS FRAME — a\n"
								"displacement, so it is not capped at 1 and must not\n"
								"be multiplied by delta time in your logic.");
					}
					// A mouse row has no keys; showing empty key fields for it
					// would invite filling them in for nothing.
					const bool keyed = a.source == AxisSource::Key;
					ImGui::TableNextColumn();
					if (keyed)
						keyBindCells("pos", "+ key", a.positive, st.dirty, assetPath, i, rowBase + k,
						             CaptureKind::AxisPositive, capturedKeyName, captureWasCancelled);
					else
					{
						ImGui::TextDisabled("\xe2\x80\x94");
						ImGui::TableNextColumn();
					}
					ImGui::TableNextColumn();
					if (keyed)
						keyBindCells("neg", "\xe2\x88\x92 key", a.negative, st.dirty, assetPath, i, rowBase + k,
						             CaptureKind::AxisNegative, capturedKeyName, captureWasCancelled);
					else
					{
						ImGui::TextDisabled("\xe2\x80\x94");
						ImGui::TableNextColumn();
					}
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::DragFloat("##scale", &a.scale, 0.05f, 0.0f, 0.0f, "\xc3\x97 %.2f"))
						st.dirty = true;
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Scale — -1 inverts, a mouse row uses it as sensitivity");
					ImGui::TableNextColumn();
					if (EditorWidgets::dangerButton("\xc3\x97", ImVec2(-FLT_MIN, 0.0f))) remove = k;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this axis");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			return remove;
		};

		const bool is2D = !e.axesY.empty();
		int removeAxis = -1, removeAxisY = -1;
		if (!e.axes.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled(is2D ? "Axes \xe2\x80\x94 X" : "Axes");
			ImGui::SameLine();
			helpMarker("Holding the \"+\" key drives the axis toward +1, the \"-\" key "
			           "toward -1; either can stay blank for a one-sided axis. Scale "
			           "multiplies the raw value \xe2\x80\x94 -1 inverts.\n\n"
			           "A Mouse row takes the movement of this frame instead of keys. "
			           "Keys are still capped at \xc2\xb1" "1 together; a mouse row is "
			           "added on top uncapped, because a fast flick has to be able to "
			           "turn further than a held key.");
			removeAxis = drawAxisTable(e.axes, "##axes", 2000, 0);
		}
		if (is2D)
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Axes \xe2\x80\x94 Y");
			removeAxisY = drawAxisTable(e.axesY, "##axesY", 3000, 500);
		}
		if (removeAxis >= 0) { e.axes.erase(e.axes.begin() + removeAxis); st.dirty = true; cancelCaptureForThisAsset(); }
		if (removeAxisY >= 0) { e.axesY.erase(e.axesY.begin() + removeAxisY); st.dirty = true; cancelCaptureForThisAsset(); }

		// ── Card footer: grow the entry ──────────────────────────────────────
		ImGui::Spacing();
		if (ImGui::Button("+ Key", ImVec2(110.0f, 0.0f)))  { e.keys.emplace_back(); st.dirty = true; }
		ImGui::SameLine();
		if (ImGui::Button(is2D ? "+ X Axis" : "+ Axis", ImVec2(110.0f, 0.0f)))
		{ e.axes.emplace_back(); st.dirty = true; }
		ImGui::SameLine();
		// Adding a Y row is what turns the entry into a 2D binding — the same
		// thing the loader keys on, so the UI and the format agree by shape
		// rather than by a flag that could disagree with either.
		if (ImGui::Button("+ Y Axis", ImVec2(110.0f, 0.0f)))
		{ e.axesY.emplace_back(); st.dirty = true; }
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Makes this a 2D binding — for an Axis 2D action");

		ImGui::EndChild();
		ImGui::Spacing();
		ImGui::PopID();
	}
	if (removeEntry >= 0) { st.entries.erase(st.entries.begin() + removeEntry); st.dirty = true; cancelCaptureForThisAsset(); }

	if (ImGui::Button("+ Add Action Entry", ImVec2(220.0f, 0.0f)))
	{ st.entries.emplace_back(); st.dirty = true; }

	ImGui::End();
}
