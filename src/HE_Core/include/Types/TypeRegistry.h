#pragma once
#include <Types/Defines.h>
#include <HorizonCode/HorizonCode.h>
#include <string>
#include <vector>

// ── User-defined types (struct + enum assets) ────────────────────────────────
// Struct and Enum assets let a project define its own data types once and use
// them everywhere: HorizonCode pins/variables, Lua/Python (tables/dicts +
// generated constants), generated C++ headers, and savegame-template fields.
// The definitions live in .hasset files (CHUNK_STDF / CHUNK_ENDF, JSON); this
// registry is the one process-global, language-neutral view of them. Keys are
// the definition asset's PROJECT-RELATIVE path — the same string a
// HorizonCode::Value carries in `typeName`.
//
// Population: the ContentManager registers a definition whenever it loads one
// of the two asset types (lazy loads included, so pak-mounted games fill the
// registry too); the editor additionally does an eager refresh at project open
// and on panel save so type dropdowns are complete before anything runs.

class ContentManager;   // global scope, like the rest of the ContentManager API

namespace HE {

struct EnumEntry
{
    std::string name;
    int         value = 0;
};

struct EnumDef
{
    std::string name;        // display name (asset filename stem)
    std::string assetPath;   // project-relative path — the registry key
    std::vector<EnumEntry> entries;

    // The entry list is authoritative for both directions; misses return the
    // fallback (first entry / empty string) so stale saved ints stay harmless.
    const EnumEntry* findEntry(const std::string& n) const;
    const EnumEntry* findValue(int v) const;
};

// One field of a user-defined struct. `type` is a HorizonCode pin type; for
// Enum/Struct fields `typeName` names the referenced definition asset (nested
// structs are allowed — cycles are rejected at registration).
struct StructField
{
    std::string          name;
    HorizonCode::PinType type = HorizonCode::PinType::Float;
    bool                 isArray = false;
    std::string          typeName;      // Enum/Struct fields: referenced def asset path
    // Seeds new instances. A scalar field holds one Value of `type`; an ARRAY
    // field uses the Value's own array payload (isArray + items), so a field can
    // ship authored starting elements instead of always beginning empty.
    HorizonCode::Value   defaultValue;
};

struct StructDef
{
    std::string name;        // display name (asset filename stem)
    std::string assetPath;   // project-relative path — the registry key
    std::vector<StructField> fields;

    const StructField* findField(const std::string& n) const;
};

// Process-global registry of every loaded struct/enum definition. Thread-safe
// (script bootstrap and codegen run off the main thread).
class HE_API TypeRegistry
{
public:
    static TypeRegistry& instance();

    // Upserts keyed by def.assetPath — always accepted, in any order (the
    // registry is dumb storage; a def that exists on disk is never invisible).
    // Validation happens at AUTHORING time: the struct panel refuses to save a
    // definition for which structWouldCycle() is true, and every recursive
    // consumer (makeDefaultValue, boundary conversion, codegen) carries a
    // visited-guard so a hand-edited cycle degrades instead of recursing.
    void registerEnum(EnumDef def);
    void registerStruct(StructDef def);
    void removeType(const std::string& assetPath);   // asset deleted
    void clear();                                    // project switch

    // Would registering `def` (under def.assetPath) close a reference cycle
    // through the currently registered structs?
    bool structWouldCycle(const StructDef& def) const;

    // Lookups by asset path (the typeName key). Copies out under the lock —
    // callers never hold pointers into the registry.
    bool getEnum(const std::string& assetPath, EnumDef& out) const;
    bool getStruct(const std::string& assetPath, StructDef& out) const;
    bool hasEnum(const std::string& assetPath) const;
    bool hasStruct(const std::string& assetPath) const;

    // Snapshots for dropdowns/bootstrap/codegen, sorted by display name.
    std::vector<EnumDef>   enums() const;
    std::vector<StructDef> structs() const;

    // True when another registered type (different assetPath) shares this
    // display name — the generated horizon.enums.<Name>/C++ symbols would
    // collide. Panels warn at save time.
    bool nameCollides(const std::string& name, const std::string& assetPath) const;

    // Build a struct VALUE seeded from the definition's field defaults (fields
    // in definition order, nested structs seeded recursively). Empty Value
    // (type Float) when the def is missing.
    HorizonCode::Value makeDefaultValue(const std::string& structAssetPath) const;

    // ── JSON round-trip (the CHUNK_STDF / CHUNK_ENDF payloads) ───────────────
    // Definitions persist name-keyed and type-named (never positional), so a
    // def edit can't silently shift persisted data.
    static std::string  enumToJson(const EnumDef& def);
    static bool         enumFromJson(const std::string& json, EnumDef& out);   // name/assetPath NOT in the payload
    static std::string  structToJson(const StructDef& def);
    static bool         structFromJson(const std::string& json, StructDef& out);

    // Load every Struct/Enum asset the manager can discover and register it —
    // the editor's project-open / panel-save refresh, and the game's startup
    // pass over the mounted pak. Returns the number of registered definitions.
    static size_t refreshFromContent(ContentManager& cm);

private:
    TypeRegistry() = default;
    struct Impl;
    Impl& impl() const;
};

} // namespace HE
