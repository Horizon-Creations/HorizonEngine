// The player's settings (Thema 85, Schritt 5): the store behind a settings
// menu — deadzone, stick look, VSync, fullscreen, volume — its hooks into the
// application, and its persistence through prefs.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/EngineApi.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace HE::api;
using HorizonCode::Value;

namespace {

// What an application would do with the hooks, written down instead of done.
struct Recorder
{
    float dz = -1.0f;
    int   vsyncCalls = 0, fullCalls = 0;
    bool  vsync = true, full = false;
    std::vector<std::string> volumes;   // "bus value" / "bus project"
    std::map<std::string, float> live{ { settings::kMaster, 1.0f }, { "Music", 0.8f } };

    settings::Host host()
    {
        settings::Host h;
        h.stickDeadzone = 0.15f;
        h.vsync         = true;
        h.fullscreen    = false;
        h.applyStickDeadzone = [this](float v) { dz = v; };
        h.applyVSync         = [this](bool on) { ++vsyncCalls; vsync = on; };
        h.applyFullscreen    = [this](bool on) { ++fullCalls; full = on; };
        h.applyVolume = [this](const std::string& bus, std::optional<float> v)
        {
            volumes.push_back(bus + (v ? " " + std::to_string(*v) : std::string(" project")));
            live[bus] = v ? *v : (bus == "Music" ? 0.8f : 1.0f);
        };
        h.currentVolume = [this](const std::string& bus)
        {
            auto it = live.find(bus);
            return it != live.end() ? it->second : 1.0f;
        };
        return h;
    }
};

// Start from nothing and leave nothing: the store is process-wide, and the
// camera rig reads it every frame in every other test.
struct Clean
{
    Clean()  { settings::uninstall(); settings::resetToDefaults(); }
    ~Clean() { settings::uninstall(); settings::resetToDefaults(); }
};

std::vector<Value> call(const char* id, const std::vector<Value>& args = {})
{
    const ApiFn* fn = find(id);
    REQUIRE_MESSAGE(fn != nullptr, id);
    Ctx ctx;
    return fn->invoke(ctx, args);
}

} // namespace

TEST_CASE("Player settings: nothing chosen answers the application's values")
{
    Clean clean;
    // No host at all (edit mode, a test): the engine's own defaults.
    CHECK(input::stickDeadzone() == doctest::Approx(0.15f));
    CHECK(camera::stickSensitivityScale() == doctest::Approx(1.0f));
    CHECK_FALSE(camera::stickInvertY());
    CHECK(app::vsync());
    CHECK_FALSE(app::isFullscreen());
    CHECK(settings::volume("Music") == doctest::Approx(1.0f));

    // A host: its base values, and install applies them once.
    Recorder rec;
    settings::Host h = rec.host();
    h.stickDeadzone = 0.2f;
    h.vsync         = false;
    h.fullscreen    = true;
    settings::install(std::move(h));
    CHECK(rec.dz == doctest::Approx(0.2f));
    CHECK(rec.vsyncCalls == 1);
    CHECK_FALSE(rec.vsync);
    CHECK(rec.fullCalls == 1);
    CHECK(rec.full);
    CHECK(rec.volumes.empty());   // no bus the player set, nothing to hand back
    CHECK(input::stickDeadzone() == doctest::Approx(0.2f));
    CHECK_FALSE(app::vsync());
    CHECK(app::isFullscreen());
    CHECK(settings::volume("Music") == doctest::Approx(0.8f));   // the live bus
}

