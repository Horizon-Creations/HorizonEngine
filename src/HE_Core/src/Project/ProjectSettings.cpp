#include "Project/ProjectSettings.h"
#include "Diagnostics/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace HE {

namespace {

using json = nlohmann::json;

// Tolerant readers: a key that is missing or of the wrong type leaves the
// default in place. The file is hand-editable, and a typo in one number must
// not cost the other thirty.
template <class T>
void readNumber(const json& j, const char* key, T& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return;
    double v = it->get<double>();
    if (!std::isfinite(v)) return;
    // Into a range every T here can hold before the cast — a 1e30 in the file
    // is a mistake, not a licence for undefined behaviour. clamp() narrows it
    // the rest of the way.
    v   = std::clamp(v, -1.0e9, 1.0e9);
    out = static_cast<T>(v);
}

void readBool(const json& j, const char* key, bool& out)
{
    const auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) out = it->get<bool>();
}

void readString(const json& j, const char* key, std::string& out)
{
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) out = it->get<std::string>();
}

// A sub-object, or an empty one when the key is absent or not an object — so
// every reader below can run against it and land on its default.
const json& section(const json& j, const char* key)
{
    static const json kEmpty = json::object();
    const auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : kEmpty;
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) <= 1e-6f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

// The three spellings the runtime accepts, and only those: anything else in
// the file would become "Windowed" in the game with no error anywhere, so it
// is corrected HERE, where somebody can still see it.
const char* const kWindowModes[] = { "Windowed", "Fullscreen", "Borderless" };

int roundToPowerOfTwo(int v)
{
    int p = 1;
    while (p * 2 <= v) p *= 2;
    // Nearest, not floor: 3500 is closer to 4096 than to 2048 and somebody who
    // typed it meant the bigger one.
    return (v - p) < (p * 2 - v) ? p : p * 2;
}

} // namespace

// ─── ProjectSettings ──────────────────────────────────────────────────────────

bool ProjectSettings::isDefault() const
{
    return *this == ProjectSettings{};
}

bool ProjectSettings::operator==(const ProjectSettings& o) const
{
    return game.title == o.game.title
        && game.splashEnabled == o.game.splashEnabled
        && game.splashImage == o.game.splashImage
        && game.splashSubtitle == o.game.splashSubtitle
        && nearlyEqual(shadows.distance, o.shadows.distance)
        && shadows.cascadeCount == o.shadows.cascadeCount
        && shadows.resolution == o.shadows.resolution
        && nearlyEqual(shadows.splitLambda, o.shadows.splitLambda)
        && nearlyEqual(shadows.slopeBias, o.shadows.slopeBias)
        && nearlyEqual(shadows.minBias, o.shadows.minBias)
        && physics.fixedHz == o.physics.fixedHz
        && nearlyEqual(physics.gravity.x, o.physics.gravity.x)
        && nearlyEqual(physics.gravity.y, o.physics.gravity.y)
        && nearlyEqual(physics.gravity.z, o.physics.gravity.z)
        && renderDefaults.useEditorSettings == o.renderDefaults.useEditorSettings
        && renderDefaults.windowWidth == o.renderDefaults.windowWidth
        && renderDefaults.windowHeight == o.renderDefaults.windowHeight
        && renderDefaults.windowMode == o.renderDefaults.windowMode
        && renderDefaults.vsync == o.renderDefaults.vsync
        && renderDefaults.backend == o.renderDefaults.backend;
}

void ProjectSettings::clamp()
{
    auto& s = shadows;
    s.distance     = std::clamp(s.distance, 1.0f, ProjectShadowSettings::kMaxDistance);
    s.cascadeCount = std::clamp(s.cascadeCount, 1, ProjectShadowSettings::kMaxCascades);
    s.resolution   = roundToPowerOfTwo(std::clamp(s.resolution,
                                                  ProjectShadowSettings::kMinResolution,
                                                  ProjectShadowSettings::kMaxResolution));
    s.splitLambda  = std::clamp(s.splitLambda, 0.0f, 1.0f);
    s.slopeBias    = std::clamp(s.slopeBias, 0.0f, 0.1f);
    s.minBias      = std::clamp(s.minBias, 0.0f, 0.1f);

    auto& p = physics;
    p.fixedHz = std::clamp(p.fixedHz, ProjectPhysicsSettings::kMinHz, ProjectPhysicsSettings::kMaxHz);
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(p.gravity[i])) p.gravity[i] = 0.0f;
        p.gravity[i] = std::clamp(p.gravity[i], -1000.0f, 1000.0f);
    }

    auto& r = renderDefaults;
    r.windowWidth  = std::clamp(r.windowWidth,  ProjectRenderDefaults::kMinWindowEdge,
                                                ProjectRenderDefaults::kMaxWindowEdge);
    r.windowHeight = std::clamp(r.windowHeight, ProjectRenderDefaults::kMinWindowEdge,
                                                ProjectRenderDefaults::kMaxWindowEdge);
    if (std::find(std::begin(kWindowModes), std::end(kWindowModes), r.windowMode)
        == std::end(kWindowModes))
        r.windowMode = ProjectRenderDefaults{}.windowMode;
}

