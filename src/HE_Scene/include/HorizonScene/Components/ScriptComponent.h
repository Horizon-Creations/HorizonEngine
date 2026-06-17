#pragma once
#include <Types/UUID.h>
#include <Scripting/ScriptTypes.h>
#include <string>
#include <unordered_map>

// Identifies a Lua script for an entity. ScriptEngine manages the lifecycle.
struct ScriptComponent {
    HE::UUID    scriptAssetId;   // asset ID of the .lua file
    std::string moduleName;      // logical name used with ScriptEngine::loadScript
    bool        enabled = true;

    // Per-instance overrides for properties declared in M.properties.
    // Injected into the script instance before onStart in play mode.
    std::unordered_map<std::string, ScriptPropValue> properties;
};
