#include "CompiledBuildConfig.h"

// HE_GAME_BUILD_CONFIG is 1 only when CMake was given the generated header
// (-DHE_GAME_BUILD_CONFIG=<path>, HE_Game/CMakeLists.txt), which also puts its
// directory first on this target's include path. The only TU that includes it,
// so a new header recompiles one file.
#if HE_GAME_BUILD_CONFIG
#include "HEGameBuildConfig.h"

const HE::GameBuildConfig* compiledGameBuildConfig() { return &HE::GameBuild::kConfig; }
#else
const HE::GameBuildConfig* compiledGameBuildConfig() { return nullptr; }
#endif