TEST_CASE("Player settings: the rows apply at once, clamp, and refuse NaN")
{
    Clean clean;
    Recorder rec;
    settings::install(rec.host());

    call("input.setStickDeadzone", { Value::ofFloat(0.3f) });
    CHECK(rec.dz == doctest::Approx(0.3f));
    CHECK(call("input.stickDeadzone")[0].f == doctest::Approx(0.3f));
    call("input.setStickDeadzone", { Value::ofFloat(4.0f) });
    CHECK(rec.dz == doctest::Approx(0.9f));
    call("input.setStickDeadzone", { Value::ofFloat(-1.0f) });
    CHECK(rec.dz == doctest::Approx(0.0f));
    input::setStickDeadzone(std::nanf(""));
    CHECK(input::stickDeadzone() == doctest::Approx(0.0f));   // unchanged

    call("camera.setStickSensitivityScale", { Value::ofFloat(0.0f) });
    CHECK(call("camera.stickSensitivityScale")[0].f == doctest::Approx(0.05f));
    call("camera.setStickSensitivityScale", { Value::ofFloat(100.0f) });
    CHECK(camera::stickSensitivityScale() == doctest::Approx(10.0f));
    call("camera.setStickInvertY", { Value::ofBool(true) });
    CHECK(call("camera.stickInvertY")[0].b);

    call("app.setVSync", { Value::ofBool(false) });
    CHECK_FALSE(rec.vsync);
    CHECK_FALSE(call("app.vsync")[0].b);
    call("app.setFullscreen", { Value::ofBool(true) });
    CHECK(rec.full);
    CHECK(call("app.isFullscreen")[0].b);

    call("settings.setVolume", { Value::ofString("Music"), Value::ofFloat(0.5f) });
    call("settings.setVolume", { Value::ofString(""), Value::ofFloat(3.0f) });   // "" = master, clamped
    REQUIRE(rec.volumes.size() == 2);
    CHECK(rec.volumes[0] == "Music " + std::to_string(0.5f));
    CHECK(rec.volumes[1] == std::string(settings::kMaster) + " " + std::to_string(2.0f));
    CHECK(call("settings.volume", { Value::ofString("Music") })[0].f == doctest::Approx(0.5f));
    CHECK(call("settings.volume", { Value::ofString("Master") })[0].f == doctest::Approx(2.0f));
    // Only the bus that changed is touched — a bus a script turned down stays.
    rec.volumes.clear();
    settings::setVolume("Music", 0.25f);
    REQUIRE(rec.volumes.size() == 1);
    CHECK(rec.volumes[0] == "Music " + std::to_string(0.25f));
}

TEST_CASE("Player settings: reset goes back to the project's values and hands the buses back")
{
    Clean clean;
    Recorder rec;
    settings::install(rec.host());
    input::setStickDeadzone(0.4f);
    app::setVSync(false);
    app::setFullscreen(true);
    camera::setStickSensitivityScale(3.0f);
    settings::setVolume("Music", 0.1f);
    rec.volumes.clear();

    call("settings.resetToDefaults");
    CHECK(rec.dz == doctest::Approx(0.15f));
    CHECK(rec.vsync);
    CHECK_FALSE(rec.full);
    CHECK(camera::stickSensitivityScale() == doctest::Approx(1.0f));
    REQUIRE(rec.volumes.size() == 1);
    CHECK(rec.volumes[0] == "Music project");
    CHECK(settings::volume("Music") == doctest::Approx(0.8f));   // the project's again
    CHECK_FALSE(settings::values().stickDeadzone.has_value());
}

TEST_CASE("Player settings: JSON keeps only what was chosen and survives damage")
{
    Clean clean;
    settings::Values v;
    v.stickDeadzone = 0.2f;
    v.stickInvertY  = true;
    v.volumes["SFX"] = 0.7f;
    const std::string json = settings::toJson(v);
    const nlohmann::json j = nlohmann::json::parse(json);
    CHECK(j.size() == 3);   // deadzone, invert, volumes — nothing unchosen
    CHECK_FALSE(j.contains("vsync"));

    settings::Values back;
    REQUIRE(settings::fromJson(json, back));
    CHECK(back.stickDeadzone.value_or(-1.0f) == doctest::Approx(0.2f));
    CHECK(back.stickInvertY.value_or(false));
    CHECK_FALSE(back.vsync.has_value());
    CHECK(back.volumes.at("SFX") == doctest::Approx(0.7f));

    // A hand-edited file: wrong types skipped, out of range clamped, the rest kept.
    REQUIRE(settings::fromJson(
        R"({"stickDeadzone":"high","stickSensitivityScale":50,"vsync":false,)"
        R"("fullscreen":1,"volumes":{"Music":-3,"":0.5,"Voice":"loud"}})", back));
    CHECK_FALSE(back.stickDeadzone.has_value());
    CHECK(back.stickSensitivityScale.value_or(0.0f) == doctest::Approx(10.0f));
    CHECK_FALSE(back.vsync.value_or(true));
    CHECK_FALSE(back.fullscreen.has_value());
    REQUIRE(back.volumes.size() == 1);
    CHECK(back.volumes.at("Music") == doctest::Approx(0.0f));

    CHECK(settings::fromJson("", back));
    CHECK_FALSE(settings::fromJson("[1,2]", back));
    CHECK_FALSE(settings::fromJson("{not json", back));
}

