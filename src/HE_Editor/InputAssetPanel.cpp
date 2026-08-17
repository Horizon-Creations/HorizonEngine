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
#include <cctype>
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
// anything: a mouse or stick row has no keys, it takes the device value
// instead. A Key-source row may hold keys, pad buttons, or both — the UI
// edits one kind per row (the source combo shows "Pad Buttons" for a row
// whose pair is buttons), but the format allows both side by side.
struct AxisRow
{
	std::string positive, negative;             // SDL scancode names
	std::string positiveButton, negativeButton; // SDL gamepad button names
	float       scale = 1.0f;
	AxisSource  source = AxisSource::Key;
	// UI-only, not serialized: whether the source combo shows this Key-source
	// row as "Pad Buttons". Needed because a row with both pairs still empty
	// is otherwise indistinguishable from a plain key row.
	bool        uiPadRow = false;
};
struct MapEntry
{
	std::string actionPath;            // content-relative InputAction path
	std::vector<std::string> keys;     // Button bindings (SDL scancode names)
	std::vector<std::string> gamepadButtons; // pad bindings (SDL button names)
	std::vector<std::string> mouseButtons;   // mouse bindings ("left"/"right"/…)
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
// AnyBinding is the "Auto Detect" capture: it listens on ALL devices at once —
// keyboard, mouse buttons AND gamepad buttons — and appends whatever is
// pressed first to the entry's matching binding list.
enum class CaptureKind { None, Key, AxisPositive, AxisNegative, AnyBinding };
struct CaptureState
{
	std::string  assetPath;
	int          entryIndex = -1;
	int          subIndex   = -1;
	CaptureKind  kind       = CaptureKind::None;
	bool         primed     = false;                    // snapshot-only first frame
	bool         prevKeys[SDL_SCANCODE_COUNT] = {};      // last frame's held-key snapshot
	uint32_t     prevMouse  = 0;                         // …held-mouse-button mask
	bool         prevPad[SDL_GAMEPAD_BUTTON_COUNT] = {}; // …held-pad-button snapshot
};
CaptureState s_capture;

// Search text of the "+ Add Binding" popup. One popup is open at a time
// (ImGui closes the previous), so one filter string serves all tabs.
std::string s_addFilter;

// What one frame of an AnyBinding capture found (at most one of the three).
struct DetectedBinding
{
	std::string key;           // SDL scancode name
	std::string mouseButton;   // "left"/"right"/… (HE::mouseButtonName)
	std::string gamepadButton; // SDL button string ("a"/"dpup"/…)
	bool any() const { return !key.empty() || !mouseButton.empty() || !gamepadButton.empty(); }
};

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
		if (e.contains("gamepadButtons") && e["gamepadButtons"].is_array())
			for (const auto& gb : e["gamepadButtons"])
				if (gb.is_string()) me.gamepadButtons.push_back(gb.get<std::string>());
		if (e.contains("mouseButtons") && e["mouseButtons"].is_array())
			for (const auto& mb : e["mouseButtons"])
				if (mb.is_string()) me.mouseButtons.push_back(mb.get<std::string>());
		auto readAxes = [](const nlohmann::json& arr, std::vector<AxisRow>& into)
		{
			for (const auto& a : arr)
			{
				if (!a.is_object()) continue;
				AxisRow r;
				r.positive       = a.value("positive", "");
				r.negative       = a.value("negative", "");
				r.positiveButton = a.value("positiveButton", "");
				r.negativeButton = a.value("negativeButton", "");
				r.scale          = a.value("scale", 1.0f);
				r.source         = HE::axisSourceFromName(a.value("source", "Key"));
				r.uiPadRow       = !r.positiveButton.empty() || !r.negativeButton.empty();
				into.push_back(std::move(r));
			}
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
		if (!e.gamepadButtons.empty()) je["gamepadButtons"] = e.gamepadButtons;
		if (!e.mouseButtons.empty())   je["mouseButtons"]   = e.mouseButtons;
		auto writeAxes = [](const std::vector<AxisRow>& rows)
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& a : rows)
			{
				nlohmann::json ja = { {"positive", a.positive}, {"negative", a.negative},
				                      {"scale", a.scale},
				                      {"source", HE::axisSourceName(a.source)} };
				// Written only when set — old contexts round-trip byte-stable.
				if (!a.positiveButton.empty()) ja["positiveButton"] = a.positiveButton;
				if (!a.negativeButton.empty()) ja["negativeButton"] = a.negativeButton;
				arr.push_back(std::move(ja));
			}
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

// One gamepad button as a dropdown cell. Unlike keys the button set is small
// and enumerable, so a picker beats capture-and-type — and it needs no pad
// plugged in to author with. Names are SDL's mapping-string table ("a", "b",
// "leftshoulder", "dpup", …): Xbox-layout positions, so "a" is the south
// button — Cross on a PlayStation pad. Returns true if the value changed.
bool padButtonCombo(const char* id, std::string& value, const char* noneLabel)
{
	bool changed = false;
	ImGui::SetNextItemWidth(-FLT_MIN);
	// The value STORES SDL's stable name ("a", "dpup"); the person SEES the
	// display name ("A (South)", "D-Pad Up").
	const std::string shownName = value.empty()
		? std::string()
		: HE::gamepadButtonDisplayName(SDL_GetGamepadButtonFromString(value.c_str()));
	const char* shown = value.empty() ? noneLabel : shownName.c_str();
	if (ImGui::BeginCombo(id, shown))
	{
		if (ImGui::Selectable(noneLabel, value.empty()))
		{
			if (!value.empty()) { value.clear(); changed = true; }
		}
		for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
		{
			const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
			if (!n || !n[0]) continue;
			const std::string label =
				HE::gamepadButtonDisplayName(static_cast<SDL_GamepadButton>(b));
			if (ImGui::Selectable(label.c_str(), value == n))
			{
				if (value != n) { value = n; changed = true; }
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		ImGui::SetTooltip("Positions are named in Xbox-layout terms:\n"
		                  "\"A (South)\" is Cross on a PlayStation pad.");
	return changed;
}

// One mouse button as a dropdown cell — same shape as padButtonCombo: the
// value stores the short JSON name ("left"), the person sees the display name.
bool mouseButtonComboCell(const char* id, std::string& value, const char* noneLabel)
{
	bool changed = false;
	ImGui::SetNextItemWidth(-FLT_MIN);
	const std::string shownName = value.empty()
		? std::string()
		: HE::mouseButtonDisplayName(HE::mouseButtonFromName(value));
	const char* shown = value.empty() ? noneLabel : shownName.c_str();
	if (ImGui::BeginCombo(id, shown))
	{
		if (ImGui::Selectable(noneLabel, value.empty()))
		{
			if (!value.empty()) { value.clear(); changed = true; }
		}
		for (int b = 0; b < kMouseButtonCount; ++b)
		{
			const std::string stored = HE::mouseButtonName(b);
			const std::string label  = HE::mouseButtonDisplayName(b);
			if (ImGui::Selectable(label.c_str(), value == stored))
			{
				if (value != stored) { value = stored; changed = true; }
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

// Case-insensitive substring match for the Add Binding search field.
bool matchesFilter(const std::string& label, const std::string& filter)
{
	if (filter.empty()) return true;
	auto lower = [](std::string s){
		std::transform(s.begin(), s.end(), s.begin(),
		               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
		return s;
	};
	return lower(label).find(lower(filter)) != std::string::npos;
}

// Every named keyboard key, built once. ~240 entries — small enough to filter
// per frame, and the scancode ORDER groups related keys (letters, digits,
// function row) better than alphabetic sorting would.
const std::vector<std::string>& allKeyNames()
{
	static const std::vector<std::string> names = []{
		std::vector<std::string> v;
		for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc)
			if (const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc)); n && n[0])
				v.emplace_back(n);
		return v;
	}();
	return names;
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
	ImGui::TextDisabled("Binds keyboard, mouse and gamepad inputs to Input Actions.");
	ImGui::SameLine();
	helpMarker(
		"\"+ Add Binding\" opens a searchable list of every bindable input \xe2\x80\x94 "
		"keyboard keys, mouse buttons and gamepad buttons \xe2\x80\x94 no hardware "
		"needs to be plugged in. \"Auto Detect\" listens on all three devices at "
		"once and binds whatever you press first (Esc cancels).\n\n"
		"Key fields also accept typed SDL key names (e.g. \"W\", \"Space\", "
		"\"Left Shift\") and \"Bind\" captures the next key press. A red outline "
		"means a typed name isn't recognized and won't bind at runtime.\n\n"
		"Gamepad positions are named in Xbox-layout terms: \"A (South)\" is Cross "
		"on a PlayStation pad. Mouse-button bindings fire in play-in-editor only "
		"while play mode holds the mouse, so clicking editor panels stays safe.\n\n"
		"An Axis reads -1..+1 each frame: holding the \"+\" key drives it toward "
		"+1, the \"-\" key toward -1. Either can be left blank for a one-sided "
		"axis. Stick rows read the deadzone-filtered deflection, trigger rows "
		"0..1. Scale multiplies that raw value \xe2\x80\x94 try -1 to invert "
		"(stick Y is positive downward!), or a higher value for a faster response.");
	ImGui::Spacing();

	// Poll the live device state once per frame while a field on THIS tab is
	// listening. s_capture is global across all open tabs; only the tab it
	// targets does anything with the poll result below. A Key-field capture
	// polls the keyboard alone; an AnyBinding ("Auto Detect") capture listens
	// on mouse buttons and gamepad buttons as well.
	std::string capturedKeyName;
	bool captureWasCancelled = false;
	DetectedBinding detected;
	if (s_capture.kind != CaptureKind::None && s_capture.assetPath == assetPath)
	{
		const bool any = s_capture.kind == CaptureKind::AnyBinding;
		int numKeys = 0;
		const bool* keyState = SDL_GetKeyboardState(&numKeys);
		numKeys = std::min(numKeys, static_cast<int>(SDL_SCANCODE_COUNT));

		// Mouse: SDL's mask orders left/MIDDLE/right — translate to our
		// left/right/middle indices (same trap as Input::ProcessMouseEvent).
		uint32_t mouseMask = 0;
		if (any)
		{
			const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
			if (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   mouseMask |= 1u << 0;
			if (mb & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  mouseMask |= 1u << 1;
			if (mb & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) mouseMask |= 1u << 2;
			if (mb & SDL_BUTTON_MASK(SDL_BUTTON_X1))     mouseMask |= 1u << 3;
			if (mb & SDL_BUTTON_MASK(SDL_BUTTON_X2))     mouseMask |= 1u << 4;
		}
		const GamepadFrame* pad = any && ctx.appInput ? &ctx.appInput->gamepad() : nullptr;

		if (!s_capture.primed)
		{
			// First active frame is snapshot-only, so a key or button still
			// held from the click that opened the capture isn't mistaken for
			// a fresh press — that click itself included.
			std::memcpy(s_capture.prevKeys, keyState, sizeof(bool) * numKeys);
			s_capture.prevMouse = mouseMask;
			if (pad) std::memcpy(s_capture.prevPad, pad->buttons, sizeof(s_capture.prevPad));
			else     std::memset(s_capture.prevPad, 0, sizeof(s_capture.prevPad));
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
					{ capturedKeyName = n; detected.key = n; }
					break;
				}
			}
			if (any && !detected.any() && !captureWasCancelled)
			{
				for (int b = 0; b < 5; ++b)
					if ((mouseMask & (1u << b)) && !(s_capture.prevMouse & (1u << b)))
					{ detected.mouseButton = HE::mouseButtonName(b); break; }
			}
			if (pad && !detected.any() && !captureWasCancelled)
			{
				for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
					if (pad->buttons[b] && !s_capture.prevPad[b])
					{
						if (const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b)); n && n[0])
							detected.gamepadButton = n;
						break;
					}
			}
			std::memcpy(s_capture.prevKeys, keyState, sizeof(bool) * numKeys);
			s_capture.prevMouse = mouseMask;
			if (pad) std::memcpy(s_capture.prevPad, pad->buttons, sizeof(s_capture.prevPad));
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

		// ── Gamepad buttons ──────────────────────────────────────────────────
		int removePadButton = -1;
		if (!e.gamepadButtons.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Gamepad Buttons");
			if (ImGui::BeginTable("##padbuttons", 3, ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch);
				// The bind column stays, empty, so the picker sits on the same
				// rail as the key fields above it.
				ImGui::TableSetupColumn("##bind",   ImGuiTableColumnFlags_WidthFixed, bindW);
				ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, removeW);
				for (int k = 0; k < static_cast<int>(e.gamepadButtons.size()); ++k)
				{
					ImGui::PushID(1500 + k);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (padButtonCombo("##btn", e.gamepadButtons[k], "Select a button\xE2\x80\xA6"))
						st.dirty = true;
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					if (EditorWidgets::dangerButton("\xc3\x97", ImVec2(-FLT_MIN, 0.0f))) removePadButton = k;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this button");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		if (removePadButton >= 0)
		{ e.gamepadButtons.erase(e.gamepadButtons.begin() + removePadButton); st.dirty = true; }

		// ── Mouse buttons ────────────────────────────────────────────────────
		int removeMouseButton = -1;
		if (!e.mouseButtons.empty())
		{
			ImGui::Spacing();
			ImGui::TextDisabled("Mouse Buttons");
			ImGui::SameLine();
			helpMarker("A mouse-button binding obeys the same ownership rule as "
			           "mouse look: in the game it always fires, in play-in-editor "
			           "only while play mode holds the mouse \xe2\x80\x94 so "
			           "clicking editor panels never triggers game actions.");
			if (ImGui::BeginTable("##mousebuttons", 3, ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("##bind",   ImGuiTableColumnFlags_WidthFixed, bindW);
				ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, removeW);
				for (int k = 0; k < static_cast<int>(e.mouseButtons.size()); ++k)
				{
					ImGui::PushID(1700 + k);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (mouseButtonComboCell("##mbtn", e.mouseButtons[k], "Select a button\xE2\x80\xA6"))
						st.dirty = true;
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					if (EditorWidgets::dangerButton("\xc3\x97", ImVec2(-FLT_MIN, 0.0f))) removeMouseButton = k;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this button");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		if (removeMouseButton >= 0)
		{ e.mouseButtons.erase(e.mouseButtons.begin() + removeMouseButton); st.dirty = true; }

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
						// UI list: index 0/1 are the two faces of the Key
						// source (key pair vs pad-button pair); from index 2
						// on, combo index = AxisSource value + 1.
						static const char* kNames[] = { "Keys", "Pad Buttons",
							"Mouse X", "Mouse Y", "Wheel",
							"Left Stick X", "Left Stick Y",
							"Right Stick X", "Right Stick Y",
							"Left Trigger", "Right Trigger" };
						int src = a.source == AxisSource::Key
							? (a.uiPadRow ? 1 : 0)
							: static_cast<int>(a.source) + 1;
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (ImGui::Combo("##src", &src, kNames, IM_ARRAYSIZE(kNames)))
						{
							if (src <= 1)
							{
								a.source   = AxisSource::Key;
								a.uiPadRow = src == 1;
								// The runtime reads BOTH pairs of a Key row, so
								// the pair this row no longer shows must be
								// cleared or it would keep binding invisibly.
								if (a.uiPadRow) { a.positive.clear(); a.negative.clear(); }
								else { a.positiveButton.clear(); a.negativeButton.clear(); }
							}
							else
							{
								a.source   = static_cast<AxisSource>(src - 1);
								a.uiPadRow = false;
							}
							st.dirty = true;
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(
								"Where this row's value comes from.\n"
								"Keys / Pad Buttons: the two fields, -1..+1 while held.\n"
								"Sticks: the deadzone-filtered deflection, -1..+1;\n"
								"triggers read 0..1 (a one-sided axis). Held states,\n"
								"capped at \xc2\xb1" "1 together with keys.\n"
								"Mouse / Wheel: how far it moved THIS FRAME — a\n"
								"displacement, so it is not capped at 1 and must not\n"
								"be multiplied by delta time in your logic.\n"
								"Stick Y is positive DOWNWARD (SDL convention) —\n"
								"use scale -1 for an up-positive axis.");
					}
					// A mouse or stick row has no pair fields; showing empty
					// ones for it would invite filling them in for nothing.
					const bool keyed  = a.source == AxisSource::Key && !a.uiPadRow;
					const bool padded = a.source == AxisSource::Key && a.uiPadRow;
					ImGui::TableNextColumn();
					if (keyed)
						keyBindCells("pos", "+ key", a.positive, st.dirty, assetPath, i, rowBase + k,
						             CaptureKind::AxisPositive, capturedKeyName, captureWasCancelled);
					else if (padded)
					{
						if (padButtonCombo("##posbtn", a.positiveButton, "+ button\xE2\x80\xA6"))
							st.dirty = true;
						ImGui::TableNextColumn();
					}
					else
					{
						ImGui::TextDisabled("\xe2\x80\x94");
						ImGui::TableNextColumn();
					}
					ImGui::TableNextColumn();
					if (keyed)
						keyBindCells("neg", "\xe2\x88\x92 key", a.negative, st.dirty, assetPath, i, rowBase + k,
						             CaptureKind::AxisNegative, capturedKeyName, captureWasCancelled);
					else if (padded)
					{
						if (padButtonCombo("##negbtn", a.negativeButton, "\xe2\x88\x92 button\xE2\x80\xA6"))
							st.dirty = true;
						ImGui::TableNextColumn();
					}
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
		// Apply this frame's Auto Detect result to THIS entry before drawing
		// the footer, so the pressed button appears in its list immediately.
		if (s_capture.kind == CaptureKind::AnyBinding &&
		    s_capture.assetPath == assetPath && s_capture.entryIndex == i)
		{
			if (detected.any())
			{
				if      (!detected.key.empty())           e.keys.push_back(detected.key);
				else if (!detected.mouseButton.empty())   e.mouseButtons.push_back(detected.mouseButton);
				else if (!detected.gamepadButton.empty()) e.gamepadButtons.push_back(detected.gamepadButton);
				st.dirty = true;
				s_capture.kind = CaptureKind::None;
			}
			else if (captureWasCancelled)
				s_capture.kind = CaptureKind::None;
		}
		const bool detecting = s_capture.kind == CaptureKind::AnyBinding &&
		                       s_capture.assetPath == assetPath && s_capture.entryIndex == i;

		ImGui::Spacing();
		// One entry point for every device instead of a button per device: a
		// searchable list (below) or a press on the actual hardware (right).
		if (ImGui::Button("+ Add Binding", ImVec2(130.0f, 0.0f)))
		{
			s_addFilter.clear();
			ImGui::OpenPopup("##addbinding");
		}
		ImGui::SameLine();
		if (detecting)
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(214, 122, 30, 255));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(230, 140, 40, 255));
			if (ImGui::Button("Press any input\xE2\x80\xA6", ImVec2(150.0f, 0.0f)))
				s_capture.kind = CaptureKind::None;
			ImGui::PopStyleColor(2);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Press a key, click a mouse button or press a\n"
				                  "gamepad button to bind it. Esc or click to cancel.");
		}
		else if (ImGui::Button("Auto Detect", ImVec2(150.0f, 0.0f)))
		{
			beginCapture(assetPath, i, 0, CaptureKind::AnyBinding);
		}
		if (!detecting && ImGui::IsItemHovered())
			ImGui::SetTooltip("Listens on keyboard, mouse AND gamepad at once\n"
			                  "and adds whatever you press first.");
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

		// ── Add Binding popup: one searchable list across all devices ───────
		if (ImGui::BeginPopup("##addbinding"))
		{
			ImGui::SetNextItemWidth(260.0f);
			// Keyboard focus lands in the search field the frame the popup
			// opens, so typing filters immediately.
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##search", "Search bindings\xE2\x80\xA6", &s_addFilter);

			bool chose = false;
			ImGui::BeginChild("##addlist", ImVec2(260.0f, 320.0f));
			ImGui::SeparatorText("Gamepad");
			for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
			{
				const char* stored = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
				if (!stored || !stored[0]) continue;
				const std::string label =
					HE::gamepadButtonDisplayName(static_cast<SDL_GamepadButton>(b));
				if (!matchesFilter(label, s_addFilter)) continue;
				if (ImGui::Selectable((label + "##gp").c_str()))
				{ e.gamepadButtons.emplace_back(stored); st.dirty = true; chose = true; }
			}
			ImGui::SeparatorText("Mouse");
			for (int b = 0; b < kMouseButtonCount; ++b)
			{
				const std::string label = HE::mouseButtonDisplayName(b);
				if (!matchesFilter(label, s_addFilter)) continue;
				if (ImGui::Selectable((label + "##ms").c_str()))
				{ e.mouseButtons.push_back(HE::mouseButtonName(b)); st.dirty = true; chose = true; }
			}
			ImGui::SeparatorText("Keyboard");
			for (const std::string& keyName : allKeyNames())
			{
				if (!matchesFilter(keyName, s_addFilter)) continue;
				if (ImGui::Selectable((keyName + "##kb").c_str()))
				{ e.keys.push_back(keyName); st.dirty = true; chose = true; }
			}
			ImGui::EndChild();
			if (chose) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::EndChild();
		ImGui::Spacing();
		ImGui::PopID();
	}
	if (removeEntry >= 0) { st.entries.erase(st.entries.begin() + removeEntry); st.dirty = true; cancelCaptureForThisAsset(); }

	if (ImGui::Button("+ Add Action Entry", ImVec2(220.0f, 0.0f)))
	{ st.entries.emplace_back(); st.dirty = true; }

	ImGui::End();
}
