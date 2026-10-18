#include "ExportSdkBuild.h"

#include <Crypto/Sha256.h>
#include <Hpak/GameBuildConfig.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/ProjectExporter.h>     // readEmbeddedPakKey
#include <Platform/GameLayout.h>
#include <Project/ProjectSettings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace HE::ExportSdk
{
namespace fs = std::filesystem;

namespace
{
GameSdk sdkAt(const fs::path& root, const std::string& profile)
{
    std::error_code ec;
    if (root.empty()) return {};
    GameSdk sdk;
    sdk.packageDir  = root / "lib" / "cmake" / "HorizonEngine";
    sdk.templateDir = root / "share" / "HorizonEngine" / "template";
    // One SDK holds a lib set per profile (cmake/HorizonSDK.cmake); the one
    // asked for must be installed. Whether its header stamp still matches is
    // the package's call: a refusal there fails the configure, and the
    // prebuilt game stays.
    if (!fs::is_regular_file(sdk.packageDir / "HorizonEngineConfig.cmake", ec) ||
        !fs::is_regular_file(sdk.packageDir / ("HorizonEngineTargets-" + profile + ".cmake"), ec) ||
        !fs::is_regular_file(sdk.templateDir / "CMakeLists.txt", ec))
        return {};
    sdk.root    = root;
    sdk.profile = profile;
    return sdk;
}

// What the game executable and its engine libraries are called on disk.
bool isEngineLibrary(const fs::path& p)
{
    const std::string n = p.filename().string();
    const std::string e = p.extension().string();
    if (e == ".dylib" || e == ".dll") return true;
    // libHorizonCore.so, libSDL3.so.0, libSDL3.so.0.2.10
    return n.find(".so") != std::string::npos && n.rfind("lib", 0) == 0;
}
} // namespace

GameSdk findGameSdk(const fs::path& editorBase, const std::string& profile,
                    const fs::path& overrideRoot)
{
    if (!overrideRoot.empty()) return sdkAt(overrideRoot, profile);
    const char* dir = kGameSdkDirName;
    if (GameSdk s = sdkAt(editorBase / dir, profile); s.valid()) return s;
    if (!editorBase.empty())
    {
        // SDL_GetBasePath ends in a separator: parent_path() of ".../Editor/" is
        // ".../Editor", so strip the empty last element first.
        fs::path b = editorBase;
        if (!b.has_filename()) b = b.parent_path();
        if (GameSdk s = sdkAt(b.parent_path() / dir, profile); s.valid()) return s;
    }
    return {};
}

std::string unavailableReason(const GameSdk& sdk, const std::string& profile,
                              bool cmakeFound, bool compilerFound)
{
    if (!sdk.valid())
        return "no engine SDK with the '" + profile + "' lib set (" + kGameSdkDirName +
               " beside the editor, or HE_GAME_SDK_DIR)";
    if (!cmakeFound)    return "cmake not found";
    if (!compilerFound) return "no working C++ compiler found";
    return {};
}

std::string writeBuildConfigFromExport(const fs::path& dataDir, const fs::path& prebuiltExe,
                                       const std::string& profile, const fs::path& header)
{
    ProjectConfig cfg;
    if (!ProjectConfigLoader::load(dataDir, cfg))
        return "cannot read project.hcfg in " + dataDir.string();
    if (cfg.encrypted &&
        std::all_of(std::begin(cfg.encKey), std::end(cfg.encKey), [](uint8_t b) { return b == 0; }))
    {
        // The exporter patched the key into the prebuilt executable and left the
        // hcfg without it: the compiled game must get it from the header instead.
        if (prebuiltExe.empty() || !readEmbeddedPakKey(prebuiltExe, cfg.encKey))
            return "the pak key is neither in project.hcfg nor in the prebuilt game";
    }
    ProjectSettings settings;
    if (!loadProjectSettings(dataDir, settings))
        return "cannot read " + projectSettingsPath(dataDir).string();
    if (!writeGameBuildConfigHeader(header, cfg, &settings, profile.c_str()))
        return "cannot write " + header.string();
    return {};
}

fs::path exportDataDir(const fs::path& exportedExe)
{
    const fs::path dir = exportedExe.parent_path();
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents")
        return dir.parent_path() / "Resources";
    std::error_code ec;
    const fs::path data = dir / GameLayout::kDataDirName;
    return fs::is_directory(data, ec) ? data : dir;
}

fs::path exportEngineDir(const fs::path& exportedExe)
{
    std::error_code ec;
    const fs::path engine = exportedExe.parent_path() / GameLayout::kEngineDirName;
    return fs::is_directory(engine, ec) ? engine : exportedExe.parent_path();
}

HE::hccg::DylibBuildSpec makeBuildSpec(const GameSdk& sdk, const fs::path& workDir,
                                       const fs::path& header)
{
    HE::hccg::DylibBuildSpec spec;
    spec.sourceDir     = sdk.templateDir;
    spec.buildDir      = workDir / "build";
    spec.logFile       = workDir / "build.log";
    spec.artifactNames = { "HorizonGame", "HorizonGame.exe" };
    spec.what          = "game executable";
    spec.defines       = {
        "HorizonEngine_DIR=" + sdk.packageDir.generic_string(),
        "HE_BUILD_PROFILE=" + sdk.profile,
        "HE_GAME_NAME=HorizonGame",
        "HE_GAME_BUILD_CONFIG=" + header.generic_string(),
        "HE_GAME_PORTABLE=ON",
    };
    return spec;
}

CodegenSdkChoice chooseCodegenSdk(const GameSdk& gameSdk, bool gameFromSdk, bool runtimeHasEngineDir,
                                  const HE::hccg::SdkInfo& legacy)
{
    CodegenSdkChoice c;
    if (gameFromSdk && gameSdk.valid())
    {
        c.sdk.packageDir = gameSdk.packageDir;
        c.sdk.profile    = gameSdk.profile;
        c.fromGameSdk    = true;
        return c;
    }
    if (runtimeHasEngineDir)
    {
        c.why = "compiled HorizonCode is built with the game from the engine SDK; "
                "turn on \"Compile game from SDK\"";
        return c;
    }
    c.sdk = legacy;
    if (!c.sdk.valid()) c.why = "no codegen SDK found (HE_HCGEN_SDK, <editor>/SDK, he_sdk_config.json)";
    return c;
}

fs::path buildWorkDir(const fs::path& cacheRoot, const std::string& project, const std::string& profile)
{
    // A folder name on every platform, whatever the project is called.
    std::string name = project.empty() ? std::string("Game") : project;
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return cacheRoot / (name + "-" + profile);
}

namespace
{
std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    return s;
}

// set(PACKAGE_VERSION "0.7.0") in the package's version file.
std::string packageVersion(const fs::path& packageDir)
{
    const std::string text = readText(packageDir / "HorizonEngineConfigVersion.cmake");
    const std::string tag = "set(PACKAGE_VERSION \"";
    const size_t at = text.find(tag);
    if (at == std::string::npos) return "unknown";
    const size_t from = at + tag.size();
    const size_t to = text.find('"', from);
    return to == std::string::npos ? "unknown" : text.substr(from, to - from);
}

// Name, size and modification time of every file in the lib set, hashed: a
// reinstalled set with new libraries but the same headers still misses.
std::string libSetDigest(const GameSdk& sdk)
{
    std::vector<std::string> lines;
    std::error_code ec;
    for (const char* sub : { "lib", "bin" })
    {
        const fs::path dir = sdk.root / sub / sdk.profile;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (!e.is_regular_file(ec)) continue;
            const auto size = static_cast<unsigned long long>(e.file_size(ec));
            const auto time = static_cast<long long>(e.last_write_time(ec).time_since_epoch().count());
            lines.push_back(std::string(sub) + "/" + e.path().filename().string() + " " +
                            std::to_string(size) + " " + std::to_string(time));
        }
    }
    std::sort(lines.begin(), lines.end());
    HE::Crypto::Sha256 h;
    for (const std::string& l : lines) { h.update(l.data(), l.size()); h.update("\n", 1); }
    std::uint8_t d[HE::Crypto::Sha256::kDigestSize];
    h.finish(d);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : d) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
} // namespace