void ProjectSettings::toJson(json& out) const
{
    out = json::object();
    out["version"] = kVersion;

    out["game"] = { { "title",          game.title },
                    { "splashEnabled",  game.splashEnabled },
                    { "splashImage",    game.splashImage },
                    { "splashSubtitle", game.splashSubtitle } };

    out["shadows"] = {
        { "distance",     shadows.distance },
        { "cascadeCount", shadows.cascadeCount },
        { "resolution",   shadows.resolution },
        { "splitLambda",  shadows.splitLambda },
        { "slopeBias",    shadows.slopeBias },
        { "minBias",      shadows.minBias },
    };

    out["physics"] = {
        { "fixedHz", physics.fixedHz },
        { "gravity", { physics.gravity.x, physics.gravity.y, physics.gravity.z } },
    };

    out["renderDefaults"] = {
        { "useEditorSettings", renderDefaults.useEditorSettings },
        { "windowWidth",       renderDefaults.windowWidth },
        { "windowHeight",      renderDefaults.windowHeight },
        { "windowMode",        renderDefaults.windowMode },
        { "vsync",             renderDefaults.vsync },
        { "backend",           renderDefaults.backend },
    };
}

void ProjectSettings::fromJson(const json& in)
{
    *this = ProjectSettings{};
    if (!in.is_object()) return;

    {
        const json& g = section(in, "game");
        readString(g, "title",          game.title);
        readBool  (g, "splashEnabled",  game.splashEnabled);
        readString(g, "splashImage",    game.splashImage);
        readString(g, "splashSubtitle", game.splashSubtitle);
    }
    {
        const json& s = section(in, "shadows");
        readNumber(s, "distance",     shadows.distance);
        readNumber(s, "cascadeCount", shadows.cascadeCount);
        readNumber(s, "resolution",   shadows.resolution);
        readNumber(s, "splitLambda",  shadows.splitLambda);
        readNumber(s, "slopeBias",    shadows.slopeBias);
        readNumber(s, "minBias",      shadows.minBias);
    }
    {
        const json& p = section(in, "physics");
        readNumber(p, "fixedHz", physics.fixedHz);
        const auto g = p.find("gravity");
        if (g != p.end() && g->is_array() && g->size() == 3)
        {
            glm::vec3 v = physics.gravity;
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i)
            {
                if (!(*g)[i].is_number()) { ok = false; break; }
                v[i] = (*g)[i].get<float>();
            }
            if (ok) physics.gravity = v;
        }
    }
    {
        const json& r = section(in, "renderDefaults");
        readBool  (r, "useEditorSettings", renderDefaults.useEditorSettings);
        readNumber(r, "windowWidth",       renderDefaults.windowWidth);
        readNumber(r, "windowHeight",      renderDefaults.windowHeight);
        readString(r, "windowMode",        renderDefaults.windowMode);
        readBool  (r, "vsync",             renderDefaults.vsync);
        readString(r, "backend",           renderDefaults.backend);
    }

    clamp();
}

// ─── The file ─────────────────────────────────────────────────────────────────

std::filesystem::path projectSettingsPath(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Config" / "ProjectSettings.json";
}

bool loadProjectSettings(const std::filesystem::path& projectRoot, ProjectSettings& out)
{
    const std::filesystem::path path = projectSettingsPath(projectRoot);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        out = ProjectSettings{};
        return true;
    }

    std::ifstream in(path);
    if (!in.is_open())
    {
        HE_LOG_ERROR(Config, "Project settings: '%s' exists but cannot be read",
                     path.string().c_str());
        return false;
    }
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        HE_LOG_ERROR(Config, "Project settings: '%s' is not a JSON object — left untouched",
                     path.string().c_str());
        return false;
    }
    out.fromJson(j);
    return true;
}

bool saveProjectSettings(const std::filesystem::path& projectRoot, const ProjectSettings& settings)
{
    const std::filesystem::path path = projectSettingsPath(projectRoot);
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (!exists && settings.isDefault())
        return true;   // nothing to say, and no file to say it in

    std::filesystem::create_directories(path.parent_path(), ec);

    json j;
    settings.toJson(j);

    // Temp + rename, as the .heproj does: an in-place truncate would leave an
    // empty file behind a crash mid-write, and this one is read at every open.
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open())
        {
            HE_LOG_ERROR(Config, "Project settings: cannot create '%s'", tmp.string().c_str());
            return false;
        }
        out << j.dump(4, ' ', false, json::error_handler_t::replace) << '\n';
        out.flush();
        if (!out.good())
        {
            out.close();
            std::filesystem::remove(tmp, ec);
            HE_LOG_ERROR(Config, "Project settings: write to '%s' failed — the previous file is intact",
                         tmp.string().c_str());
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        HE_LOG_ERROR(Config, "Project settings: could not replace '%s' (%s)",
                     path.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

} // namespace HE
