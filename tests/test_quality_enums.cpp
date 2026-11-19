// The engine's own enums, QualityLevel and QualitySetting (Types/TypeRegistry.h):
// what a HorizonCode graph switches on and a script reads as horizon.enums.*.
// Their numbers are part of a contract — a graph that saved "Medium" saved 1, and
// the quality rows (HE::QualityPreset) use the same values — so they are pinned
// here, together with the rule that keeps a project from changing them.
#include "doctest.h"

#include <HorizonScene/EngineApi.h>
#include <Project/ProjectSettings.h>
#include <Types/TypeRegistry.h>

#include <string>

using HE::TypeRegistry;

TEST_CASE("Built-in enums: QualityLevel and QualitySetting are there from the first call")
{
    TypeRegistry& reg = TypeRegistry::instance();
    HE::EnumDef level, setting;
    REQUIRE(reg.getEnum(HE::kQualityLevelEnum, level));
    REQUIRE(reg.getEnum(HE::kQualitySettingEnum, setting));
    CHECK(level.name == "QualityLevel");
    CHECK(setting.name == "QualitySetting");

    // The numbers are HE::QualityPreset's, entry by entry.
    REQUIRE(level.entries.size() == static_cast<size_t>(HE::kQualityPresetCount));
    for (int i = 0; i < HE::kQualityPresetCount; ++i)
    {
        INFO("entry ", i);
        CHECK(level.entries[i].value == i);
        CHECK(level.entries[i].name == HE::qualityPresetName(static_cast<HE::QualityPreset>(i)));
    }

    // Every area a quality row can mean, in one place, numbered from 0.
    const char* const kAreas[] = { "Overall", "Shadows", "AntiAliasing", "RenderResolution",
                                   "AmbientOcclusion", "Reflections", "GlobalIllumination",
                                   "RayTracedReflections", "Textures", "Materials" };
    REQUIRE(setting.entries.size() == std::size(kAreas));
    for (size_t i = 0; i < std::size(kAreas); ++i)
    {
        CHECK(setting.entries[i].name == kAreas[i]);
        CHECK(setting.entries[i].value == static_cast<int>(i));
    }

    // They are what the editor's pickers and a script's horizon.enums list.
    bool inPicker = false;
    for (const HE::EnumDef& d : reg.enums()) inPicker |= d.assetPath == HE::kQualityLevelEnum;
    CHECK(inPicker);
}

TEST_CASE("Built-in enums: a project switch keeps them, and a project cannot change or delete them")
{
    TypeRegistry& reg = TypeRegistry::instance();

    // A project's own enum, and the engine's, side by side.
    HE::EnumDef mine;
    mine.name = "Mood"; mine.assetPath = "Types/Mood.hasset";
    mine.entries = { { "Calm", 0, {} } };
    reg.registerEnum(mine);
    CHECK(reg.hasEnum("Types/Mood.hasset"));

    reg.clear();   // the project switch
    CHECK_FALSE(reg.hasEnum("Types/Mood.hasset"));
    CHECK(reg.hasEnum(HE::kQualityLevelEnum));
    CHECK(reg.hasEnum(HE::kQualitySettingEnum));

    // Writing under the engine's key is ignored, not an upsert.
    HE::EnumDef imposter;
    imposter.name = "QualityLevel"; imposter.assetPath = HE::kQualityLevelEnum;
    imposter.entries = { { "Only", 7, {} } };
    const uint64_t before = reg.generation();
    reg.registerEnum(imposter);
    reg.removeType(HE::kQualityLevelEnum);
    CHECK(reg.generation() == before);   // nothing was written
    HE::EnumDef level;
    REQUIRE(reg.getEnum(HE::kQualityLevelEnum, level));
    CHECK(level.entries.size() == 5);
    CHECK(level.entries[1].name == "Medium");

    CHECK(TypeRegistry::isBuiltin(HE::kQualityLevelEnum));
    CHECK_FALSE(TypeRegistry::isBuiltin("Types/Mood.hasset"));
    CHECK_FALSE(TypeRegistry::isBuiltin(""));
}

