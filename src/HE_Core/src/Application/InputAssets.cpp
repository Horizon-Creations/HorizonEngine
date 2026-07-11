#include "Application/InputAssets.h"
#include "Application/InputMapping.h"
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <filesystem>
#include <vector>

namespace HE
{

std::string inputEventPressed (const std::string& a) { return "Input." + a + ".Pressed"; }
std::string inputEventReleased(const std::string& a) { return "Input." + a + ".Released"; }
std::string inputEventAxis    (const std::string& a) { return "Input." + a + ".Axis"; }

bool inputActionIsAxis(const std::string& json)
{
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	return j.is_object() && j.value("valueType", "Button") == std::string("Axis");
}

std::string inputActionNameFromPath(const std::string& path)
{
	return std::filesystem::path(path).stem().string();
}

size_t applyInputMappingContext(InputMapping& mapping, const std::string& json)
{
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array())
		return 0;

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
		if (e.contains("axes") && e["axes"].is_array())
		{
			std::vector<AxisBinding> binds;
			for (const auto& a : e["axes"])
			{
				if (!a.is_object()) continue;
				AxisBinding b;
				b.positiveKey = SDL_GetScancodeFromName(a.value("positive", "").c_str());
				b.negativeKey = SDL_GetScancodeFromName(a.value("negative", "").c_str());
				b.scale       = a.value("scale", 1.0f);
				// A one-sided axis (only positive or only negative) is fine.
				if (b.positiveKey != SDL_SCANCODE_UNKNOWN || b.negativeKey != SDL_SCANCODE_UNKNOWN)
					binds.push_back(b);
			}
			if (!binds.empty()) { mapping.mapAxis(name, std::move(binds)); ++bound; }
		}
	}
	return bound;
}

} // namespace HE
