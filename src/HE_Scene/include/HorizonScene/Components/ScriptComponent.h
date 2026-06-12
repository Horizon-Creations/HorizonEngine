#pragma once
#include <Types/UUID.h>
#include <string>

// Holds a reference to a Python script module.
// HorizonScripting reads this component and manages the Python object lifecycle.
struct ScriptComponent {
    HE::UUID    scriptAssetId;   // asset ID of the .py file
    std::string moduleName;      // e.g. "player_controller"
    bool        enabled = true;
};
