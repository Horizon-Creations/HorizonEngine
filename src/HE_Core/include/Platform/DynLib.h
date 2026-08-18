#pragma once
#include <Types/Defines.h> // HE_API
#include <string>
#include <filesystem>

namespace HE {

// Thin cross-platform wrapper around LoadLibrary/dlopen.
// Used by GameLogicLoader to load GameLogic.dll at runtime, and by
// ScriptContext (HorizonScene) to load the Python plugin — HE_API because
// HE_Core does not export-all on Windows: without it every out-of-module
// user hits LNK2019 on ~DynLib/load/getSymbol.
class HE_API DynLib {
public:
    DynLib() = default;
    ~DynLib();

    DynLib(const DynLib&)            = delete;
    DynLib& operator=(const DynLib&) = delete;

    DynLib(DynLib&& other) noexcept;
    DynLib& operator=(DynLib&& other) noexcept;

    // Load a shared library from disk. Returns false on failure.
    bool load(const std::filesystem::path& path);

    // Free the library if currently loaded.
    void unload();

    // Resolve a symbol by name. Returns nullptr if not found / not loaded.
    void* getSymbol(const std::string& name) const;

    bool  isLoaded() const { return m_handle != nullptr; }
    void* nativeHandle() const { return m_handle; }

private:
    void* m_handle = nullptr;
};

} // namespace HE
