#include "Application/InputAssets.h"
#include "Application/InputMapping.h"
#include "Diagnostics/Log.h"
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <filesystem>
#include <vector>

namespace HE
{

std::string inputEventPressed (const std::string& a) { return "Input." + a + ".Pressed"; }
std::string inputEventReleased(const std::string& a) { return "Input." + a + ".Released"; }
std::string inputEventAxis    (const std::string& a) { return "Input." + a + ".Axis"; }
std::string inputEventAxis2D  (const std::string& a) { return "Input." + a + ".Axis2D"; }

namespace
{
std::string valueTypeOf(const std::string& json)
{
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object()) return "Button";
	return j.value("valueType", "Button");
}
}

bool inputActionIsAxis  (const std::string& json) { return valueTypeOf(json) == "Axis"; }
bool inputActionIsAxis2D(const std::string& json) { return valueTypeOf(json) == "Axis2D"; }

std::string inputActionNameFromPath(const std::string& path)
{
	return std::filesystem::path(path).stem().string();
}

std::string axisSourceName(AxisSource s)
{
	switch (s)
	{
		case AxisSource::MouseX:              return "MouseX";
		case AxisSource::MouseY:              return "MouseY";
		case AxisSource::MouseWheel:          return "MouseWheel";
		case AxisSource::GamepadLeftX:        return "GamepadLeftX";
		case AxisSource::GamepadLeftY:        return "GamepadLeftY";
		case AxisSource::GamepadRightX:       return "GamepadRightX";
		case AxisSource::GamepadRightY:       return "GamepadRightY";
		case AxisSource::GamepadLeftTrigger:  return "GamepadLeftTrigger";
		case AxisSource::GamepadRightTrigger: return "GamepadRightTrigger";
		case AxisSource::Key:                 break;
	}
	return "Key";
}

AxisSource axisSourceFromName(const std::string& name)
{
	if (name == "MouseX")              return AxisSource::MouseX;
	if (name == "MouseY")              return AxisSource::MouseY;
	if (name == "MouseWheel")          return AxisSource::MouseWheel;
	if (name == "GamepadLeftX")        return AxisSource::GamepadLeftX;
	if (name == "GamepadLeftY")        return AxisSource::GamepadLeftY;
	if (name == "GamepadRightX")       return AxisSource::GamepadRightX;
	if (name == "GamepadRightY")       return AxisSource::GamepadRightY;
	if (name == "GamepadLeftTrigger")  return AxisSource::GamepadLeftTrigger;
	if (name == "GamepadRightTrigger") return AxisSource::GamepadRightTrigger;
	return AxisSource::Key;   // unknown reads as the default, never as an error
}

size_t applyInputMappingContext(InputMapping& mapping, const std::string& json)
{
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array())
	{
		// Returning 0 here means "no controls at all" — worth an error, since the
		// symptom is a game that ignores every key.
		HE_LOG_ERROR(Input, "Input mapping context is unusable: %s",
		             j.is_discarded() ? "the JSON is malformed"
		                              : "no 'entries' array");
		return 0;
	}

	size_t bound = 0;
	for (const auto& e : j["entries"])
	{
		if (!e.is_object()) continue;
		const std::string action = e.value("action", "");
		if (action.empty()) continue;
		const std::string name = inputActionNameFromPath(action);

		// Keys and gamepad buttons are SEPARATE arrays in SEPARATE name spaces:
		// "A" in "keys" is the keyboard key (SDL_GetScancodeFromName); "a" in
		// "gamepadButtons" is the pad's SOUTH button — pad names come from
		// SDL's mapping-string table ("a", "b", "leftshoulder", "dpup", …,
		// SDL_GetGamepadButtonFromString), Xbox-layout positions by convention.
		if ((e.contains("keys") && e["keys"].is_array()) ||
		    (e.contains("gamepadButtons") && e["gamepadButtons"].is_array()))
		{
			std::vector<ActionBinding> binds;
			if (e.contains("keys") && e["keys"].is_array())
				for (const auto& k : e["keys"])
				{
					if (!k.is_string()) continue;
					const SDL_Scancode sc = SDL_GetScancodeFromName(k.get<std::string>().c_str());
					if (sc != SDL_SCANCODE_UNKNOWN) binds.push_back({ sc });
				}
			if (e.contains("gamepadButtons") && e["gamepadButtons"].is_array())
				for (const auto& gb : e["gamepadButtons"])
				{
					if (!gb.is_string()) continue;
					const SDL_GamepadButton b =
						SDL_GetGamepadButtonFromString(gb.get<std::string>().c_str());
					if (b != SDL_GAMEPAD_BUTTON_INVALID)
					{
						ActionBinding ab;
						ab.gamepadButton = b;
						binds.push_back(ab);
					}
				}
			if (!binds.empty()) { mapping.mapAction(name, std::move(binds)); ++bound; }
		}
		// One axis row. "source" is absent in every context written before mouse
		// sources existed, and its default is Key — so those parse unchanged.
		auto readAxes = [](const nlohmann::json& arr)
		{
			std::vector<AxisBinding> binds;
			for (const auto& a : arr)
			{
				if (!a.is_object()) continue;
				AxisBinding b;
				b.source      = axisSourceFromName(a.value("source", "Key"));
				b.positiveKey = SDL_GetScancodeFromName(a.value("positive", "").c_str());
				b.negativeKey = SDL_GetScancodeFromName(a.value("negative", "").c_str());
				b.scale       = a.value("scale", 1.0f);
				// Pad buttons for button-as-axis rows (D-pad movement); same
				// SDL name space as "gamepadButtons" above.
				b.positiveButton = SDL_GetGamepadButtonFromString(a.value("positiveButton", "").c_str());
				b.negativeButton = SDL_GetGamepadButtonFromString(a.value("negativeButton", "").c_str());
				// A key row needs at least one key or pad button — a one-sided
				// axis is fine. A mouse/stick row has neither and is always usable.
				if (b.source != AxisSource::Key ||
				    b.positiveKey != SDL_SCANCODE_UNKNOWN || b.negativeKey != SDL_SCANCODE_UNKNOWN ||
				    b.positiveButton != SDL_GAMEPAD_BUTTON_INVALID ||
				    b.negativeButton != SDL_GAMEPAD_BUTTON_INVALID)
					binds.push_back(b);
			}
			return binds;
		};
		if (e.contains("axes") && e["axes"].is_array())
		{
			std::vector<AxisBinding> binds = readAxes(e["axes"]);
			if (!binds.empty()) { mapping.mapAxis(name, std::move(binds)); ++bound; }
		}
		// A 2D action binds each component separately — "axesX"/"axesY" rather
		// than a shape inside "axes", so the 1D reader above cannot half-read one.
		if ((e.contains("axesX") && e["axesX"].is_array()) ||
		    (e.contains("axesY") && e["axesY"].is_array()))
		{
			std::vector<AxisBinding> bx = e.contains("axesX") ? readAxes(e["axesX"])
			                                                  : std::vector<AxisBinding>{};
			std::vector<AxisBinding> by = e.contains("axesY") ? readAxes(e["axesY"])
			                                                  : std::vector<AxisBinding>{};
			if (!bx.empty() || !by.empty())
			{ mapping.mapAxis2D(name, std::move(bx), std::move(by)); ++bound; }
		}
	}
	HE_LOG_INFO(Input, "Applied input mapping context: %zu binding group(s) from %zu entry/-ies",
	            bound, j["entries"].size());
	return bound;
}

} // namespace HE
