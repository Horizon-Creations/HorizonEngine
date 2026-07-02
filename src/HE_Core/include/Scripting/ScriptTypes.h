#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class ScriptPropType { Float, Int, Bool, String };

// Gameplay scripting language of a ScriptAsset. Lua is 0 so that language-tagged
// instance ids (language in the high byte) stay bit-identical to the pre-tagging
// Lua-only ids.
enum class ScriptLanguage : uint8_t { Lua = 0, Python = 1 };

// Derive the language from an asset/file path by extension (".py" → Python,
// everything else → Lua, the engine's default scripting language).
inline ScriptLanguage scriptLanguageFromPath(const std::string& path)
{
	if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".py") == 0)
		return ScriptLanguage::Python;
	return ScriptLanguage::Lua;
}

// A single typed value used both as a default (in ScriptPropDef) and as a
// per-instance override stored in ScriptComponent::properties.
struct ScriptPropValue {
    ScriptPropType type = ScriptPropType::Float;
    float          f    = 0.0f;
    int            i    = 0;
    bool           b    = false;
    std::string    s;
};

// One declared property: name + default value inferred from M.properties table.
struct ScriptPropDef {
    std::string    name;
    ScriptPropValue defaultVal;
};
