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
    : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

DynLib& DynLib::operator=(DynLib&& other) noexcept
{
    if (this != &other) {
        unload();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool DynLib::load(const std::filesystem::path& path)
{
    unload();
#if defined(_WIN32)
    handle_ = static_cast<void*>(::LoadLibraryW(path.wstring().c_str()));
#else
    handle_ = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    return handle_ != nullptr;
}

void DynLib::unload()
{
    if (!handle_) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* DynLib::getSymbol(const std::string& name) const
{
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), name.c_str()));
#else
    return ::dlsym(handle_, name.c_str());
#endif
}

} // namespace HE
