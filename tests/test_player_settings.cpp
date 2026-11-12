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

// ── Graphics quality as a player setting ─────────────────────────────────────
// The preset and the single values live in the same store as the volumes: a menu
// sets them, Save keeps them, reset hands the project's graphics back. What the
// application is told is the EFFECTIVE graphics: base, then preset, then values.

namespace {
struct GraphicsRecorder
{
    int calls = 0;
    HE::ProjectGraphics       graphics;
    HE::ProjectShadowSettings shadows;
    settings::Host host()
    {
        settings::Host h;
        h.baseGraphics.antiAliasing = 3;     // the project's own, so "base" is visible
        h.baseShadows.resolution    = 4096;
        h.applyQuality = [this](const HE::ProjectGraphics& g, const HE::ProjectShadowSettings& s)
        { ++calls; graphics = g; shadows = s; };
        return h;
    }
};
}

TEST_CASE("Player quality: a player who chose nothing leaves the application's graphics alone")
{
    Clean clean;
    GraphicsRecorder rec;
    settings::install(rec.host());
    CHECK(rec.calls == 0);                        // install did not push the base back
    CHECK(settings::qualityPreset() == "Custom"); // the project's values match no preset
    CHECK(settings::qualityValue("antiAliasing") == doctest::Approx(3.0f));   // read from the base
    CHECK(settings::qualityValue("shadowResolution") == doctest::Approx(4096.0f));
}

TEST_CASE("Player quality: a preset applies, a single value over it makes it Custom, a preset replaces the values")
{
    Clean clean;
    GraphicsRecorder rec;
    settings::install(rec.host());

    CHECK(settings::setQualityPreset("high"));    // case-insensitive
    CHECK(rec.calls == 1);
    CHECK(rec.graphics.enabled);
    CHECK(rec.graphics.textureQuality == 2);
    CHECK(rec.shadows.resolution == 2048);
    CHECK(settings::qualityPreset() == "High");
    CHECK(rec.graphics.preset == HE::QualityPreset::High);

    CHECK(settings::setQualityValue("textureQuality", 0.0f));
    CHECK(rec.graphics.textureQuality == 0);
    CHECK(rec.graphics.ssrEnabled);                // the rest is still High's
    CHECK(settings::qualityPreset() == "Custom");
    CHECK(settings::qualityValue("textureQuality") == doctest::Approx(0.0f));

    CHECK(settings::setQualityPreset("Low"));      // drops the single value
    CHECK(settings::values().quality.empty());
    CHECK(rec.graphics.textureQuality == 0);       // Low's own, not the leftover
    CHECK(rec.shadows.resolution == 1024);

    // Custom keeps what is there.
    const int before = rec.calls;
    CHECK(settings::setQualityPreset("Custom"));
    CHECK(rec.calls == before);
    CHECK(settings::qualityPreset() == "Low");
}

TEST_CASE("Player quality: single values are clamped and rounded, unknown names are refused")
{
    Clean clean;
    GraphicsRecorder rec;
    settings::install(rec.host());

    CHECK(settings::setQualityValue("textureQuality", 7.0f));
    CHECK(settings::qualityValue("textureQuality") == doctest::Approx(2.0f));   // clamped to High
    CHECK(settings::setQualityValue("giEnabled", 0.7f));
    CHECK(rec.graphics.giEnabled);                                             // bool: >= 0.5
    CHECK(settings::setQualityValue("shadowResolution", 3000.0f));
    CHECK(rec.shadows.resolution == 2048);                                      // nearer power of two
    CHECK(settings::setQualityValue("renderScale", 9.0f));
    CHECK(rec.graphics.renderScale == doctest::Approx(2.0f));

    const int calls = rec.calls;
    CHECK_FALSE(settings::setQualityValue("bloomWobble", 1.0f));
    CHECK_FALSE(settings::setQualityValue("textureQuality", std::nanf("")));
    CHECK_FALSE(settings::setQualityPreset("Insane"));
    CHECK(rec.calls == calls);
    CHECK(settings::qualityValue("bloomWobble") == doctest::Approx(0.0f));
}

