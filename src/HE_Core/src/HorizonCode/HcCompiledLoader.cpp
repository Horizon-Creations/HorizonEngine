#include "HorizonCode/HcCompiledLoader.h"
#include <Diagnostics/Logger.h>
#include <Platform/BuildProfile.h>

#include <cstring>

namespace HorizonCode {

namespace {
using ManifestFn = const CompiledClassEntry* (*)(int* count, const char** engineVersion);
using ProfileFn  = const char* (*)();
}

bool CompiledClassTable::load(const std::filesystem::path& libPath, const std::string& engineVersion,
                              const char* profile)
{
    if (m_lib.isLoaded()) return true;   // process-lifetime: load once

    std::error_code ec;
    if (!std::filesystem::exists(libPath, ec))
        return false;   // absence is the normal interpreted case — callers log context

    if (!m_lib.load(libPath))
    {
        HE_LOG_WARN(HorizonCode, "%s",
            ("HorizonCode: could not load compiled classes from '" + libPath.string() +
             "' — running interpreted").c_str());
        return false;
    }
    const auto manifest = (ManifestFn)m_lib.getSymbol("HE_HorizonCodeGenClasses");
    if (!manifest)
    {
        HE_LOG_WARN(HorizonCode, "%s",
            ("HorizonCode: '" + libPath.string() +
             "' has no HE_HorizonCodeGenClasses export — running interpreted").c_str());
        m_lib.unload();
        return false;
    }
    int count = 0;
    const char* baked = nullptr;
    const CompiledClassEntry* entries = manifest(&count, &baked);
    // The ABI handshake: a library built against another engine build would be
    // undefined behavior to call into — reject it cleanly instead.
    if (!baked || engineVersion != baked)
    {
        HE_LOG_ERROR(HorizonCode, "%s",
            ("HorizonCode: compiled classes were built for engine '" +
             std::string(baked ? baked : "?") + "' but this is '" + engineVersion +
             "' — running interpreted").c_str());
        m_lib.unload();
        return false;
    }
    // The profile handshake: the classes were compiled with their SDK lib set's
    // HE_BUILD_PROFILE_* switches, and call into an engine that must have the
    // same ones. Same version, other profile is still a mix.
    const char* want = profile ? profile : HE::kBuildProfileName;
    const auto profileFn = (ProfileFn)m_lib.getSymbol("HE_HorizonCodeGenProfile");
    const char* built = profileFn ? profileFn() : nullptr;
    if (!built || std::strcmp(built, want) != 0)
    {
        HE_LOG_ERROR(HorizonCode, "%s",
            ("HorizonCode: compiled classes in '" + libPath.string() + "' were built for the '" +
             std::string(built ? built : "unknown") + "' profile but this engine is '" + want +
             "' — running interpreted; export the game again").c_str());
        m_lib.unload();
        return false;
    }
    for (int i = 0; i < count; ++i)
        if (entries[i].key && entries[i].create && entries[i].destroy)
            m_entries.emplace(entries[i].key, &entries[i]);
    HE_LOG_INFO(HorizonCode, "%s",
        ("HorizonCode: " + std::to_string(m_entries.size()) + " compiled classes").c_str());
    return true;
}

const CompiledClassEntry* CompiledClassTable::find(const std::string& key) const
{
    auto it = m_entries.find(key);
    return it != m_entries.end() ? it->second : nullptr;
}

CompiledPtr CompiledClassTable::create(const std::string& key) const
{
    const CompiledClassEntry* e = find(key);
    if (!e) return {};
    return CompiledPtr(e->create(), CompiledDeleter{ e->destroy });
}

CompiledClassTable& compiledClasses()
{
    static CompiledClassTable s_table;
    return s_table;
}

const char* compiledLibraryName()
{
#if defined(_WIN32)
    return "HorizonCodeGen.dll";
#elif defined(__APPLE__)
    return "libHorizonCodeGen.dylib";
#else
    return "libHorizonCodeGen.so";
#endif
}

} // namespace HorizonCode
