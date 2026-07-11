#pragma once
#include "Types/Defines.h"
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

	// True when an InputActionAsset JSON payload declares "valueType":"Axis".
	// Tolerant: malformed or missing → Button.
	HE_API bool inputActionIsAxis(const std::string& json);

	// The logical action name for a content-relative InputAction asset path —
	// the file stem ("Content/Input/IA_Jump.hasset" → "IA_Jump"). Mapping
	// entries reference actions by path; events and bindings key on this name.
	HE_API std::string inputActionNameFromPath(const std::string& path);

	// Apply one InputMappingContextAsset JSON payload to `mapping`: resolves
	// each entry's action path to its logical name and registers key ("keys")
	// and/or axis ("axes") bindings by SDL scancode name. Unknown key names
	// are skipped. Returns the number of entries that produced a binding.
	HE_API size_t applyInputMappingContext(InputMapping& mapping, const std::string& json);
}
