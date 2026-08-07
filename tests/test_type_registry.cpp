#include "doctest.h"
#include <Types/TypeRegistry.h>

// HE::TypeRegistry — user-defined struct/enum definitions: JSON round-trip,
// upsert/remove semantics, cycle detection, name collisions, seeded defaults.
// The registry is process-global, so every case cleans up the paths it touches
// (other suites in this binary may rely on an empty registry).

using HE::TypeRegistry;
using HE::EnumDef;
using HE::EnumEntry;
using HE::StructDef;
using HE::StructField;
using HorizonCode::PinType;
using HorizonCode::Value;

namespace
{
struct RegistryCleanup
{
    std::vector<std::string> paths;
    ~RegistryCleanup()
    {
        for (const auto& p : paths) TypeRegistry::instance().removeType(p);
    }
};
} // namespace

TEST_CASE("TypeRegistry: enum JSON round-trips entries by name and value")
{
    EnumDef def;
    def.name = "Weapon"; def.assetPath = "Content/Weapon.hasset";
    def.entries = { { "Sword", 0 }, { "Bow", 1 }, { "Staff", 5 } };

    const std::string json = TypeRegistry::enumToJson(def);
    EnumDef back;
    REQUIRE(TypeRegistry::enumFromJson(json, back));
    REQUIRE(back.entries.size() == 3);
    CHECK(back.entries[2].name == "Staff");
    CHECK(back.entries[2].value == 5);
    CHECK(back.findEntry("Bow")->value == 1);
    CHECK(back.findValue(5)->name == "Staff");
    CHECK(back.findEntry("Axe") == nullptr);

    // Malformed payloads are rejected, never fatal.
    EnumDef junk;
    CHECK(!TypeRegistry::enumFromJson("not json{", junk));
    CHECK(!TypeRegistry::enumFromJson("[1,2,3]", junk));
}

TEST_CASE("TypeRegistry: struct JSON round-trips typed fields and defaults")
{
    StructDef def;
    def.name = "PlayerStats"; def.assetPath = "Content/PlayerStats.hasset";
    {
        StructField hp; hp.name = "hp"; hp.type = PinType::Float;
        hp.defaultValue = Value::ofFloat(100.0f);
        StructField title; title.name = "title"; title.type = PinType::String;
        title.defaultValue = Value::ofString("Rookie");
        StructField hardcore; hardcore.name = "hardcore"; hardcore.type = PinType::Bool;
        hardcore.defaultValue = Value::ofBool(true);
        StructField tags; tags.name = "tags"; tags.type = PinType::String; tags.isArray = true;
        StructField weapon; weapon.name = "weapon"; weapon.type = PinType::Enum;
        weapon.typeName = "Content/Weapon.hasset"; weapon.defaultValue.s = "Bow";
        def.fields = { hp, title, hardcore, tags, weapon };
    }

    const std::string json = TypeRegistry::structToJson(def);
    StructDef back;
    REQUIRE(TypeRegistry::structFromJson(json, back));
    REQUIRE(back.fields.size() == 5);
    CHECK(back.fields[0].defaultValue.f == doctest::Approx(100.0f));
    CHECK(back.fields[1].defaultValue.s == "Rookie");
    CHECK(back.fields[2].defaultValue.b == true);
    CHECK(back.fields[3].isArray);
    CHECK(back.fields[4].type == PinType::Enum);
    CHECK(back.fields[4].typeName == "Content/Weapon.hasset");
    CHECK(back.fields[4].defaultValue.s == "Bow");
    CHECK(back.findField("hp") != nullptr);
    CHECK(back.findField("mp") == nullptr);
}

TEST_CASE("TypeRegistry: upsert, remove and name collisions")
{
    RegistryCleanup cleanup;
    cleanup.paths = { "Content/A/Stats.hasset", "Content/B/Stats.hasset" };
    auto& reg = TypeRegistry::instance();

    StructDef a; a.name = "Stats"; a.assetPath = "Content/A/Stats.hasset";
    reg.registerStruct(a);
    CHECK(reg.hasStruct(a.assetPath));
    CHECK(!reg.nameCollides("Stats", a.assetPath));  // only itself

    EnumDef b; b.name = "Stats"; b.assetPath = "Content/B/Stats.hasset";
    reg.registerEnum(b);
    CHECK(reg.nameCollides("Stats", a.assetPath));   // now the enum collides

    // Upsert replaces in place.
    a.fields.push_back({ "hp", PinType::Float });
    reg.registerStruct(a);
    StructDef got;
    REQUIRE(reg.getStruct(a.assetPath, got));
    CHECK(got.fields.size() == 1);

    reg.removeType(a.assetPath);
    CHECK(!reg.hasStruct(a.assetPath));
    CHECK(!reg.nameCollides("Stats", b.assetPath));  // struct gone → enum stands alone
}

