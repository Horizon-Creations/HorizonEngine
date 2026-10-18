// Windows only (MSVC, linked with /DELAYLOAD for the engine and SDL3, see
// HE_Game/CMakeLists.txt): load the engine DLLs from HorizonEngine\ beside the
// executable.
//
// The exported game keeps its executable, a data folder and a HorizonEngine
// folder side by side. The Windows loader resolves a static import only beside
// the executable, in the system folders and on PATH, never in a subfolder, and
// it does so before main() runs, so nothing in main() could add the folder in
// time. Delay-loaded, the import is resolved on the first call instead, through
// this hook: it loads the DLL by its full path in HorizonEngine\. The DLL's own
// imports (SDL3, a bundled lz4/zstd/OpenSSL) then resolve in that folder too
// (LOAD_WITH_ALTERED_SEARCH_PATH searches the loaded DLL's directory first).
// The same holds for a call from a static initializer, which runs before main().
//
// Anything that is not in HorizonEngine\ falls through to the normal search, so
// a game in the old flat layout (DLLs beside the executable) still starts.

#if defined(_WIN32) && defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

#include <string>

namespace
{
FARPROC WINAPI loadFromEngineDir(unsigned notify, PDelayLoadInfo info)
{
    if (notify != dliNotePreLoadLibrary || !info || !info->szDll)
        return nullptr;

    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return nullptr;
    dir.resize(slash + 1);
    dir += L"HorizonEngine\\";

    // The DLL names in the import table are ASCII.
    for (const char* c = info->szDll; *c; ++c)
        dir.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return nullptr;
    return reinterpret_cast<FARPROC>(
        LoadLibraryExW(dir.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}
} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = loadFromEngineDir;

#endif
