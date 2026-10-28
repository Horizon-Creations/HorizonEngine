#include "Platform/EngineProfile.h"

#include "Platform/BuildProfile.h"

// Compiled into HorizonCore, and with it into the HorizonEngine bundle: the
// value is the profile of THIS library's compilation, not of whoever calls it.
extern "C" const char* HorizonEngine_BuildProfile()
{
    return HE::kBuildProfileName;
}
