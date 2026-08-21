#pragma once
#include <Application/InputMapping.h>   // AxisSource
#include <string>
#include <vector>

// ── The Input Mapping asset as the editor holds it ───────────────────────────
// The decoded model of an Input Mapping Context's JSON payload, plus the rule
// that turns a pressed input into a binding. Deliberately free of ImGui: the
// panel does the drawing and the device polling, this decides WHERE a detected
// press lands — which is the part that has to be right and the part that no
// screenshot of the editor can show.
//
// The panel edits this model and re-serializes on Save, so hand-edited/unknown
// JSON fields are NOT preserved. That matches the format's contract: these
// payloads are editor-owned (see Assets.h).

namespace HE::Ed
{

// One axis contribution. `source` decides whether the key columns mean
// anything: a mouse or stick row has no keys, it takes the device value
// instead. A Key-source row may hold keys, pad buttons, or both — the UI shows
// one pair at a time (uiPadRow), the runtime reads both.
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
	std::string actionPath;                 // content-relative InputAction path
	std::vector<std::string> keys;           // Button bindings (SDL scancode names)
	std::vector<std::string> gamepadButtons; // pad bindings (SDL button names)
	std::vector<std::string> mouseButtons;   // mouse bindings ("left"/"right"/…)
	std::vector<AxisRow>     axes;           // Axis bindings (a 2D action's X)
	std::vector<AxisRow>     axesY;          // a 2D action's Y — its own list, so
	                                         // the 1D reader can never half-read one
	// The referenced action's declared value type, resolved lazily from its
	// asset: -1 unresolved, -2 unresolvable (action missing), else 0 Button /
	// 1 Axis / 2 Axis2D. It steers what the Add Binding menu offers — a
	// Button action gets buttons, an Axis action gets axis sources — and it
	// makes an Axis 2D entry encode as axesX/axesY even while its Y list is
	// still empty (the old shape-based rule silently wrote a 1D "axes" then,
	// and axis2DValue() answered 0,0 at runtime). Reset to -1 on retarget.
	int valueType = -1;
};

void        decodeMapping(const std::string& json, std::vector<MapEntry>& out);
std::string encodeMapping(const std::vector<MapEntry>& entries);

// ── Binding by pressing the thing ────────────────────────────────────────────
// What one poll of the devices found (at most one of the four is set). The
// panel fills this from the live keyboard/mouse/gamepad state; every Bind
// button in the card consumes the field its own slot can take.
struct DetectedBinding
{
	std::string key;             // SDL scancode name
	std::string mouseButton;     // "left"/"right"/… (HE::mouseButtonName)
	std::string gamepadButton;   // SDL button string ("a"/"dpup"/…)
	int         axisSource = -1; // AxisSource value for a moved stick/trigger
	bool any() const
	{
		return !key.empty() || !mouseButton.empty() || !gamepadButton.empty() ||
		       axisSource >= 0;
	}
};

// Which field of an entry a Bind button is armed for. Every binding row in the
// card has one, so a press can be aimed at THAT row instead of only ever being
// appended to the entry as a whole.
//
// EntryAny is the card-level "Auto Detect": it APPENDS a new binding and takes
// whatever device answers first. Every other slot REPLACES the value of the row
// it names, and takes only the flavour of input that row can hold — a pad-button
// row stores a pad button, so a key press must not silently land in it.
enum class BindSlot : unsigned char
{
	None = 0,
	EntryAny,      // card footer: append a new binding, any device
	Key,           // keys[sub]
	PadButton,     // gamepadButtons[sub]
	MouseButton,   // mouseButtons[sub]
	AxisKeyPos,    // axis row: positive key
	AxisKeyNeg,    // axis row: negative key
	AxisPadPos,    // axis row: positive pad button
	AxisPadNeg,    // axis row: negative pad button
	AxisSource,    // axis row: which stick/trigger the row reads
};

// Axis rows are addressed by rowBase + index so the X and Y lists of a 2D entry
// never answer for each other (the capture identity is (asset, entry, row, slot)).
inline constexpr int kAxisRowBaseX = 0;
inline constexpr int kAxisRowBaseY = 500;

// The axis list a rowBase belongs to, and the index inside it.
inline bool axisRowIsY(int sub) { return sub >= kAxisRowBaseY; }
inline int  axisRowIndex(int sub) { return axisRowIsY(sub) ? sub - kAxisRowBaseY : sub; }

// True when this slot can hold that flavour of input at all — what the row's
// Bind button waits for, and the reason a press on the wrong device is simply
// ignored instead of being written somewhere it cannot work.
bool bindSlotAccepts(BindSlot slot, const DetectedBinding& d);

// Write the detected input into the addressed slot of `e`. `sub` is the row
// index (rowBase-encoded for axis slots), ignored by EntryAny. Returns true
// when something was taken — the caller then disarms the capture and marks the
// asset dirty. An out-of-range row, or an input the slot cannot hold, is a
// no-op that returns false.
bool applyDetected(MapEntry& e, BindSlot slot, int sub, const DetectedBinding& d);

} // namespace HE::Ed
