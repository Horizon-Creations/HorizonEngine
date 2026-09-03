#pragma once
#include <Types/Defines.h>
#include <filesystem>
#include <string>

namespace HE {

// ── Starting with the machine (plan A7) ──────────────────────────────────────
// Every system has a place where "run this at login" is written down, and on two
// of the three it is a FILE in the user's home: a LaunchAgent plist on macOS, a
// .desktop under ~/.config/autostart on Linux. Windows keeps it in the registry
// instead, which is why the writer below is the only part of this that cannot be
// tested by reading what it wrote.
//
// The identifier is the application's bundle id, so the entry belongs to the
// application and not to whatever the executable happens to be called this week.

// Where the entry lives, or an empty path when this platform keeps it elsewhere
// (Windows) or the home directory cannot be found.
HE_API std::filesystem::path heAutostartPath(const std::string& bundleId);

// What goes in it. Separate from writing it so the content can be checked
// without touching the machine the check runs on.
HE_API std::string heAutostartText(const std::string& bundleId, const std::string& appName,
                                   const std::filesystem::path& executable);

// Is this application set to start at login? Reads the file (or the registry on
// Windows) and nothing else — the question is what the SYSTEM was told, not what
// the application believes.
HE_API bool heAutostart(const std::string& bundleId);

// Say so, or take it back. False when the entry could not be written or removed;
// the caller is expected to say so rather than to pretend it worked.
HE_API bool heSetAutostart(const std::string& bundleId, const std::string& appName,
                           const std::filesystem::path& executable, bool enabled);

} // namespace HE