std::string buildCacheKey(const GameSdk& sdk, const HE::hccg::DylibBuildSpec& spec,
                          const std::string& toolchain)
{
    std::string key;
    const auto line = [&key](const std::string& name, const std::string& value)
    {
        key += name + "=" + value + "\n";
    };
    line("sdk", sdk.root.generic_string());
    line("version", packageVersion(sdk.packageDir));
    line("headers", trimmed(readText(sdk.packageDir / ("HorizonEngineHeaders-" + sdk.profile + ".txt"))));
    line("libs", libSetDigest(sdk));
    line("profile", sdk.profile);
    line("toolchain", toolchain.empty() ? std::string("unknown") : toolchain);
    for (const std::string& d : spec.defines) line("define", d);
    return key;
}

CacheCheck prepareBuildCache(const fs::path& workDir, const std::string& key)
{
    std::error_code ec;
    const fs::path keyFile = workDir / "cache-key.txt";
    const fs::path tree    = workDir / "build";
    CacheCheck c;
    if (!fs::is_regular_file(keyFile, ec) || !fs::is_regular_file(tree / "CMakeCache.txt", ec))
        c.changed = "no earlier build";
    else
    {
        // The first line that differs names the input that changed.
        std::istringstream was(readText(keyFile)), now(key);
        std::string a, b;
        while (true)
        {
            const bool moreA = static_cast<bool>(std::getline(was, a));
            const bool moreB = static_cast<bool>(std::getline(now, b));
            if (!moreA && !moreB) break;
            if (!moreA || !moreB || a != b)
            {
                const std::string& l = moreB ? b : a;
                c.changed = l.substr(0, l.find('='));
                break;
            }
        }
        c.hit = c.changed.empty();
    }
    if (c.hit) return c;
    fs::remove_all(tree, ec);
    fs::create_directories(workDir, ec);
    std::ofstream(keyFile, std::ios::binary | std::ios::trunc) << key;
    return c;
}

