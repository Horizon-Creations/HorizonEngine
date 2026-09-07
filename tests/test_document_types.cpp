#include "doctest.h"
#include <Application/DocumentTypes.h>
#include <string>
#include <vector>

// Three systems, three words for one declaration. What each of them wants is
// TEXT, so what these check is text — and the point of checking it here is that
// the alternative is finding out on somebody else's machine that a file type
// silently did not register.

namespace
{
    const std::vector<HE::AppDocumentType> kTypes = {
        { "hnote", "Horizon Note", "description" },
        { "hlist", "Horizon List", "" },
    };
    const std::string kBundle = "com.example.notes";

    bool has(const std::string& haystack, const std::string& needle)
    {
        return haystack.find(needle) != std::string::npos;
    }
}

TEST_CASE("An extension is what all three systems can carry, or it is refused")
{
    CHECK(HE::heValidDocumentExtension("hnote"));
    CHECK(HE::heValidDocumentExtension("h3"));
    CHECK_FALSE(HE::heValidDocumentExtension(""));
    CHECK_FALSE(HE::heValidDocumentExtension(".hnote"));   // the dot is not part of it
    CHECK_FALSE(HE::heValidDocumentExtension("HNote"));    // upper case is not a UTI
    CHECK_FALSE(HE::heValidDocumentExtension("my note"));
    CHECK_FALSE(HE::heValidDocumentExtension("3d"));       // a name may not start with a digit
    CHECK_FALSE(HE::heValidDocumentExtension("waytoolongextension"));
}

TEST_CASE("macOS is told both halves: what the app opens and what the type IS")
{
    const std::string plist = HE::heInfoPlistDocumentTypes(kTypes, kBundle);
    REQUIRE_FALSE(plist.empty());

    // Half one: the application opens these.
    CHECK(has(plist, "<key>CFBundleDocumentTypes</key>"));
    CHECK(has(plist, "<key>CFBundleTypeName</key><string>Horizon Note</string>"));
    CHECK(has(plist, "<key>CFBundleTypeRole</key><string>Editor</string>"));
    // Half two, and the one that is easy to forget: without an exported
    // declaration the identifier above is a name the system has never heard of,
    // and it associates nothing at all.
    CHECK(has(plist, "<key>UTExportedTypeDeclarations</key>"));
    CHECK(has(plist, "<string>com.example.notes.hnote</string>"));
    CHECK(has(plist, "<key>public.filename-extension</key>"));
    CHECK(has(plist, "<array><string>hnote</string></array>"));
    // The two halves must agree about the identifier, which is why it is derived
    // from the bundle id in one place and never typed twice.
    CHECK(has(plist, "<string>com.example.notes.hlist</string>"));

    // A project that claims no types produces nothing, so the plist reads
    // exactly as it did before document types existed.
    CHECK(HE::heInfoPlistDocumentTypes({}, kBundle).empty());
    // …and so does one whose only type is unusable.
    CHECK(HE::heInfoPlistDocumentTypes({ { "Not Valid", "X", "" } }, kBundle).empty());
}

TEST_CASE("Linux gets a .desktop that says %F, and a MIME file that names the glob")
{
    const std::string desktop = HE::heDesktopEntry("Notes", "HorizonGame", kBundle, kTypes);
    CHECK(has(desktop, "[Desktop Entry]"));
    CHECK(has(desktop, "Name=Notes"));
    // %F and not %f: opening a selection hands the application every file at
    // once, which is what the argv path on the receiving side reads.
    CHECK(has(desktop, "Exec=HorizonGame %F"));
    CHECK(has(desktop, "MimeType=application/x-com-example-notes-hnote;"));

    const std::string mime = HE::heSharedMimeInfo(kBundle, kTypes);
    CHECK(has(mime, "<mime-type type=\"application/x-com-example-notes-hnote\">"));
    CHECK(has(mime, "<glob pattern=\"*.hnote\"/>"));
    CHECK(has(mime, "<comment>Horizon Note</comment>"));
    // The MIME type in the two files has to be the same string, or the desktop
    // entry claims a type nothing defines.
    CHECK(has(desktop, "application/x-com-example-notes-hlist"));
    CHECK(has(mime, "application/x-com-example-notes-hlist"));
}

TEST_CASE("Windows gets a .reg for the current user, with the open command quoted")
{
    const std::string reg =
        HE::heWindowsRegistration("Notes", "HorizonGame.exe", kBundle, kTypes);
    CHECK(has(reg, "Windows Registry Editor Version 5.00"));
    // HKEY_CURRENT_USER, so applying it needs no administrator — an application
    // that can only register itself from an elevated prompt does not register.
    CHECK(has(reg, "[HKEY_CURRENT_USER\\Software\\Classes\\.hnote]"));
    CHECK(has(reg, "@=\"com.example.notes.hnote\""));
    CHECK(has(reg, "[HKEY_CURRENT_USER\\Software\\Classes\\com.example.notes.hnote\\shell\\open\\command]"));
    // The command has to survive a path with spaces, which means the inner
    // quotes are escaped: @="\"...exe\" \"%1\"".
    CHECK(has(reg, "\\\"%1\\\""));
    CHECK(has(reg, "HorizonGame.exe\\\""));
}

TEST_CASE("An unusable extension is skipped everywhere, not written differently")
{
    const std::vector<HE::AppDocumentType> mixed = {
        { "good", "Good", "" }, { "BAD ONE", "Bad", "" },
    };
    for (const std::string& out : { HE::heInfoPlistDocumentTypes(mixed, kBundle),
                                    HE::heDesktopEntry("A", "x", kBundle, mixed),
                                    HE::heSharedMimeInfo(kBundle, mixed),
                                    HE::heWindowsRegistration("A", "x", kBundle, mixed) })
    {
        CHECK(has(out, "good"));
        CHECK_FALSE(has(out, "BAD ONE"));
    }
}
