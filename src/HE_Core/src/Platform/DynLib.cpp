#include "Platform/DynLib.h"
#include "Diagnostics/Log.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace HE {

DynLib::~DynLib()
{
    unload();
}

DynLib::DynLib(DynLib&& other) noexcept
    : m_handle(other.m_handle)
{
    other.m_handle = nullptr;
}

DynLib& DynLib::operator=(DynLib&& other) noexcept
{
    if (this != &other) {
        unload();
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

bool DynLib::load(const std::filesystem::path& path)
{
    unload();
#if defined(_WIN32)
    m_handle = static_cast<void*>(::LoadLibraryW(path.wstring().c_str()));
    if (!m_handle)
        // GetLastError is the only thing that distinguishes "file not there"
        // from "a dependent DLL is missing" — the second is by far the more
        // common and the more confusing of the two.
        HE_LOG_ERROR(Platform, "LoadLibrary('%s') failed, GetLastError=%lu",
                     path.string().c_str(), static_cast<unsigned long>(::GetLastError()));
#else
    m_handle = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle)
    {
        const char* err = ::dlerror();
        HE_LOG_ERROR(Platform, "dlopen('%s') failed: %s",
                     path.string().c_str(), err ? err : "unknown error");
    }
#endif
    if (m_handle)
        HE_LOG_INFO(Platform, "Loaded dynamic library '%s'", path.string().c_str());
    return m_handle != nullptr;
}

void DynLib::unload()
{
    if (!m_handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(m_handle));
#else
    ::dlclose(m_handle);
#endif
    m_handle = nullptr;
}

void* DynLib::getSymbol(const std::string& name) const
{
    if (!m_handle)
    {
        HE_LOG_WARN(Platform, "getSymbol('%s') on a library that is not loaded", name.c_str());
        return nullptr;
    }
#if defined(_WIN32)
    void* sym = reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m_handle), name.c_str()));
#else
    void* sym = ::dlsym(m_handle, name.c_str());
#endif
    if (!sym)
        // Almost always a missing extern "C" or a name-mangling mismatch — the
        // caller only sees a null entry point and gives up silently.
        HE_LOG_ERROR(Platform, "Symbol '%s' not found in the loaded library", name.c_str());
    return sym;
}

} // namespace HE