void forgetBuildCache(const fs::path& workDir)
{
    std::error_code ec;
    fs::remove(workDir / "cache-key.txt", ec);
}

std::string installBuiltGame(const GameSdk& sdk, const fs::path& builtExe,
                             const fs::path& exportedExe, std::vector<std::string>* copied)
{
    std::error_code ec;
    if (exportedExe.empty()) return "the export shipped no game executable to replace";
    const fs::path dstDir = exportEngineDir(exportedExe);
    fs::copy_file(builtExe, exportedExe, fs::copy_options::overwrite_existing, ec);
    if (ec) return "cannot copy " + builtExe.string() + ": " + ec.message();
    if (copied) copied->push_back(exportedExe.filename().string());
    for (const char* sub : { "lib", "bin" })
    {
        const fs::path dir = sdk.root / sub / sdk.profile;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            // Symlinks (libSDL3.dylib -> libSDL3.0.dylib) are copied as the file
            // they name: the export folder holds plain files, like the prebuilt one.
            if (!e.is_regular_file(ec) || !isEngineLibrary(e.path())) continue;
            // The lib set carries the Python plugin built against it; it replaces
            // the prebuilt one, and goes nowhere a non-Python export left it out.
            if (e.path().filename().string().find("HorizonPython") != std::string::npos &&
                !fs::exists(dstDir / e.path().filename(), ec))
                continue;
            fs::copy_file(e.path(), dstDir / e.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
            if (ec) return "cannot copy " + e.path().string() + ": " + ec.message();
            if (copied) copied->push_back(e.path().filename().string());
        }
    }
    return {};
}
int relinkToBundledDeps(const fs::path& dir, const std::vector<std::string>& files)
{
    int rewritten = 0;
#if defined(__APPLE__)
    const auto q = [](const fs::path& p) { return "'" + p.string() + "'"; };
    for (const std::string& name : files)
    {
        const fs::path f = dir / name;
        FILE* pipe = popen(("otool -L " + q(f) + " 2>/dev/null").c_str(), "r");
        if (!pipe) continue;
        std::vector<std::string> deps;
        char buf[1024];
        while (std::fgets(buf, sizeof buf, pipe))
        {
            std::string line(buf);
            if (line.empty() || (line[0] != '\t' && line[0] != ' ')) continue;   // "<file>:" header
            line.erase(0, line.find_first_not_of(" \t"));
            line = line.substr(0, line.find(" ("));
            if (line.rfind("/opt/homebrew/", 0) == 0 || line.rfind("/usr/local/", 0) == 0)
                deps.push_back(line);
        }
        pclose(pipe);
        bool changed = false;
        for (const std::string& dep : deps)
        {
            std::string base = fs::path(dep).filename().string();
            // The plugin's CPython is a framework binary (…/Python.framework/
            // Versions/3.14/Python); the folder bundles it as libpython3.14.dylib
            // (scripts/bundle_native_deps.sh, bundled_name).
            if (const auto v = dep.find("/Python.framework/Versions/");
                v != std::string::npos && base == "Python")
            {
                const auto from = v + std::strlen("/Python.framework/Versions/");
                base = "libpython" + dep.substr(from, dep.find('/', from) - from) + ".dylib";
            }
            std::error_code ec;
            if (!fs::is_regular_file(dir / base, ec)) continue;
            const std::string cmd = "install_name_tool -change '" + dep + "' '@rpath/" + base + "' " +
                                    q(f) + " 2>/dev/null";
            if (std::system(cmd.c_str()) == 0) { ++rewritten; changed = true; }
        }
        if (changed)
            std::system(("codesign --force --sign - " + q(f) + " >/dev/null 2>&1").c_str());
    }
#else
    (void)dir; (void)files;
#endif
    return rewritten;
}
} // namespace HE::ExportSdk
