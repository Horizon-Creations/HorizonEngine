#pragma once
#include "HE_TOOLS_API.h"
#include <filesystem>
#include <string>

// ── Generated C++ types header ───────────────────────────────────────────────
// C++ projects get the project's Struct/Enum assets as REAL C++ types: the
// editor regenerates Source/Generated/GameTypes.h from the TypeRegistry
// whenever a definition is saved (and at project open), so gameplay code can
// `#include "Generated/GameTypes.h"` and use `enum class Weapon` /
// `struct PlayerStats` with the authored defaults as member initializers.
//
// The header is self-contained (std:: only — GameLogic compiles against
// <IGameLogic.h> alone, no glm/nlohmann): Vec2/Color/Transform fields map to
// tiny HeVec2/HeColor/HeTransform helper structs emitted once at the top.
// Structs are emitted in dependency order (the panel's cycle guard makes that
// a well-defined topological sort); names are sanitized into valid C++
// identifiers with the original spelling in a comment when they differ.

namespace HE {

// The full header text from the CURRENT TypeRegistry contents.
HE_TOOLS_API std::string generateCppTypesHeader();

// Write generateCppTypesHeader() to <projectDir>/Source/Generated/GameTypes.h
// (creating the directory; atomic temp+rename; skips the write when the file
// already holds identical bytes so build systems don't see phantom changes).
// Returns false only on I/O failure.
HE_TOOLS_API bool writeCppTypesHeader(const std::filesystem::path& projectDir);

} // namespace HE
