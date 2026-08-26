#include "HorizonCode/HorizonCodeGenSupport.h"
#include <cstdint>
#include <Diagnostics/Logger.h>
#include <Scripting/ScriptTypes.h>   // scriptLogLine — the same tag the interpreter prepends

// The out-of-line pieces of hc:: — everything that needs the Logger. Each log
// text is byte-identical to the interpreter's (HorizonCode.cpp), so a packaged
// game's log reads the same whichever backend ran the script.

namespace hc {

std::string toStringG(float v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

void warnArrayGet(int idx, size_t size)
{
    HE_LOG_WARN(HorizonCode, "%s",
        ("HorizonCode: Array Get index " + std::to_string(idx) + " out of range (size " +
         std::to_string(size) + ")").c_str());
}

// Defaults live in the header only — repeating them here is ill-formed.
uint32_t createObject(const Context& c, const char* classPath,
                      const float* position, const float* rotationEuler)
{
    const uint32_t ref = c.createObject
        ? c.createObject(classPath, position, rotationEuler) : 0u;
    if (ref == 0u)
        HE_LOG_ERROR(HorizonCode, "%s",
            ("HorizonCode: Create Object failed — class '" + std::string(classPath) +
             "' not found").c_str());
    return ref;
}

void print(const std::string& s)
{
    // Through HE::scriptLogLine, exactly like the interpreter's Print. This used
    // to concatenate its own "[Widget] " prefix, which is how the editor and the
    // packaged build ended up logging the same Print node differently — the tag
    // has one owner now so that cannot come back.
    HE_LOG_INFO(HorizonCode, "%s", HE::scriptLogLine(s).c_str());
}

void warnStepLimit()
{
    HE_LOG_WARN(HorizonCode, "%s",
        "HorizonCode: execution step limit hit — aborting run");
}

} // namespace hc
