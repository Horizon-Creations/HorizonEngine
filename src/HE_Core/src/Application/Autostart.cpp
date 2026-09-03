#include <Application/Autostart.h>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace HE {

namespace
{
    std::filesystem::path homeDir()
    {
        // getenv and not a platform API on purpose: the two systems that keep
        // this in a file both put it under $HOME, and a test can point $HOME
        // somewhere harmless.
        if (const char* h = std::getenv("HOME")) return std::filesystem::path(h);
#ifdef _WIN32
        if (const char* p = std::getenv("USERPROFILE")) return std::filesystem::path(p);
#endif
        return {};
    }

    std::string xmlEscape(const std::string& s)
    {
        std::string out;
        for (char c : s)
            switch (c)
            {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
            }
        return out;
    }
}

std::filesystem::path heAutostartPath(const std::string& bundleId)
{
    if (bundleId.empty()) return {};
    const std::filesystem::path home = homeDir();
    if (home.empty()) return {};
#if defined(__APPLE__)
    return home / "Library" / "LaunchAgents" / (bundleId + ".plist");
#elif defined(_WIN32)
    // The registry, not a file. Named here as an empty path so every caller has
    // one answer to "is there a file" and the Windows branches below are the
    // only place that knows better.
    return {};
#else
    return home / ".config" / "autostart" / (bundleId + ".desktop");
#endif
}

std::string heAutostartText(const std::string& bundleId, const std::string& appName,
                            const std::filesystem::path& executable)
{
#if defined(__APPLE__)
    // A LaunchAgent. RunAtLoad and nothing else: this is "start with the user's
    // session", not a daemon that should be revived when it exits.
    return std::string(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>") + xmlEscape(bundleId) + "</string>\n"
        "  <key>ProgramArguments</key>\n  <array><string>"
        + xmlEscape(executable.string()) + "</string></array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "</dict>\n</plist>\n";
#elif defined(_WIN32)
    // The registry value IS the command line; there is no file to write.
    (void)bundleId; (void)appName;
    return "\"" + executable.string() + "\"";
#else
    return std::string("[Desktop Entry]\n") +
        "Type=Application\n"
        "Name=" + appName + "\n"
        "Exec=" + executable.string() + "\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Icon=" + bundleId + "\n";
#endif
}

bool heAutostart(const std::string& bundleId)
{
#ifdef _WIN32
    // Written blind: this machine cannot run it. The shape is the documented
    // one — HKCU\...\Run holds one value per program, named after it.
    if (bundleId.empty()) return false;
    HKEY key{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const LONG r = RegQueryValueExA(key, bundleId.c_str(), nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
#else
    const std::filesystem::path p = heAutostartPath(bundleId);
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
#endif
}

bool heSetAutostart(const std::string& bundleId, const std::string& appName,
                    const std::filesystem::path& executable, bool enabled)
{
    if (bundleId.empty()) return false;
#ifdef _WIN32
    // Also blind, and deliberately HKEY_CURRENT_USER: an application that can
    // only arrange this with administrator rights cannot arrange it at all.
    HKEY key{};
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = ERROR_SUCCESS;
    if (enabled)
    {
        const std::string cmd = heAutostartText(bundleId, appName, executable);
        r = RegSetValueExA(key, bundleId.c_str(), 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>(cmd.size() + 1));
    }
    else
    {
        r = RegDeleteValueA(key, bundleId.c_str());
        // Removing something that was never there is the state the caller asked
        // for, so it is not a failure.
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
#else
    const std::filesystem::path p = heAutostartPath(bundleId);
    if (p.empty()) return false;
    std::error_code ec;
    if (!enabled)
    {
        std::filesystem::remove(p, ec);
        return !std::filesystem::exists(p, ec);
    }
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    if (!f) return false;
    f << heAutostartText(bundleId, appName, executable);
    f.close();
    return !f.fail();
#endif
}

} // namespace HE
