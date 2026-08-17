#pragma once
#include "Types/Defines.h"
#include "Application/InputMapping.h"   // AxisSource — the names below map to it
#include <string>

class InputMapping;

// Glue between the input ASSETS (InputActionAsset / InputMappingContextAsset
// JSON payloads) and the runtime InputMapping + HorizonCode event dispatch.
// The editor's event catalog and the runtime input pump must agree on the
// exact event-name strings, so both sides go through these helpers.
namespace HE
{
	// HorizonCode event names fired for a logical action.
	HE_API std::string inputEventPressed (const std::string& actionName); // "Input.<name>.Pressed"
	HE_API std::string inputEventReleased(const std::string& actionName); // "Input.<name>.Released"
	HE_API std::string inputEventAxis    (const std::string& actionName); // "Input.<name>.Axis"
	// A TWO-dimensional axis gets its own event name rather than sending a Vec2
	// under ".Axis". Retyping an action would otherwise hand a graph that still
	// has the old one-dimensional handler a Vec2 where it expects a Float, and
	// the coercion would silently produce a zero (or an x nobody asked for). A
	// separate name means the stale handler simply stops firing — visible, and
	// the same thing that already happens when an action is deleted.
	HE_API std::string inputEventAxis2D  (const std::string& actionName); // "Input.<name>.Axis2D"

	// True when an InputActionAsset JSON payload declares "valueType":"Axis".
	// Tolerant: malformed or missing → Button. Deliberately NOT true for an
	// Axis2D action — callers key on this to mean "one float".
	HE_API bool inputActionIsAxis(const std::string& json);
	// True for "valueType":"Axis2D".
	HE_API bool inputActionIsAxis2D(const std::string& json);

	// True when the action declares `"runWhilePaused": true` — it keeps firing
	// its events while the game is paused (time scale 0). Default FALSE, and
	// missing/malformed reads as false, so every action authored before this
	// existed stays silent during a pause: that is the point of a pause. The
	// pause menu's own actions (open/close, navigate, confirm) opt in.
	HE_API bool inputActionRunsWhilePaused(const std::string& json);

	// The InputAction payload for a value type ("Button"/"Axis"/"Axis2D") and
	// the pause flag. One writer, so the editor cannot drop a field the loader
	// expects by hand-assembling the JSON somewhere else.
	HE_API std::string makeInputActionJson(const std::string& valueType, bool runWhilePaused);

	// The logical action name for a content-relative InputAction asset path —
	// the file stem ("Content/Input/IA_Jump.hasset" → "IA_Jump"). Mapping
	// entries reference actions by path; events and bindings key on this name.
	HE_API std::string inputActionNameFromPath(const std::string& path);

	// The name an AxisSource carries in a mapping context's JSON, and back. The
	// editor writes these and the loader reads them, so they live together;
	// anything unrecognised reads as Key, which is what every context written
	// before mouse sources existed means by leaving the field out.
	HE_API std::string axisSourceName(AxisSource s);
	HE_API AxisSource  axisSourceFromName(const std::string& name);

	// Mouse button name in a mapping context's JSON ("left"/"right"/"middle"/
	// "x1"/"x2"), and back. Index is the MouseButton enum; unknown → -1.
	HE_API std::string mouseButtonName(int button);
	HE_API int         mouseButtonFromName(const std::string& name);

	// Human-readable labels for binding UI. The STORED names stay SDL's tables
	// (stable, what the loader parses); these are only what a person sees.
	// Gamepad names are positional in Xbox-layout terms, so the label says both
	// halves: SDL's "a" shows as "A (South)", "dpup" as "D-Pad Up".
	HE_API std::string gamepadButtonDisplayName(SDL_GamepadButton b);
	HE_API std::string mouseButtonDisplayName(int button);

	// Apply one InputMappingContextAsset JSON payload to `mapping`: resolves
	// each entry's action path to its logical name and registers key ("keys"),
	// gamepad button ("gamepadButtons"), mouse button ("mouseButtons") and/or
	// axis ("axes") bindings. Unknown names are skipped. Returns the number of
	// entries that produced a binding.
	HE_API size_t applyInputMappingContext(InputMapping& mapping, const std::string& json);
}
