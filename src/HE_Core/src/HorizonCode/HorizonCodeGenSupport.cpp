#include "HorizonCode/HorizonCodeGenSupport.h"
#include <cstdint>
#include <Diagnostics/Logger.h>

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
    Logger::Log(Logger::LogLevel::Warning,
        ("HorizonCode: Array Get index " + std::to_string(idx) + " out of range (size " +
         std::to_string(size) + ")").c_str());
}

uint32_t createObject(const Context& c, const char* classPath)
{
    const uint32_t ref = c.createObject ? c.createObject(classPath) : 0u;
    if (ref == 0u)
        Logger::Log(Logger::LogLevel::Error,
            ("HorizonCode: Create Object failed — class '" + std::string(classPath) +
             "' not found").c_str());
    return ref;
}

void print(const std::string& s)
{
    Logger::Log(Logger::LogLevel::Info, ("[Widget] " + s).c_str());
}

void warnStepLimit()
{
    Logger::Log(Logger::LogLevel::Warning,
        "HorizonCode: execution step limit hit — aborting run");
}

} // namespace hc