TEST_CASE("Built-in enums: a project enum with the same display name is flagged, not merged")
{
    TypeRegistry& reg = TypeRegistry::instance();
    reg.clear();
    HE::EnumDef mine;
    mine.name = "QualityLevel"; mine.assetPath = "Types/QualityLevel.hasset";
    mine.entries = { { "Good", 0, {} }, { "Bad", 1, {} } };
    reg.registerEnum(mine);

    // Both exist, under their own keys; the generated symbols would collide, which
    // is exactly what the enum panel's warning is for.
    CHECK(reg.hasEnum("Types/QualityLevel.hasset"));
    CHECK(reg.hasEnum(HE::kQualityLevelEnum));
    CHECK(reg.nameCollides("QualityLevel", "Types/QualityLevel.hasset"));
    reg.clear();   // leave the process the way the first test found it
}

// ── One level per area ───────────────────────────────────────────────────────
using HE::QualityLevel;
using HE::QualitySetting;
using HorizonCode::Value;

namespace {
constexpr int kLow = 0, kMedium = 1, kHigh = 2, kUltra = 3, kCustom = 4;
int areaIndex(QualitySetting s) { return static_cast<int>(s); }
QualityLevel levelOf(QualitySetting s, const HE::ProjectSettings& p)
{ return HE::qualityLevelOf(s, p.graphics, p.shadows); }

struct Clean
{
    Clean()  { HE::api::settings::uninstall(); HE::api::settings::resetToDefaults(); }
    ~Clean() { HE::api::settings::uninstall(); HE::api::settings::resetToDefaults(); }
};
struct Recorder
{
    HE::ProjectGraphics graphics; HE::ProjectShadowSettings shadows; int calls = 0;
    HE::api::settings::Host host()
    {
        HE::api::settings::Host h;
        h.applyQuality = [this](const HE::ProjectGraphics& g, const HE::ProjectShadowSettings& s)
        { ++calls; graphics = g; shadows = s; };
        return h;
    }
};
} // namespace

TEST_CASE("Quality levels: with a preset applied every area reads as that preset")
{
    for (int p = kLow; p <= kUltra; ++p)
    {
        HE::ProjectSettings s;
        HE::applyQualityPreset(static_cast<HE::QualityPreset>(p), s.graphics, s.shadows);
        INFO("preset ", p);
        CHECK(static_cast<int>(levelOf(QualitySetting::Overall, s)) == p);
        // A tie between rows goes to the row the whole configuration reads as, so a
        // High project shows High everywhere even where Medium has the same value.
        for (int a = 1; a < HE::kQualitySettingCount; ++a)
        {
            INFO("area ", a);
            CHECK(static_cast<int>(levelOf(static_cast<QualitySetting>(a), s)) == p);
        }
    }
}

TEST_CASE("Quality levels: ties between rows, hand-tuned values and the Custom answer")
{
    // The project's own defaults are no preset: Overall reads Custom, and each area
    // reads the LOWEST row it equals. SMAA is Medium's and High's AA, so Medium; the
    // default texture quality 2 is High's and Ultra's, so High.
    HE::ProjectSettings s;
    s.graphics.antiAliasing = 2;
    CHECK(levelOf(QualitySetting::Overall, s) == QualityLevel::Custom);
    CHECK(levelOf(QualitySetting::AntiAliasing, s) == QualityLevel::Medium);
    CHECK(levelOf(QualitySetting::Textures, s) == QualityLevel::High);

    // A value no preset has is Custom, and only for ITS area.
    s.graphics.antiAliasing = 0;      // Off: no row
    s.shadows.resolution = 512;       // no row
    CHECK(levelOf(QualitySetting::AntiAliasing, s) == QualityLevel::Custom);
    CHECK(levelOf(QualitySetting::Shadows, s) == QualityLevel::Custom);
    CHECK(levelOf(QualitySetting::Textures, s) == QualityLevel::High);

    // An area with several fields needs ALL of them: shadows at Medium's resolution
    // and cascades but not its distance is no row.
    HE::ProjectSettings m;
    HE::applyQualityPreset(HE::QualityPreset::Medium, m.graphics, m.shadows);
    CHECK(levelOf(QualitySetting::Shadows, m) == QualityLevel::Medium);
    m.shadows.distance = 321.0f;
    CHECK(levelOf(QualitySetting::Shadows, m) == QualityLevel::Custom);
}

