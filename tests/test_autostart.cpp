#include "doctest.h"
#include <Application/Autostart.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

// "Start with the machine" is a file in the user's home on two of the three
// systems, so two of the three can be checked here — by pointing HOME at a
// temporary directory and looking at what lands in it. The Windows path is the
// registry and is written blind.

namespace
{
    // Puts HOME somewhere harmless for the length of a case and gives it back
    // afterwards, so a test can never write into the real login items.
    struct TempHome
    {
        std::filesystem::path dir;
        std::string           previous;
        bool                  had = false;

        TempHome()
        {
            dir = std::filesystem::temp_directory_path() / "he_autostart_home";
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            if (const char* h = std::getenv("HOME")) { previous = h; had = true; }
            setenv("HOME", dir.string().c_str(), 1);
        }
        ~TempHome()
        {
            if (had) setenv("HOME", previous.c_str(), 1);
            else     unsetenv("HOME");
            std::filesystem::remove_all(dir);
        }
    };

    std::string readAll(const std::filesystem::path& p)
    {
        std::ifstream f(p);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
}

TEST_CASE("The login entry is written, found and taken back again")
{
#ifdef _WIN32
    MESSAGE("the Windows path is the registry; this case checks the file one");
#else
    TempHome home;
    const std::string bundle = "com.example.notes";
    const std::filesystem::path exe = "/Applications/Notes.app";

    // Nothing yet: an application that was never asked does not start at login.
    CHECK_FALSE(HE::heAutostart(bundle));

    REQUIRE(HE::heSetAutostart(bundle, "Notes", exe, true));
    const std::filesystem::path entry = HE::heAutostartPath(bundle);
    REQUIRE_FALSE(entry.empty());
    CHECK(std::filesystem::exists(entry));
    // It lives under the home that was just set, which is what makes this test
    // harmless — and what proves the entry is the USER's, not the machine's.
    CHECK(entry.string().rfind(home.dir.string(), 0) == 0);
    CHECK(HE::heAutostart(bundle));

    // The content names the application and what to run, and nothing else: this
    // is "start with my session", not a service to be revived when it exits.
    const std::string text = readAll(entry);
    CHECK(text.find(exe.string()) != std::string::npos);
    CHECK(text.find(bundle) != std::string::npos);

    // Taking it back removes the entry rather than leaving a disabled one, so
    // the answer to "does this start at login" has one shape.
    REQUIRE(HE::heSetAutostart(bundle, "Notes", exe, false));
    CHECK_FALSE(std::filesystem::exists(entry));
    CHECK_FALSE(HE::heAutostart(bundle));
    // …and doing it twice is not a failure: the state asked for is the state.
    CHECK(HE::heSetAutostart(bundle, "Notes", exe, false));
#endif
}

TEST_CASE("An entry without an identifier is refused, not written somewhere")
{
    // The bundle id is the file's name. Without one there is no entry to find
    // again, so writing anything at all would be leaving litter nobody can
    // remove through the same door.
    CHECK(HE::heAutostartPath("").empty());
    CHECK_FALSE(HE::heAutostart(""));
    CHECK_FALSE(HE::heSetAutostart("", "Notes", "/Applications/Notes.app", true));
}

TEST_CASE("What goes in the entry is what that system reads")
{
    const std::string text =
        HE::heAutostartText("com.example.notes", "Notes", "/Applications/Notes.app");
    REQUIRE_FALSE(text.empty());
#if defined(__APPLE__)
    // A LaunchAgent: the label is the bundle id, the program is the .app, and
    // RunAtLoad is the whole point.
    CHECK(text.find("<key>Label</key><string>com.example.notes</string>") != std::string::npos);
    CHECK(text.find("<key>RunAtLoad</key><true/>") != std::string::npos);
    CHECK(text.find("<string>/Applications/Notes.app</string>") != std::string::npos);
#elif !defined(_WIN32)
    CHECK(text.find("[Desktop Entry]") != std::string::npos);
    CHECK(text.find("Exec=/Applications/Notes.app") != std::string::npos);
    CHECK(text.find("X-GNOME-Autostart-enabled=true") != std::string::npos);
#endif
}