TEST_CASE("Player quality: reset hands the project's graphics back, and the lists name every field")
{
    Clean clean;
    GraphicsRecorder rec;
    settings::install(rec.host());
    settings::setQualityPreset("Ultra");
    REQUIRE(rec.graphics.antiAliasing == 3);       // Ultra's TAA happens to equal this base; shadows tell
    CHECK(rec.shadows.resolution == 4096);
    settings::setQualityPreset("Low");
    CHECK(rec.shadows.resolution == 1024);

    settings::resetToDefaults();
    CHECK(rec.graphics.antiAliasing == 3);
    CHECK(rec.shadows.resolution == 4096);         // the project's again
    CHECK_FALSE(rec.graphics.enabled);
    CHECK(settings::qualityPreset() == "Custom");

    const auto names = settings::qualityPresetNames();
    REQUIRE(names.size() == 5);
    CHECK(names.front() == "Low");
    CHECK(names.back() == "Custom");
    const auto fields = settings::qualityFields();
    CHECK(fields.size() == static_cast<size_t>(HE::qualityFieldCount()));
    for (const std::string& f : fields)
    {
        INFO("field ", f);
        float v = 0.0f;
        CHECK(HE::getQualityField(rec.graphics, rec.shadows, f, v));
    }
}

TEST_CASE("Player quality: JSON round-trips the choice and drops what the engine does not know")
{
    Clean clean;
    settings::Values v;
    v.qualityPreset = "ultra";
    v.quality["textureQuality"] = 1.4f;            // rounds to 1
    v.quality["noSuchField"]    = 3.0f;
    settings::set(v);
    CHECK(settings::values().qualityPreset.value_or("") == "Ultra");   // canonical spelling
    CHECK(settings::values().quality.size() == 1);
    CHECK(settings::values().quality.at("textureQuality") == doctest::Approx(1.0f));

    settings::Values back;
    REQUIRE(settings::fromJson(settings::toJson(settings::values()), back));
    CHECK(back.qualityPreset.value_or("") == "Ultra");
    CHECK(back.quality.size() == 1);

    // A damaged file keeps the rest: wrong types are skipped, "Custom" is not stored.
    settings::Values odd;
    REQUIRE(settings::fromJson(
        R"({"qualityPreset":"Custom","quality":{"giEnabled":"yes","bloomEnabled":0},"vsync":false})", odd));
    CHECK_FALSE(odd.qualityPreset.has_value());
    CHECK(odd.quality.size() == 1);
    CHECK(odd.quality.count("bloomEnabled") == 1);
    CHECK(odd.vsync.value_or(true) == false);

    // Something chosen means the store is not empty, so save() will write it.
    CHECK(settings::toJson(settings::values()).find("qualityPreset") != std::string::npos);
}

TEST_CASE("Player quality: the registry rows reach the store")
{
    Clean clean;
    GraphicsRecorder rec;
    settings::install(rec.host());

    const auto ok = call("settings.setQualityPreset", { Value::ofString("Medium") });
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].b);
    CHECK(call("settings.qualityPreset")[0].s == "Medium");
    CHECK(call("settings.setQualityValue", { Value::ofString("giEnabled"), Value::ofFloat(1.0f) })[0].b);
    CHECK(call("settings.qualityValue", { Value::ofString("giEnabled") })[0].f == doctest::Approx(1.0f));
    CHECK(call("settings.qualityPreset")[0].s == "Custom");
    const auto names = call("settings.qualityPresetNames");
    REQUIRE(names.size() == 1);
    CHECK(names[0].isArray);
    CHECK(names[0].items.size() == 5);
    CHECK(call("settings.qualityFields")[0].items.size() == static_cast<size_t>(HE::qualityFieldCount()));
}