TEST_CASE("Quality levels: setting one area writes only its fields")
{
    HE::ProjectSettings s;
    HE::applyQualityPreset(HE::QualityPreset::Ultra, s.graphics, s.shadows);
    const HE::ProjectGraphics before = s.graphics;

    REQUIRE(HE::setQualityLevelOf(QualitySetting::Textures, QualityLevel::Low, s.graphics, s.shadows));
    CHECK(s.graphics.textureQuality == 0);
    CHECK(s.graphics.antiAliasing == before.antiAliasing);       // the rest is still Ultra's
    CHECK(s.graphics.giEnabled == before.giEnabled);
    CHECK(s.shadows.resolution == 4096);
    CHECK(levelOf(QualitySetting::Textures, s) == QualityLevel::Low);
    CHECK(levelOf(QualitySetting::Overall, s) == QualityLevel::Custom);   // no longer a preset

    REQUIRE(HE::setQualityLevelOf(QualitySetting::Shadows, QualityLevel::Medium, s.graphics, s.shadows));
    CHECK(s.shadows.resolution == 2048);
    CHECK(s.shadows.cascadeCount == 3);
    CHECK(s.shadows.distance == doctest::Approx(200.0f));

    REQUIRE(HE::setQualityLevelOf(QualitySetting::RayTracedReflections, QualityLevel::Low, s.graphics, s.shadows));
    CHECK_FALSE(s.graphics.giReflectionsEnabled);
    CHECK(s.graphics.giReflBounces == 1);

    // Overall is the preset; Custom keeps what is there; nonsense is refused.
    REQUIRE(HE::setQualityLevelOf(QualitySetting::Overall, QualityLevel::Medium, s.graphics, s.shadows));
    CHECK(HE::detectQualityPreset(s.graphics, s.shadows) == HE::QualityPreset::Medium);
    const HE::ProjectGraphics kept = s.graphics;
    CHECK(HE::setQualityLevelOf(QualitySetting::Textures, QualityLevel::Custom, s.graphics, s.shadows));
    CHECK(s.graphics.textureQuality == kept.textureQuality);
    CHECK_FALSE(HE::setQualityLevelOf(QualitySetting::Textures, static_cast<QualityLevel>(9), s.graphics, s.shadows));
    CHECK_FALSE(HE::setQualityLevelOf(static_cast<QualitySetting>(42), QualityLevel::Low, s.graphics, s.shadows));

    // The fields each area is made of exist in the name table, so the player store
    // can keep them one by one.
    for (int a = 1; a < HE::kQualitySettingCount; ++a)
    {
        const auto fields = HE::qualityAreaFields(static_cast<QualitySetting>(a));
        INFO("area ", HE::qualitySettingName(static_cast<QualitySetting>(a)));
        CHECK_FALSE(fields.empty());
        for (const std::string& f : fields)
        {
            float v = 0.0f;
            CHECK_MESSAGE(HE::getQualityField(s.graphics, s.shadows, f, v), f);
        }
    }
    CHECK(HE::qualityAreaFields(QualitySetting::Overall).empty());
}

