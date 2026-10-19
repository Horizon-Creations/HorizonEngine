#pragma once

// Which build profile the ENGINE LIBRARY was compiled with, asked at run time.
//
// Platform/BuildProfile.h answers the same question at compile time, for the
// translation unit that includes it. A game executable and the HorizonEngine
// library it loads are compiled separately (the SDK ships one lib set per
// profile), so a dev executable can end up beside a release library, or the
// other way round. Where their symbols still match, the dynamic loader does not
// notice; the game asks the library here and refuses to start on a mismatch
// (HE_Game/src/main.cpp).
//
// extern "C", so a tool can also find it by name in a library it only opened
// (HE::DynLib), without linking it.

#include "Types/Defines.h"

extern "C"
{
// "dev" or "release": HE_BUILD_PROFILE of the engine tree that built the library.
HE_API const char* HorizonEngine_BuildProfile();
}
