#pragma once
#include <string>
#include <unordered_map>
#include <vector>

enum class ScriptPropType { Float, Int, Bool, String };

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
