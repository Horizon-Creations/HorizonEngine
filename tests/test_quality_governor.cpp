// Automatic quality: the controller (HE::QualityGovernor) on lists of frame times, its
// place in the player's settings store (project values -> the governor's level -> the
// player's preset -> the player's single values), and the project's switch. The
// controller has no clock of its own, so every case here is a loop over numbers.
#include "doctest.h"

#include <HorizonScene/EngineApi.h>
#include <Project/ProjectSettings.h>
#include <Project/QualityGovernor.h>
#include <Types/TypeRegistry.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using HE::QualityGovernor;
using HE::QualityGovernorConfig;

namespace {
constexpr float kFast = 1.0f / 60.0f;   // meets a 60 fps target exactly
constexpr float kSlow = 1.0f / 30.0f;   // misses it by a lot

// Feeds `seconds` of frames of `dt` each and returns the times (seconds since the call
// began) at which the level changed.
std::vector<float> run(QualityGovernor& g, float seconds, float dt)
{
    std::vector<float> changes;
    float t = 0.0f;
    while (t < seconds)
    {
        t += dt;
        if (g.update(dt)) changes.push_back(t);
    }
    return changes;
}
} // namespace

TEST_CASE("Governor: meeting the target at the top level changes nothing, ever")
{
    QualityGovernor g;
    g.configure({}, 3);
    CHECK(run(g, 120.0f, kFast).empty());
    CHECK(g.level() == 3);
}

TEST_CASE("Governor: meeting the target for 15 s tries one level up, not before")
{
    QualityGovernor g;
    g.configure({}, 2);
    CHECK(run(g, 14.0f, kFast).empty());
    CHECK(g.level() == 2);
    run(g, 2.0f, kFast);
    CHECK(g.level() == 3);
}

TEST_CASE("Governor: missing the target steps down after 2 s, then measures the new level for 3 s first")
{
    QualityGovernor g;
    g.configure({}, 3);
    const std::vector<float> steps = run(g, 20.0f, kSlow);
    REQUIRE(steps.size() == 3);                 // 3 -> 2 -> 1 -> 0, then the floor holds
    CHECK(steps[0] == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(steps[1] == doctest::Approx(7.0f).epsilon(0.03));    // 2 s miss + 3 s cooldown + 2 s miss
    CHECK(steps[2] == doctest::Approx(12.0f).epsilon(0.03));
    CHECK(g.level() == 0);
    CHECK(run(g, 60.0f, kSlow).empty());        // nothing below Low
}

TEST_CASE("Governor: one long frame is not a measurement, and a short stall costs no quality")
{
    QualityGovernor g;
    g.configure({}, 3);
    run(g, 3.0f, kFast);
    CHECK_FALSE(g.update(2.5f));                // a scene load
    CHECK_FALSE(g.update(0.5f));                // exactly the hitch bound: ignored too
    for (int i = 0; i < 20; ++i)                // a long game with a 0.3 s hitch every 5 s
    {
        run(g, 5.0f, kFast);
        g.update(0.3f);
    }
    CHECK(g.level() == 3);
}

TEST_CASE("Governor: bad frame times are ignored")
{
    QualityGovernor g;
    g.configure({}, 2);
    CHECK_FALSE(g.update(0.0f));
    CHECK_FALSE(g.update(-1.0f));
    CHECK_FALSE(g.update(std::numeric_limits<float>::quiet_NaN()));
    CHECK(g.smoothedFrameTime() == 0.0f);
    CHECK(g.level() == 2);
}

TEST_CASE("Governor: a level that failed is not tried again for a minute")
{
    QualityGovernor g;
    g.configure({}, 3);
    run(g, 3.0f, kSlow);                        // 3 -> 2 at about 2 s
    REQUIRE(g.level() == 2);
    // Frames now meet the target: it would go back up after 15 s, but level 3 just failed.
    run(g, 55.0f, kFast);                       // about 58 s in
    CHECK(g.level() == 2);
    run(g, 8.0f, kFast);                        // past 62 s
    CHECK(g.level() == 3);
}

TEST_CASE("Governor: the limits are kept, and the start is clamped into them")
{
    QualityGovernorConfig cfg;
    cfg.minLevel = 1;
    cfg.maxLevel = 2;
    QualityGovernor g;
    g.configure(cfg, 3);
    CHECK(g.level() == 2);
    run(g, 60.0f, kSlow);
    CHECK(g.level() == 1);
    // After the failed-level block has run out, a long fast stretch is capped at max.
    run(g, 90.0f, kFast);
    CHECK(g.level() == 2);
    run(g, 60.0f, kFast);
    CHECK(g.level() == 2);

    // A nonsense config is repaired, not obeyed.
    QualityGovernorConfig bad;
    bad.minLevel = 9; bad.maxLevel = -4; bad.targetFps = 0.0f;
    g.configure(bad, 99);
    CHECK(g.config().minLevel == 3);
    CHECK(g.config().maxLevel == 3);
    CHECK(g.config().targetFps >= 10.0f);
    CHECK(g.level() == 3);
}

TEST_CASE("Governor: sitting on the edge of the target neither lowers nor raises")
{
    QualityGovernor g;
    g.configure({}, 2);
    // 8 % over the target: above "meeting" (5 %), below "missing" (12 %).
    CHECK(run(g, 120.0f, kFast * 1.08f).empty());
    CHECK(g.level() == 2);
}

TEST_CASE("Governor: the target frame rate is the project's, not 60")
{
    QualityGovernorConfig cfg;
    cfg.targetFps = 30.0f;
    QualityGovernor g;
    g.configure(cfg, 3);
    CHECK(run(g, 30.0f, kSlow).empty());        // 30 fps meets a 30 fps target
    cfg.targetFps = 120.0f;
    g.configure(cfg, 3);
    CHECK_FALSE(run(g, 10.0f, kFast).empty());  // 60 fps misses a 120 fps one
}

// ── In the player's settings store ───────────────────────────────────────────

namespace S = HE::api::settings;

namespace {
struct Clean
{
    Clean()  { S::uninstall(); S::resetToDefaults(); }
    ~Clean() { S::uninstall(); S::resetToDefaults(); }
};
struct Recorder
{
    int calls = 0;
    HE::ProjectGraphics graphics;
    HE::ProjectShadowSettings shadows;
    S::Host host(bool available = true, bool projectOn = true)
    {
        S::Host h;
        h.autoQualityAvailable = available;
        h.baseAuto.enabled     = projectOn;
        h.baseAuto.targetFps   = 60.0f;
        h.applyQuality = [this](const HE::ProjectGraphics& g, const HE::ProjectShadowSettings& s)
        { ++calls; graphics = g; shadows = s; };
        return h;
    }
};
void feed(float seconds, float dt)
{
    for (float t = 0.0f; t < seconds; t += dt) S::tickAutoQuality(dt);
}
constexpr int kLowL = 0, kMediumL = 1, kHighL = 2, kCustomL = 4;
} // namespace

TEST_CASE("Auto quality: the project's switch starts the controller at the project's overall level")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host());
    CHECK(S::autoQuality());
    CHECK(S::autoQualityLevel() == kHighL);          // the project's values match no preset: High
    CHECK(S::qualityPreset() == "High");
    CHECK(rec.calls >= 1);                           // the host was told at install
    CHECK(rec.graphics.preset == HE::QualityPreset::High);

    feed(2.3f, kSlow);                               // misses the target
    CHECK(S::autoQualityLevel() == kMediumL);
    CHECK(S::qualityPreset() == "Medium");
    CHECK(rec.graphics.preset == HE::QualityPreset::Medium);
    CHECK(rec.shadows.resolution == 2048);
    feed(6.0f, kSlow);
    CHECK(S::autoQualityLevel() == kLowL);
    CHECK(rec.shadows.resolution == 1024);
}