TEST_CASE("Quality levels: the settings rows work in levels, over the player store")
{
    Clean clean;
    Recorder rec;
    HE::api::settings::install(rec.host());
    namespace S = HE::api::settings;

    // The project's own values: textures read High (the lowest row with value 2).
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Textures)) == kHigh);

    CHECK(S::setQualityLevel(areaIndex(QualitySetting::Textures), kLow));
    CHECK(rec.graphics.textureQuality == 0);
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Textures)) == kLow);
    // Shadows are untouched: the project's 2048 / 3 cascades / 250 m is exactly High's row.
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Shadows)) == kHigh);

    // Three texture levels: asking for Ultra stores High's value, so it reads High.
    CHECK(S::setQualityLevel(areaIndex(QualitySetting::Textures), kUltra));
    CHECK(rec.graphics.textureQuality == 2);
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Textures)) == kHigh);

    // Overall is the preset; the answer for Overall is the preset name's level.
    CHECK(S::setQualityLevel(areaIndex(QualitySetting::Overall), kHigh));
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Overall)) == kHigh);
    CHECK(S::qualityPreset() == "High");
    CHECK(S::setQualityLevel(areaIndex(QualitySetting::AntiAliasing), kUltra));
    CHECK(rec.graphics.antiAliasing == 3);
    CHECK(S::qualityLevel(areaIndex(QualitySetting::Overall)) == kCustom);   // one area moved off High

    // Custom keeps; bad input is refused and changes nothing.
    const int calls = rec.calls;
    CHECK(S::setQualityLevel(areaIndex(QualitySetting::Textures), kCustom));
    CHECK_FALSE(S::setQualityLevel(99, kLow));
    CHECK_FALSE(S::setQualityLevel(areaIndex(QualitySetting::Textures), 9));
    CHECK(S::qualityLevel(99) == kCustom);
    CHECK(rec.calls == calls);

    // The settings object: one fixed reference that is never a script instance's id,
    // and the Of rows are the same two rows with it in front (0 works as well).
    const uint32_t eng = S::engineSettings();
    CHECK(eng == S::kEngineSettingsRef);
    CHECK(eng != 0u);
    CHECK(S::setQualityOf(eng, areaIndex(QualitySetting::Reflections), kUltra));
    CHECK(rec.graphics.ssrEnabled);
    CHECK(S::qualityOf(eng, areaIndex(QualitySetting::Reflections)) == kUltra);
    CHECK(S::qualityOf(0u, areaIndex(QualitySetting::Reflections)) == kUltra);

    // And it all goes back to the project's values with the rest of the store.
    S::resetToDefaults();
    CHECK(rec.graphics.textureQuality == 2);
    CHECK_FALSE(rec.graphics.ssrEnabled);
}

TEST_CASE("Quality levels: the registry rows carry the enum types on their pins and run on them")
{
    Clean clean;
    Recorder rec;
    HE::api::settings::install(rec.host());

    const HE::api::ApiFn* get = HE::api::find("settings.qualityLevel");
    const HE::api::ApiFn* set = HE::api::find("settings.setQualityOf");
    const HE::api::ApiFn* obj = HE::api::find("settings.engine");
    REQUIRE(get); REQUIRE(set); REQUIRE(obj);

    // The pins: Enum, and which enum — the editor draws them in its name and a Switch
    // on Enum connects to them.
    REQUIRE(get->params.size() == 1);
    CHECK(get->params[0].type == HorizonCode::PinType::Enum);
    CHECK(std::string(get->params[0].typeName) == HE::kQualitySettingEnum);
    REQUIRE(get->results.size() == 1);
    CHECK(get->results[0].type == HorizonCode::PinType::Enum);
    CHECK(std::string(get->results[0].typeName) == HE::kQualityLevelEnum);
    CHECK(obj->results[0].type == HorizonCode::PinType::Ref);
    REQUIRE(set->params.size() == 3);
    CHECK(set->params[0].type == HorizonCode::PinType::Ref);
    // The node pin built from it keeps the key (toFuncParam is the one place that does).
    const HorizonCode::FuncParam fp = HE::api::toFuncParam(get->results[0]);
    CHECK(fp.typeName == HE::kQualityLevelEnum);
    CHECK(HE::api::toFuncParam(obj->results[0]).typeName.empty());

    HE::api::Ctx ctx;
    const Value ref = obj->invoke(ctx, {})[0];
    CHECK(ref.ref == HE::api::settings::kEngineSettingsRef);
    auto asEnum = [](int v, const char* tn) { Value e = Value::ofInt(v); e.type = HorizonCode::PinType::Enum; e.typeName = tn; return e; };
    const auto ok = set->invoke(ctx, { ref, asEnum(areaIndex(QualitySetting::Textures), HE::kQualitySettingEnum),
                                       asEnum(kLow, HE::kQualityLevelEnum) });
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].b);
    const auto level = get->invoke(ctx, { asEnum(areaIndex(QualitySetting::Textures), HE::kQualitySettingEnum) });
    REQUIRE(level.size() == 1);
    CHECK(level[0].type == HorizonCode::PinType::Enum);
    CHECK(level[0].i == kLow);
    CHECK(level[0].typeName == HE::kQualityLevelEnum);   // the result says which enum it is
}
