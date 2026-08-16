#include <Types/TypeRegistry.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/HAsset.h>
#include <Diagnostics/Logger.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace HE {

using json = nlohmann::json;
using HorizonCode::PinType;
using HorizonCode::Value;

// ─── Def lookups ─────────────────────────────────────────────────────────────

const EnumEntry* EnumDef::findEntry(const std::string& n) const
{
    for (const EnumEntry& e : entries) if (e.name == n) return &e;
    return nullptr;
}
const EnumEntry* EnumDef::findValue(int v) const
{
    for (const EnumEntry& e : entries) if (e.value == v) return &e;
    return nullptr;
}
const StructField* StructDef::findField(const std::string& n) const
{
    for (const StructField& f : fields) if (f.name == n) return &f;
    return nullptr;
}

// ─── Registry storage ────────────────────────────────────────────────────────

struct TypeRegistry::Impl
{
    mutable std::mutex mutex;
    std::unordered_map<std::string, EnumDef>   enums;    // key = assetPath
    std::unordered_map<std::string, StructDef> structs;  // key = assetPath
};

TypeRegistry& TypeRegistry::instance()
{
    static TypeRegistry r;
    return r;
}

TypeRegistry::Impl& TypeRegistry::impl() const
{
    static Impl i;
    return i;
}

void TypeRegistry::registerEnum(EnumDef def)
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    im.enums[def.assetPath] = std::move(def);
}

void TypeRegistry::registerStruct(StructDef def)
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    im.structs[def.assetPath] = std::move(def);
}

void TypeRegistry::removeType(const std::string& assetPath)
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    im.enums.erase(assetPath);
    im.structs.erase(assetPath);
}

void TypeRegistry::clear()
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    im.enums.clear();
    im.structs.clear();
}

bool TypeRegistry::getEnum(const std::string& assetPath, EnumDef& out) const
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    auto it = im.enums.find(assetPath);
    if (it == im.enums.end()) return false;
    out = it->second;
    return true;
}

bool TypeRegistry::getStruct(const std::string& assetPath, StructDef& out) const
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    auto it = im.structs.find(assetPath);
    if (it == im.structs.end()) return false;
    out = it->second;
    return true;
}

bool TypeRegistry::hasEnum(const std::string& assetPath) const
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    return im.enums.count(assetPath) != 0;
}

bool TypeRegistry::hasStruct(const std::string& assetPath) const
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    return im.structs.count(assetPath) != 0;
}

std::vector<EnumDef> TypeRegistry::enums() const
{
    Impl& im = impl();
    std::vector<EnumDef> out;
    {
        std::lock_guard<std::mutex> lk(im.mutex);
        out.reserve(im.enums.size());
        for (const auto& [_, d] : im.enums) out.push_back(d);
    }
    std::sort(out.begin(), out.end(),
        [](const EnumDef& a, const EnumDef& b){ return a.name < b.name; });
    return out;
}

std::vector<StructDef> TypeRegistry::structs() const
{
    Impl& im = impl();
    std::vector<StructDef> out;
    {
        std::lock_guard<std::mutex> lk(im.mutex);
        out.reserve(im.structs.size());
        for (const auto& [_, d] : im.structs) out.push_back(d);
    }
    std::sort(out.begin(), out.end(),
        [](const StructDef& a, const StructDef& b){ return a.name < b.name; });
    return out;
}

bool TypeRegistry::nameCollides(const std::string& name, const std::string& assetPath) const
{
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);
    for (const auto& [path, d] : im.enums)
        if (d.name == name && path != assetPath) return true;
    for (const auto& [path, d] : im.structs)
        if (d.name == name && path != assetPath) return true;
    return false;
}