TEST_CASE("Auto quality: off in the project means off until the player turns it on")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host(true, /*projectOn=*/false));
    CHECK_FALSE(S::autoQuality());
    CHECK(S::autoQualityLevel() == kCustomL);
    const int calls = rec.calls;
    feed(10.0f, kSlow);
    CHECK(rec.calls == calls);                       // nothing moves, nobody is told

    CHECK(S::setAutoQuality(true));
    CHECK(S::autoQuality());
    feed(2.3f, kSlow);
    CHECK(S::autoQualityLevel() == kMediumL);
    CHECK(S::setAutoQuality(false));
    CHECK_FALSE(S::autoQuality());
}

TEST_CASE("Auto quality: a preset the player picks ends it, and turning it back on drops that preset")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host());
    REQUIRE(S::autoQuality());

    CHECK(S::setQualityPreset("Ultra"));
    CHECK_FALSE(S::autoQuality());                   // a fixed preset and automation exclude each other
    CHECK(S::qualityPreset() == "Ultra");
    const int calls = rec.calls;
    feed(10.0f, kSlow);
    CHECK(rec.calls == calls);                       // the frames no longer matter
    CHECK(S::qualityPreset() == "Ultra");

    CHECK(S::setAutoQuality(true));
    CHECK(S::autoQuality());
    CHECK_FALSE(S::values().qualityPreset.has_value());   // automation owns the overall level again
    CHECK(S::autoQualityLevel() == kHighL);               // restarted from the project's
    CHECK(S::qualityPreset() == "High");
}

TEST_CASE("Auto quality: an area the player pinned stays pinned while the level moves")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host());
    CHECK(S::setQualityValue("textureQuality", 0.0f));    // a single value, not a preset
    CHECK(S::autoQuality());                              // so automation continues
    feed(2.3f, kSlow);
    CHECK(S::autoQualityLevel() == kMediumL);
    CHECK(rec.graphics.textureQuality == 0);              // Medium's would be 1: the pin wins
    CHECK(rec.graphics.renderScale == doctest::Approx(1.0f));   // the rest follows Medium
    feed(6.0f, kSlow);
    CHECK(S::autoQualityLevel() == kLowL);
    CHECK(rec.graphics.textureQuality == 0);
    CHECK(rec.graphics.renderScale == doctest::Approx(0.75f));  // Low's
}

