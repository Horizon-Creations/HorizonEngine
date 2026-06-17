#pragma once
#include <Types/UUID.h>
#include <string>

// Identifies a Lua script for an entity. ScriptEngine manages the lifecycle.
struct ScriptComponent {
    HE::UUID    scriptAssetId;   // asset ID of the .lua file
    std::string moduleName;      // logical name used with ScriptEngine::loadScript
    bool        enabled = true;
};
