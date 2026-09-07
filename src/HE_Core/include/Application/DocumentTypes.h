#pragma once
#include <Types/Defines.h>
#include <string>
#include <vector>

namespace HE {

// ── The file types an application owns (plan A7) ─────────────────────────────
// "Open with", from the other side: before a system can offer an application for
// a double-clicked file, the application has to have SAID which files are its.
// Every platform wants that in its own words, so this is one small declaration
// and three renderers of it.
//
// The renderers return TEXT rather than writing files, for the same reason the
// icon renderer returns pixels: what these produce is exactly what a test can
// compare, and a format that is only ever checked by the code that wrote it is
// not checked.
struct HE_API AppDocumentType
{
    // Without the dot, lower case: "hnote". The system's word for the type is
    // derived from it, so it has to be usable in a reverse-domain name.
    std::string extension;
    std::string displayName;   // "Horizon Note" — what a file dialog shows
    // One of the engine's built-in icons, drawn on the application's plate. Empty
    // uses the application's own icon, which is better than nothing and much
    // better than making somebody draw a second one.
    std::string iconName;
};

// Letters and digits only, at most 12, no dot: what every one of the three
// systems can carry and what a UTI may contain. Everything else is refused where
// it is typed rather than producing a declaration nobody can use.
HE_API bool heValidDocumentExtension(const std::string& ext);

// macOS: the CFBundleDocumentTypes + UTExportedTypeDeclarations block, indented
// to sit inside an Info.plist <dict>. Empty string for no types — the plist then
// looks exactly as it did before this existed.
HE_API std::string heInfoPlistDocumentTypes(const std::vector<AppDocumentType>& types,
                                            const std::string& bundleId);

// Linux: the .desktop entry (with MimeType=) and the shared-mime-info XML that
// gives those MIME types a name and a glob. Installing them is `xdg-mime` /
// `desktop-file-install`, which belongs to an installer and not to an export.
HE_API std::string heDesktopEntry(const std::string& appName, const std::string& exeName,
                                  const std::string& bundleId,
                                  const std::vector<AppDocumentType>& types);
HE_API std::string heSharedMimeInfo(const std::string& bundleId,
                                    const std::vector<AppDocumentType>& types);

// Windows: a .reg file for HKEY_CURRENT_USER\Software\Classes. Also an
// installer's job — an export writes no registry, it writes the file that would.
HE_API std::string heWindowsRegistration(const std::string& appName,
                                         const std::string& exeName,
                                         const std::string& bundleId,
                                         const std::vector<AppDocumentType>& types);

} // namespace HE