bool TypeRegistry::structWouldCycle(const StructDef& def) const
{
    // DFS over struct-typed field references, treating `def` as the (possibly
    // updated) definition of def.assetPath. Cycle = we re-enter a path already
    // on the stack.
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mutex);

    std::unordered_set<std::string> onStack, done;
    // Iterative DFS with explicit stack of (path, resolved def pointer).
    struct Frame { std::string path; const StructDef* d; size_t next; };
    auto resolve = [&](const std::string& path) -> const StructDef* {
        if (path == def.assetPath) return &def;  // the incoming (edited) def wins
        auto it = im.structs.find(path);
        return it != im.structs.end() ? &it->second : nullptr;
    };
    std::vector<Frame> stack{ { def.assetPath, &def, 0 } };
    onStack.insert(def.assetPath);
    while (!stack.empty())
    {
        Frame& f = stack.back();
        bool descended = false;
        while (f.next < f.d->fields.size())
        {
            const StructField& fl = f.d->fields[f.next++];
            if (fl.type != PinType::Struct || fl.typeName.empty()) continue;
            if (onStack.count(fl.typeName)) return true;          // back edge
            if (done.count(fl.typeName)) continue;
            if (const StructDef* child = resolve(fl.typeName))
            {
                onStack.insert(fl.typeName);
                stack.push_back({ fl.typeName, child, 0 });
                descended = true;
                break;
            }
        }
        if (!descended && f.next >= f.d->fields.size())
        {
            onStack.erase(f.path);
            done.insert(f.path);
            stack.pop_back();
        }
    }
    return false;
}

// ─── Default values ──────────────────────────────────────────────────────────

namespace {

Value fieldDefault(const TypeRegistry& reg, const StructField& f,
                   std::unordered_set<std::string>& visiting);

Value structDefault(const TypeRegistry& reg, const std::string& assetPath,
                    std::unordered_set<std::string>& visiting)
{
    Value v;
    v.type = PinType::Struct;
    v.typeName = assetPath;
    StructDef def;
    if (!reg.getStruct(assetPath, def)) return v;                 // unknown → empty struct
    if (!visiting.insert(assetPath).second) return v;             // cycle guard: stop descending
    v.items.reserve(def.fields.size());
    for (const StructField& f : def.fields)
        v.items.push_back(fieldDefault(reg, f, visiting));
    visiting.erase(assetPath);
    return v;
}

Value fieldDefault(const TypeRegistry& reg, const StructField& f,
                   std::unordered_set<std::string>& visiting)
{
    if (f.isArray)
    {
        // The authored slots seed the array; no slots = an empty one.
        Value v; v.type = f.type; v.isArray = true; v.typeName = f.typeName;
        v.items = f.defaultValue.items;
        for (Value& it : v.items)
        {
            it.isArray = false;
            it.type = f.type;
            it.typeName = f.typeName;
            if (f.type == PinType::Enum)
            {
                // Slots persist the entry NAME; resolve it like everywhere else.
                EnumDef ed;
                const std::string entry = it.s;
                it.i = 0;
                if (reg.getEnum(f.typeName, ed))
                {
                    if (const EnumEntry* e = ed.findEntry(entry)) it.i = e->value;
                    else if (!ed.entries.empty())                 it.i = ed.entries.front().value;
                }
            }
            else if (f.type == PinType::Struct)
            {
                it = reg.makeDefaultValue(f.typeName);   // elements use their own defaults
            }
        }
        return v;
    }
    switch (f.type)
    {
    case PinType::Struct:
        return structDefault(reg, f.typeName, visiting);
    case PinType::Enum:
    {
        Value v; v.type = PinType::Enum; v.typeName = f.typeName;
        // The authored default is the entry NAME (robust across renumbering);
        // resolve it now, falling back to the first entry, then 0.
        EnumDef ed;
        if (reg.getEnum(f.typeName, ed))
        {
            if (const EnumEntry* e = ed.findEntry(f.defaultValue.s)) v.i = e->value;
            else if (!ed.entries.empty())                            v.i = ed.entries.front().value;
        }
        return v;
    }
    default:
    {
        Value v = f.defaultValue;
        v.type = f.type;           // authoritative even if the payload was defaulted
        v.typeName.clear();
        return v;
    }
    }
}

} // namespace

