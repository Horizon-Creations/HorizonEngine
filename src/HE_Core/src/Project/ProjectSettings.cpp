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

// A policy as the file spells it: the names of the set bits, in bit order, so
// two saves of the same policy are the same text.
json policyToJson(std::uint32_t mask)
{
    json arr = json::array();
    for (int i = 0; i < ProjectAntiCheatSettings::kResponseCount; ++i)
        if (mask & (1u << i)) arr.push_back(ProjectAntiCheatSettings::kResponseNames[i]);
    return arr;
}

// Missing or not an array = the default stays. An array is taken WHOLE, so
// `[]` really is "nothing but log" (§5.3) and not "the default again"; a
// name the engine does not know is skipped, a name it does is set. Log is put
// back by clamp() whatever the file said.
void readPolicy(const json& j, const char* key, std::uint32_t& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    std::uint32_t mask = 0;
    for (const json& e : *it)
    {
        if (!e.is_string()) continue;
        const std::string s = e.get<std::string>();
        for (int i = 0; i < ProjectAntiCheatSettings::kResponseCount; ++i)
            if (s == ProjectAntiCheatSettings::kResponseNames[i]) mask |= 1u << i;
    }
    out = mask;
}

bool rulesEqual(const std::vector<ProjectAntiCheatRule>& a,
                const std::vector<ProjectAntiCheatRule>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].name != b[i].name || a[i].level != b[i].level) return false;
        if (!nearlyEqual(a[i].min, b[i].min) || !nearlyEqual(a[i].max, b[i].max)
            || !nearlyEqual(a[i].maxPerSecond, b[i].maxPerSecond))
            return false;
    }
    return true;
}

} // namespace

// ─── ProjectAntiCheatRule ─────────────────────────────────────────────────────

