#pragma once
#include <string>
#include <filesystem>

namespace HE {

// Thin cross-platform wrapper around LoadLibrary/dlopen.
// Used by GameLogicLoader to load GameLogic.dll at runtime.
class DynLib {
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

    bool  isLoaded() const { return handle_ != nullptr; }
    void* nativeHandle() const { return handle_; }

private:
    void* handle_ = nullptr;
};

} // namespace HE