TEST_CASE("Auto quality: where the application cannot run it, it refuses and does nothing")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host(/*available=*/false, /*projectOn=*/true));   // the editor
    CHECK_FALSE(S::autoQuality());                   // the project's switch is ignored
    CHECK(S::autoQualityLevel() == kCustomL);
    CHECK_FALSE(S::setAutoQuality(true));
    CHECK_FALSE(S::autoQuality());
    const int calls = rec.calls;
    feed(10.0f, kSlow);
    CHECK(rec.calls == calls);
}

TEST_CASE("Auto quality: the switch is saved, the level is not, and a reset goes back to the project's")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host(true, /*projectOn=*/false));
    CHECK(S::setAutoQuality(true));
    feed(2.3f, kSlow);
    REQUIRE(S::autoQualityLevel() == kMediumL);

    const std::string json = S::toJson(S::values());
    CHECK(json.find("\"autoQuality\":true") != std::string::npos);
    CHECK(json.find("qualityPreset") == std::string::npos);   // the governor's level is not a choice
    S::Values back;
    REQUIRE(S::fromJson(json, back));
    CHECK(back.autoQuality.value_or(false));

    S::resetToDefaults();                              // the project said off
    CHECK_FALSE(S::autoQuality());

    // A player with only this switch set is still a player who chose something.
    S::Values only;
    only.autoQuality = true;
    S::set(only);
    CHECK(S::autoQuality());
    CHECK(S::autoQualityLevel() == kHighL);            // a fresh start, not the old Medium
}

TEST_CASE("Auto quality: the registry rows, with the level as the engine's enum")
{
    Clean clean;
    Recorder rec;
    S::install(rec.host(true, false));
    using HorizonCode::Value;
    const HE::api::ApiFn* get   = HE::api::find("settings.autoQuality");
    const HE::api::ApiFn* set   = HE::api::find("settings.setAutoQuality");
    const HE::api::ApiFn* level = HE::api::find("settings.autoQualityLevel");
    REQUIRE(get); REQUIRE(set); REQUIRE(level);
    REQUIRE(level->results.size() == 1);
    CHECK(level->results[0].type == HorizonCode::PinType::Enum);
    CHECK(std::string(level->results[0].typeName) == HE::kQualityLevelEnum);

    HE::api::Ctx ctx;
    CHECK_FALSE(get->invoke(ctx, {})[0].b);
    CHECK(set->invoke(ctx, { Value::ofBool(true) })[0].b);
    CHECK(get->invoke(ctx, {})[0].b);
    const Value lv = level->invoke(ctx, {})[0];
    CHECK(lv.type == HorizonCode::PinType::Enum);
    CHECK(lv.i == kHighL);
    CHECK(lv.typeName == HE::kQualityLevelEnum);
}

// ── The project's block ──────────────────────────────────────────────────────

TEST_CASE("Project auto quality: off by default, round-trips, and is held to its ranges")
{
    const HE::ProjectSettings def;
    CHECK_FALSE(def.autoQuality.enabled);
    CHECK(def.autoQuality.targetFps == doctest::Approx(60.0f));
    CHECK(def.isDefault());

    // A file from before the block existed loads as off.
    HE::ProjectSettings old;
    old.fromJson(nlohmann::json{ { "graphics", { { "enabled", true } } } });
    CHECK_FALSE(old.autoQuality.enabled);

    HE::ProjectSettings a;
    a.autoQuality.enabled   = true;
    a.autoQuality.targetFps = 144.0f;
    a.autoQuality.minLevel  = 1;
    a.autoQuality.maxLevel  = 2;
    nlohmann::json j;
    a.toJson(j);
    REQUIRE(j.contains("autoQuality"));
    CHECK(j["autoQuality"]["targetFps"].get<float>() == doctest::Approx(144.0f));
    HE::ProjectSettings b;
    b.fromJson(j);
    CHECK(a == b);
    HE::ProjectSettings c = a;
    c.autoQuality.targetFps = 143.0f;
    CHECK(a != c);

    HE::ProjectSettings wild;
    wild.fromJson(nlohmann::json{ { "autoQuality", { { "enabled", true }, { "targetFps", 5000.0 },
                                                      { "minLevel", 3 }, { "maxLevel", 0 } } } });
    CHECK(wild.autoQuality.targetFps == doctest::Approx(1000.0f));
    CHECK(wild.autoQuality.minLevel == 3);
    CHECK(wild.autoQuality.maxLevel == 3);          // never below the minimum
    wild.fromJson(nlohmann::json{ { "autoQuality", { { "targetFps", 2.0 } } } });
    CHECK(wild.autoQuality.targetFps == doctest::Approx(10.0f));
}