int ProjectAntiCheatRule::levelIndex() const
{
    for (int i = 0; i < kLevelCount; ++i)
        if (level == kLevels[i]) return i;
    return 0;
}

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
        && renderDefaults.backend == o.renderDefaults.backend
        && antiCheat.enabled == o.antiCheat.enabled
        && antiCheat.integrityCheck == o.antiCheat.integrityCheck
        && nearlyEqual(antiCheat.tolerance, o.antiCheat.tolerance)
        && nearlyEqual(antiCheat.windowSec, o.antiCheat.windowSec)
        && antiCheat.maxInputsPerSecond == o.antiCheat.maxInputsPerSecond
        && nearlyEqual(antiCheat.scoreHalfLifeSec, o.antiCheat.scoreHalfLifeSec)
        && nearlyEqual(antiCheat.scoreSuspect, o.antiCheat.scoreSuspect)
        && nearlyEqual(antiCheat.scoreConfirmed, o.antiCheat.scoreConfirmed)
        && antiCheat.policySuspect == o.antiCheat.policySuspect
        && antiCheat.policyConfirmed == o.antiCheat.policyConfirmed
        && antiCheat.policyHard == o.antiCheat.policyHard
        && antiCheat.telemetryUrl == o.antiCheat.telemetryUrl
        && rulesEqual(antiCheat.rules, o.antiCheat.rules);
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

    using AC = ProjectAntiCheatSettings;
    auto& a = antiCheat;
    auto finiteOr = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    a.tolerance          = std::clamp(finiteOr(a.tolerance, AC{}.tolerance), 0.0f, AC::kMaxTolerance);
    a.windowSec          = std::clamp(finiteOr(a.windowSec, AC{}.windowSec),
                                      AC::kMinWindowSec, AC::kMaxWindowSec);
    a.maxInputsPerSecond = std::clamp(a.maxInputsPerSecond,
                                      AC::kMinInputsPerSecond, AC::kMaxInputsPerSecond);
    a.scoreHalfLifeSec   = std::clamp(finiteOr(a.scoreHalfLifeSec, AC{}.scoreHalfLifeSec),
                                      AC::kMinHalfLifeSec, AC::kMaxHalfLifeSec);
    a.scoreSuspect       = std::clamp(finiteOr(a.scoreSuspect, AC{}.scoreSuspect),
                                      0.0f, AC::kMaxScoreThreshold);
    // Confirmed below Suspect would make Suspect unreachable: the score would
    // jump straight past it. Confirmed is lifted, not Suspect lowered — the
    // one somebody edited last is the one that has to give way, and a raised
    // Suspect is the more common edit.
    a.scoreConfirmed     = std::clamp(finiteOr(a.scoreConfirmed, AC{}.scoreConfirmed),
                                      a.scoreSuspect, AC::kMaxScoreThreshold);
    // Log is not optional (§5.2): a level change is always a line in the log.
    a.policySuspect   = (a.policySuspect   & AC::AllResponses) | AC::Log;
    a.policyConfirmed = (a.policyConfirmed & AC::AllResponses) | AC::Log;
    a.policyHard      = (a.policyHard      & AC::AllResponses) | AC::Log;

    if (a.rules.size() > static_cast<std::size_t>(AC::kMaxRules))
        a.rules.resize(static_cast<std::size_t>(AC::kMaxRules));
    for (ProjectAntiCheatRule& rule : a.rules)
    {
        // An empty name is left alone: the page adds a row before it has a
        // name and saves the moment the first edit ends, so a clamp that
        // dropped nameless rules would delete the row under the user's cursor.
        // The runtime skips a rule it cannot address by name.
        rule.min          = std::clamp(finiteOr(rule.min, 0.0f), -AC::kMaxRuleValue, AC::kMaxRuleValue);
        rule.max          = std::clamp(finiteOr(rule.max, 0.0f), rule.min, AC::kMaxRuleValue);
        rule.maxPerSecond = std::clamp(finiteOr(rule.maxPerSecond, 0.0f), 0.0f, AC::kMaxRuleValue);
        rule.level        = ProjectAntiCheatRule::kLevels[rule.levelIndex()];
    }
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

    // The shape docs/anti-cheat-plan.md §4.4 shows, key for key.
    json rules = json::array();
    for (const ProjectAntiCheatRule& r : antiCheat.rules)
        rules.push_back({ { "name",         r.name },
                          { "min",          r.min },
                          { "max",          r.max },
                          { "maxPerSecond", r.maxPerSecond },
                          { "level",        r.level } });
    out["anticheat"] = {
        { "enabled",            antiCheat.enabled },
        { "integrityCheck",     antiCheat.integrityCheck },
        { "tolerance",          antiCheat.tolerance },
        { "windowSec",          antiCheat.windowSec },
        { "maxInputsPerSecond", antiCheat.maxInputsPerSecond },
        { "score", { { "halfLifeSec", antiCheat.scoreHalfLifeSec },
                     { "suspect",     antiCheat.scoreSuspect },
                     { "confirmed",   antiCheat.scoreConfirmed } } },
        { "policy", { { "suspect",   policyToJson(antiCheat.policySuspect) },
                      { "confirmed", policyToJson(antiCheat.policyConfirmed) },
                      { "hard",      policyToJson(antiCheat.policyHard) } } },
        { "telemetryUrl",       antiCheat.telemetryUrl },
        { "rules",              std::move(rules) },
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
    {
        const json& a = section(in, "anticheat");
        readBool  (a, "enabled",            antiCheat.enabled);
        readBool  (a, "integrityCheck",     antiCheat.integrityCheck);
        readNumber(a, "tolerance",          antiCheat.tolerance);
        readNumber(a, "windowSec",          antiCheat.windowSec);
        readNumber(a, "maxInputsPerSecond", antiCheat.maxInputsPerSecond);
        const json& sc = section(a, "score");
        readNumber(sc, "halfLifeSec", antiCheat.scoreHalfLifeSec);
        readNumber(sc, "suspect",     antiCheat.scoreSuspect);
        readNumber(sc, "confirmed",   antiCheat.scoreConfirmed);
        const json& po = section(a, "policy");
        readPolicy(po, "suspect",   antiCheat.policySuspect);
        readPolicy(po, "confirmed", antiCheat.policyConfirmed);
        readPolicy(po, "hard",      antiCheat.policyHard);
        readString(a, "telemetryUrl", antiCheat.telemetryUrl);
        // A rules key that is there and an array is taken whole — an empty one
        // is "no rules", not "the default's rules" (which is also none). An
        // element that is not an object is skipped, not turned into a blank
        // rule; a rule with a missing field gets that field's default.
        const auto rules = a.find("rules");
        if (rules != a.end() && rules->is_array())
        {
            antiCheat.rules.clear();
            for (const json& e : *rules)
            {
                if (!e.is_object()) continue;
                ProjectAntiCheatRule rule;
                readString(e, "name",         rule.name);
                readNumber(e, "min",          rule.min);
                readNumber(e, "max",          rule.max);
                readNumber(e, "maxPerSecond", rule.maxPerSecond);
                readString(e, "level",        rule.level);
                antiCheat.rules.push_back(std::move(rule));
            }
        }
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
