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

bool inputActionRunsWhilePaused(const std::string& json)
{
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	// Anything that is not an explicit `true` — a missing field, a malformed
	// payload, a string — reads as false. An action that fires during a pause
	// has to SAY so; guessing yes would let a stray character re-arm the game.
	if (!j.is_object()) return false;
	const auto it = j.find("runWhilePaused");
	return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::string makeInputActionJson(const std::string& valueType, bool runWhilePaused)
{
	nlohmann::json j;
	j["valueType"] = valueType.empty() ? "Button" : valueType;
	// Written even when false: an author reading the file should see the switch
	// exists, not have to know that its absence means "off".
	j["runWhilePaused"] = runWhilePaused;
	return j.dump();
}

std::string inputActionNameFromPath(const std::string& path)
{
	return std::filesystem::path(path).stem().string();
}

std::string axisSourceName(AxisSource s)
{
	switch (s)
	{
		case AxisSource::MouseX:     return "MouseX";
		case AxisSource::MouseY:     return "MouseY";
		case AxisSource::MouseWheel: return "MouseWheel";
		case AxisSource::Key:        break;
	}
	return "Key";
}

AxisSource axisSourceFromName(const std::string& name)
{
	if (name == "MouseX")     return AxisSource::MouseX;
	if (name == "MouseY")     return AxisSource::MouseY;
	if (name == "MouseWheel") return AxisSource::MouseWheel;
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

		if (e.contains("keys") && e["keys"].is_array())
		{
			std::vector<ActionBinding> binds;
			for (const auto& k : e["keys"])
			{
				if (!k.is_string()) continue;
				const SDL_Scancode sc = SDL_GetScancodeFromName(k.get<std::string>().c_str());
				if (sc != SDL_SCANCODE_UNKNOWN) binds.push_back({ sc });
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
				// A key row needs at least one key — a one-sided axis is fine.
				// A mouse row has no keys at all and is always usable.
				if (b.source != AxisSource::Key ||
				    b.positiveKey != SDL_SCANCODE_UNKNOWN || b.negativeKey != SDL_SCANCODE_UNKNOWN)
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