TEST_CASE("TypeRegistry: cycle detection through nested struct references")
{
    RegistryCleanup cleanup;
    cleanup.paths = { "Content/A.hasset", "Content/B.hasset", "Content/C.hasset" };
    auto& reg = TypeRegistry::instance();

    // A → B (registered first, no cycle)
    StructDef b; b.name = "B"; b.assetPath = "Content/B.hasset";
    reg.registerStruct(b);
    StructDef a; a.name = "A"; a.assetPath = "Content/A.hasset";
    a.fields.push_back({ "child", PinType::Struct, false, "Content/B.hasset" });
    CHECK(!reg.structWouldCycle(a));
    reg.registerStruct(a);

    // Direct self-reference.
    StructDef selfRef; selfRef.name = "C"; selfRef.assetPath = "Content/C.hasset";
    selfRef.fields.push_back({ "me", PinType::Struct, false, "Content/C.hasset" });
    CHECK(reg.structWouldCycle(selfRef));

    // Editing B to point back at A closes A → B → A.
    StructDef bEdited = b;
    bEdited.fields.push_back({ "parent", PinType::Struct, false, "Content/A.hasset" });
    CHECK(reg.structWouldCycle(bEdited));

    // An enum reference is not a struct edge — no cycle.
    StructDef bEnum = b;
    bEnum.fields.push_back({ "kind", PinType::Enum, false, "Content/A.hasset" });
    CHECK(!reg.structWouldCycle(bEnum));
}

TEST_CASE("TypeRegistry: makeDefaultValue seeds nested defaults and guards cycles")
{
    RegistryCleanup cleanup;
    cleanup.paths = { "Content/Weapon.hasset", "Content/Inner.hasset", "Content/Outer.hasset" };
    auto& reg = TypeRegistry::instance();

    EnumDef weapon; weapon.name = "Weapon"; weapon.assetPath = "Content/Weapon.hasset";
    weapon.entries = { { "Sword", 0 }, { "Bow", 7 } };
    reg.registerEnum(weapon);

    StructDef inner; inner.name = "Inner"; inner.assetPath = "Content/Inner.hasset";
    {
        StructField f; f.name = "speed"; f.type = PinType::Float;
        f.defaultValue = Value::ofFloat(3.5f);
        inner.fields.push_back(f);
    }
    reg.registerStruct(inner);

    StructDef outer; outer.name = "Outer"; outer.assetPath = "Content/Outer.hasset";
    {
        StructField w; w.name = "weapon"; w.type = PinType::Enum;
        w.typeName = "Content/Weapon.hasset"; w.defaultValue.s = "Bow";
        StructField n; n.name = "inner"; n.type = PinType::Struct;
        n.typeName = "Content/Inner.hasset";
        StructField arr; arr.name = "log"; arr.type = PinType::String; arr.isArray = true;
        outer.fields = { w, n, arr };
    }
    reg.registerStruct(outer);

    const Value v = reg.makeDefaultValue("Content/Outer.hasset");
    CHECK(v.type == PinType::Struct);
    CHECK(v.typeName == "Content/Outer.hasset");
    REQUIRE(v.items.size() == 3);
    CHECK(v.items[0].type == PinType::Enum);
    CHECK(v.items[0].i == 7);                       // "Bow" resolved by name
    CHECK(v.items[1].type == PinType::Struct);
    REQUIRE(v.items[1].items.size() == 1);
    CHECK(v.items[1].items[0].f == doctest::Approx(3.5f));
    CHECK(v.items[2].isArray);
    CHECK(v.items[2].items.empty());                // arrays start empty

    // A hand-edited cycle (bypassing the panel's save check) degrades to an
    // empty nested struct instead of recursing forever.
    StructDef evil = inner;
    evil.fields.push_back({ "outer", PinType::Struct, false, "Content/Outer.hasset" });
    reg.registerStruct(evil);
    const Value v2 = reg.makeDefaultValue("Content/Outer.hasset");
    REQUIRE(v2.items.size() == 3);
    REQUIRE(v2.items[1].items.size() == 2);
    CHECK(v2.items[1].items[1].items.empty());      // the cycle edge stops descending

    // Unknown struct → empty struct value, not a crash.
    const Value unknown = reg.makeDefaultValue("Content/Nope.hasset");
    CHECK(unknown.type == PinType::Struct);
    CHECK(unknown.items.empty());
}
