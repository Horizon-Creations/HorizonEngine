#include "Platform/DynLib.h"

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
#else
    m_handle = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
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
    if (!m_handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(m_handle), name.c_str()));
#else
    return ::dlsym(m_handle, name.c_str());
#endif
}

} // namespace HE
