#include "CompiledBuildConfig.h"

// HE_GAME_BUILD_CONFIG is 1 only when CMake was given the generated header
// (-DHE_GAME_BUILD_CONFIG=<path>, HE_Game/CMakeLists.txt), which also puts its
// directory first on this target's include path. The only TU that includes it,
// so a new header recompiles one file. Defined 0 otherwise, so the #if below
// never reads an undefined macro (docs/coding-conventions.md section 9).
#ifndef HE_GAME_BUILD_CONFIG
#define HE_GAME_BUILD_CONFIG 0
#endif
#if HE_GAME_BUILD_CONFIG
#include "HEGameBuildConfig.h"

const HE::GameBuildConfig* compiledGameBuildConfig() { return &HE::GameBuild::kConfig; }
#else
const HE::GameBuildConfig* compiledGameBuildConfig() { return nullptr; }
#endif
