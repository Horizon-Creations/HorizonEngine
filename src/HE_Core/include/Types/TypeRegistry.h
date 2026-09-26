#pragma once
#include <Types/Defines.h>
#include <HorizonCode/HorizonCode.h>
#include <functional>
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

// ── Renames (formerNames) ────────────────────────────────────────────────────
// Fields and entries have no stable ids — everything persisted about them is
// keyed by NAME (save files, Variable::structDefaults, enum defaults in graph
// JSON). A rename therefore books the old name as an ALIAS, and every reader
// that misses the current name falls back to it, so data written before the
// rename still finds its field. Oldest first; a chain a→b→c keeps both.
//
// One rule decides every ambiguity: a LIVE name always wins. An alias that is
// also the current name of another field/entry in the same definition is
// ignored (and not persisted), so "rename x→y, then add a new x" hands old x
// data to the new x, not to y.

// Book a rename into `formerNames`: `oldName` joins it, `newName` leaves it
// (renaming back must not leave the field aliasing itself). No-op when the two
// are equal or `oldName` is empty.
HE_API void noteRename(std::vector<std::string>& formerNames,
                       const std::string& oldName, const std::string& newName);

struct EnumEntry
{
    std::string name;
    int         value = 0;
    std::vector<std::string> formerNames;   // see "Renames" above
};

// HE_API on the struct (not just the two out-of-line methods below): findEntry/
// findValue are defined in TypeRegistry.cpp, so without it MSVC never exports
// their symbols from HorizonCore.dll and any other DLL calling them (HcCodegen,
// EngineApi — cross-module by design) fails to link with LNK2019. Trivial
// members-only structs elsewhere (HE::UUID, EnumEntry) don't need this because
// every one of their functions is inline; this one isn't.
struct HE_API EnumDef
{
    std::string name;        // display name (asset filename stem)
    std::string assetPath;   // project-relative path — the registry key
    std::vector<EnumEntry> entries;

    // The entry list is authoritative for both directions; misses return the
    // fallback (first entry / empty string) so stale saved ints stay harmless.
    // findEntry matches the current names first, then former names — an entry
    // name written before a rename still resolves to the renamed entry.
    const EnumEntry* findEntry(const std::string& n) const;
    const EnumEntry* findValue(int v) const;
    bool isLiveName(const std::string& n) const;
};

// One field of a user-defined struct. `type` is a HorizonCode pin type; for
// Enum/Struct fields `typeName` names the referenced definition asset (nested
// structs are allowed — cycles are rejected at registration).
struct StructField
{
    std::string          name;
    HorizonCode::PinType type = HorizonCode::PinType::Float;
    bool                 isArray = false;   // the field holds a CONTAINER of `type`
    std::string          typeName;      // Enum/Struct fields: referenced def asset path
    // Seeds new instances. A scalar field holds one Value of `type`; a CONTAINER
    // field uses the Value's own container payload (isArray + items, plus `keys`
    // for a map), so a field can ship authored starting elements instead of
    // always beginning empty.
    HorizonCode::Value   defaultValue;
    // Which container (HorizonCode::ContainerKind) — None + isArray reads as
    // Array, which is every field declared before Set/Map existed. For a MAP,
    // `type`/`typeName` are the VALUE side and these name the key.
    //
    // A container field is also how a container NESTS: a pin carries one element
    // type and one kind, so Map<string, Array<int>> is not expressible directly,
    // but Map<string, Loadout> with an array field on Loadout is.
    HorizonCode::ContainerKind container = HorizonCode::ContainerKind::None;
    HorizonCode::PinType       keyType = HorizonCode::PinType::String;
    std::string                keyTypeName;
    std::vector<std::string>   formerNames;   // see "Renames" above

    HorizonCode::ContainerKind kind() const
    { return HorizonCode::containerKindOf(isArray, container); }
};

// HE_API for the same reason as EnumDef above: findField is out-of-line.
struct HE_API StructDef
{
    std::string name;        // display name (asset filename stem)
    std::string assetPath;   // project-relative path — the registry key
    std::vector<StructField> fields;

    // Current names first, then former names (see "Renames" above).
    const StructField* findField(const std::string& n) const;
    bool isLiveName(const std::string& n) const;

    // The key `f`'s data sits under in a NAME-KEYED store (a save file's
    // object, a graph's structDefaults): its current name when `has` finds it,
    // else the newest former name `has` finds that no live field claims. Empty
    // when neither is there. `f` must be one of this def's fields.
    std::string storedKey(const StructField& f,
                          const std::function<bool(const std::string&)>& has) const;
};

// Book the rename of row `index` (already carrying its NEW name) from
// `oldName`: noteRename on that row, and the old name is taken OFF every other
// row. An alias has exactly one owner — the row that held the name last — so
// "x→a, later a new x→y" leaves x pointing at y, never at both. What the panel
// and the MCP tools call; hand-edited duplicates are settled by the readers
// (findField/findEntry: first in definition order).
HE_API void noteFieldRename(StructDef& def, size_t index, const std::string& oldName);
HE_API void noteEntryRename(EnumDef& def, size_t index, const std::string& oldName);

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

    // The starting Value of ONE field — exactly what makeDefaultValue puts into
    // a struct's slot for it. Public because the savegame seeder needs the same
    // answer for a template's fields (a SaveGameTemplate IS a StructDef), and
    // the two used to spell the rule out separately and had to be kept in step
    // by hand; containers made that a third copy waiting to drift.
    HorizonCode::Value makeFieldDefault(const StructField& f) const;

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