Value TypeRegistry::makeDefaultValue(const std::string& structAssetPath) const
{
    std::unordered_set<std::string> visiting;
    return structDefault(*this, structAssetPath, visiting);
}

// ─── JSON round-trip ─────────────────────────────────────────────────────────
// Payload shapes (CHUNK_ENDF / CHUNK_STDF):
//   enum:   { "entries": [ { "name": "Sword", "value": 0 }, ... ] }
//   struct: { "fields": [ { "name": "hp", "type": 1, "isArray": false,
//                           "typeName": "", "default": <scalar> }, ... ] }
// Scalar default encodings: Float/Int → number, Bool → bool, String → string,
// Vec2 → [x,y], Color → [r,g,b,a], Transform → {"pos":[3],"rot":[3],"scl":[3]},
// Enum → entry name (string). Struct fields and arrays carry no inline default
// (nested defs supply their own; arrays start empty).

namespace {

json defaultToJson(const StructField& f)
{
    const Value& v = f.defaultValue;
    switch (f.type)
    {
    case PinType::Float:  return v.f;
    case PinType::Int:    return v.i;
    case PinType::Bool:   return v.b;
    case PinType::String: return v.s;
    case PinType::Enum:   return v.s;   // entry name
    case PinType::Vec2:   return json::array({ v.v2.x, v.v2.y });
    case PinType::Color:  return json::array({ v.col.x, v.col.y, v.col.z, v.col.w });
    case PinType::Vec3:   return json::array({ v.v3.x, v.v3.y, v.v3.z });
    case PinType::Vec4:   return json::array({ v.v4.x, v.v4.y, v.v4.z, v.v4.w });
    case PinType::Transform:
        return json{ { "pos", { v.tpos.x, v.tpos.y, v.tpos.z } },
                     { "rot", { v.trot.x, v.trot.y, v.trot.z } },
                     { "scl", { v.tscl.x, v.tscl.y, v.tscl.z } } };
    default: return nullptr;
    }
}

void defaultFromJson(const json& j, StructField& f)
{
    Value& v = f.defaultValue;
    switch (f.type)
    {
    case PinType::Float:  if (j.is_number())  v.f = j.get<float>(); break;
    case PinType::Int:    if (j.is_number())  v.i = j.get<int>();   break;
    case PinType::Bool:   if (j.is_boolean()) v.b = j.get<bool>();  break;
    case PinType::String: if (j.is_string())  v.s = j.get<std::string>(); break;
    case PinType::Enum:   if (j.is_string())  v.s = j.get<std::string>(); break;
    case PinType::Vec2:
        if (j.is_array() && j.size() >= 2)
            v.v2 = { j[0].get<float>(), j[1].get<float>() };
        break;
    case PinType::Vec3:
        if (j.is_array() && j.size() >= 3)
            v.v3 = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
        break;
    case PinType::Vec4:
        if (j.is_array() && j.size() >= 4)
            v.v4 = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
        break;
    case PinType::Color:
        if (j.is_array() && j.size() >= 4)
            v.col = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
        break;
    case PinType::Transform:
        if (j.is_object())
        {
            auto vec3 = [&](const char* key, glm::vec3& out, const glm::vec3& def) {
                out = def;
                auto it = j.find(key);
                if (it != j.end() && it->is_array() && it->size() >= 3)
                    out = { (*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>() };
            };
            vec3("pos", v.tpos, glm::vec3(0.0f));
            vec3("rot", v.trot, glm::vec3(0.0f));
            vec3("scl", v.tscl, glm::vec3(1.0f));
        }
        break;
    default: break;
    }
    v.type = f.type;
}

} // namespace

std::string TypeRegistry::enumToJson(const EnumDef& def)
{
    json entries = json::array();
    for (const EnumEntry& e : def.entries)
        entries.push_back({ { "name", e.name }, { "value", e.value } });
    return json{ { "entries", std::move(entries) } }.dump(2);
}

bool TypeRegistry::enumFromJson(const std::string& text, EnumDef& out)
{
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    out.entries.clear();
    if (auto it = j.find("entries"); it != j.end() && it->is_array())
        for (const json& e : *it)
        {
            if (!e.is_object()) continue;
            EnumEntry en;
            en.name  = e.value("name", std::string{});
            en.value = e.value("value", 0);
            if (!en.name.empty()) out.entries.push_back(std::move(en));
        }
    return true;
}

std::string TypeRegistry::structToJson(const StructDef& def)
{
    json fields = json::array();
    for (const StructField& f : def.fields)
    {
        json jf{ { "name", f.name },
                 { "type", static_cast<int>(f.type) },
                 { "isArray", f.isArray },
                 { "typeName", f.typeName } };
        if (f.isArray)
        {
            // Authored starting elements. Each slot uses the SCALAR encoding of
            // the element type (enum = entry name); struct elements carry no
            // slot payload — they seed from their own definition.
            if (f.type != PinType::Struct && !f.defaultValue.items.empty())
            {
                json arr = json::array();
                StructField elem = f; elem.isArray = false;
                for (const Value& it : f.defaultValue.items)
                {
                    elem.defaultValue = it;
                    elem.defaultValue.type = f.type;
                    json d = defaultToJson(elem);
                    if (!d.is_null()) arr.push_back(std::move(d));
                }
                if (!arr.empty()) jf["default"] = std::move(arr);
            }
        }
        else if (f.type != PinType::Struct)
        {
            json d = defaultToJson(f);
            if (!d.is_null()) jf["default"] = std::move(d);
        }
        fields.push_back(std::move(jf));
    }
    return json{ { "fields", std::move(fields) } }.dump(2);
}

bool TypeRegistry::structFromJson(const std::string& text, StructDef& out)
{
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;
    out.fields.clear();
    if (auto it = j.find("fields"); it != j.end() && it->is_array())
        for (const json& e : *it)
        {
            if (!e.is_object()) continue;
            StructField f;
            f.name     = e.value("name", std::string{});
            f.type     = static_cast<PinType>(e.value("type", 1));
            f.isArray  = e.value("isArray", false);
            f.typeName = e.value("typeName", std::string{});
            if (auto d = e.find("default"); d != e.end())
            {
                if (f.isArray)
                {
                    f.defaultValue = {};
                    f.defaultValue.isArray = true;
                    f.defaultValue.type = f.type;
                    f.defaultValue.typeName = f.typeName;
                    if (d->is_array())
                        for (const json& slot : *d)
                        {
                            StructField elem = f; elem.isArray = false; elem.defaultValue = {};
                            defaultFromJson(slot, elem);
                            elem.defaultValue.isArray = false;
                            f.defaultValue.items.push_back(std::move(elem.defaultValue));
                        }
                }
                else
                {
                    defaultFromJson(*d, f);
                }
            }
            else
            {
                f.defaultValue.type = f.type;
                f.defaultValue.isArray = f.isArray;
            }
            if (!f.name.empty()) out.fields.push_back(std::move(f));
        }
    return true;
}

// ─── Content refresh ─────────────────────────────────────────────────────────

size_t TypeRegistry::refreshFromContent(::ContentManager& cm)
{
    // loadAsset on a StructType/EnumType asset registers the definition as a
    // side effect (see ContentManager), so discovery + load IS the refresh.
    size_t n = 0;
    for (const HE::UUID id : cm.discoverAssets(HE::AssetType::EnumType))
        if (cm.getEnumType(id)) ++n;
    for (const HE::UUID id : cm.discoverAssets(HE::AssetType::StructType))
        if (cm.getStructType(id)) ++n;
    return n;
}

} // namespace HE