TEST_CASE("Player settings: save writes one prefs key, load brings it back, empty removes it")
{
    Clean clean;
    const auto sandbox = std::filesystem::temp_directory_path() / "he_test_player_settings_prefs";
    he_test::removeAllQuiet(sandbox);

    // Nowhere to write: false, and nothing pretends it worked.
    fs::setSandboxRoot("");
    input::setStickDeadzone(0.3f);
    CHECK_FALSE(call("settings.save")[0].b);

    fs::setSandboxRoot(sandbox.string());
    Ctx ctx;
    prefs::remove(ctx, settings::kPrefsKey);
    app::setVSync(false);
    settings::setVolume("Music", 0.4f);
    REQUIRE(call("settings.save")[0].b);
    REQUIRE(prefs::has(ctx, settings::kPrefsKey));
    // On disk, not only in memory: the next start reads the file.
    const std::string onDisk = fs::readText("Prefs.json");
    CHECK(onDisk.find("\"settings\"") != std::string::npos);

    // A new session: the store forgets, the host installs, load restores and
    // applies through it — the order both applications use.
    settings::resetToDefaults();
    Recorder rec;
    settings::install(rec.host());
    CHECK(rec.vsync);                              // nothing loaded yet
    REQUIRE(settings::load());
    CHECK(input::stickDeadzone() == doctest::Approx(0.3f));
    CHECK(rec.dz == doctest::Approx(0.3f));
    CHECK_FALSE(rec.vsync);
    CHECK(settings::volume("Music") == doctest::Approx(0.4f));
    REQUIRE(!rec.volumes.empty());
    CHECK(rec.volumes.back() == "Music " + std::to_string(0.4f));

    // Unsaved changes do not survive a reload…
    input::setStickDeadzone(0.6f);
    settings::load();
    CHECK(input::stickDeadzone() == doctest::Approx(0.3f));

    // …and saving nothing takes the key away rather than storing "{}".
    settings::resetToDefaults();
    REQUIRE(settings::save());
    CHECK_FALSE(prefs::has(ctx, settings::kPrefsKey));

    // A damaged value: load says so and starts from the project's.
    prefs::setString(ctx, settings::kPrefsKey, "not an object");
    CHECK_FALSE(settings::load());
    CHECK_FALSE(settings::values().vsync.has_value());

    prefs::remove(ctx, settings::kPrefsKey);
    fs::setSandboxRoot("");
    he_test::removeAllQuiet(sandbox);
}

TEST_CASE("Player settings: uninstall drops the hooks but keeps the values")
{
    Clean clean;
    Recorder rec;
    settings::install(rec.host());
    CHECK(settings::installed());
    settings::uninstall();
    CHECK_FALSE(settings::installed());

    input::setStickDeadzone(0.5f);                // no hook, no crash
    CHECK(rec.dz == doctest::Approx(0.15f));      // still install's value
    CHECK(input::stickDeadzone() == doctest::Approx(0.5f));
    // The camera half keeps working without any host.
    camera::setStickSensitivityScale(2.0f);
    CHECK(settings::values().stickSensitivityScale.value_or(0.0f) == doctest::Approx(2.0f));
}
