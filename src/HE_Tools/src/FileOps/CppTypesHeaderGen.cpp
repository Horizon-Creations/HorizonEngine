#include "CppTypesHeaderGen.h"
#include <Types/TypeRegistry.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace HE {

using HorizonCode::PinType;
using HorizonCode::Value;

namespace {

// A definition name becomes a C++ identifier: invalid characters collapse to
// '_', a leading digit gets one prefixed. "HP %" → "HP__".
std::string sanitizeIdent(const std::string& name)
{
    std::string out;
    out.reserve(name.size() + 1);
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty()) out = "_";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
    return out;
}

std::string cppStringLiteral(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string floatLit(float f)
{
    std::ostringstream o;
    o << f;
    std::string s = o.str();
    // "5" → "5.0f" so the literal is unambiguously float.
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s + "f";
}

struct NameTable
{
    // assetPath → sanitized unique C++ name for every enum/struct.
    std::unordered_map<std::string, std::string> names;

    std::string of(const std::string& assetPath) const
    {
        auto it = names.find(assetPath);
        return it != names.end() ? it->second : std::string("int /* unknown type */");
    }
};

std::string fieldCppType(const StructField& f, const NameTable& nt)
{
    std::string base;
    switch (f.type)
    {
    case PinType::Float:     base = "float"; break;
    case PinType::Int:       base = "int"; break;
    case PinType::Bool:      base = "bool"; break;
    case PinType::String:    base = "std::string"; break;
    case PinType::Vec2:      base = "HeVec2"; break;
    case PinType::Color:     base = "HeColor"; break;
    case PinType::Transform: base = "HeTransform"; break;
    case PinType::Enum:
    case PinType::Struct:    base = nt.of(f.typeName); break;
    default:                 base = "float"; break;
    }
    return f.isArray ? "std::vector<" + base + ">" : base;
}

// Member initializer for a scalar field ("" = none, keep the type's default).
std::string fieldInitializer(const StructField& f, const NameTable& nt,
                             const TypeRegistry& reg)
{
    if (f.isArray) return {};                        // vectors start empty
    const Value& v = f.defaultValue;
    switch (f.type)
    {
    case PinType::Float:  return " = " + floatLit(v.f);
    case PinType::Int:    return " = " + std::to_string(v.i);
    case PinType::Bool:   return v.b ? " = true" : " = false";
    case PinType::String: return v.s.empty() ? std::string{} : " = " + cppStringLiteral(v.s);
    case PinType::Vec2:
        return " = { " + floatLit(v.v2.x) + ", " + floatLit(v.v2.y) + " }";
    case PinType::Color:
        return " = { " + floatLit(v.col.x) + ", " + floatLit(v.col.y) + ", "
             + floatLit(v.col.z) + ", " + floatLit(v.col.w) + " }";
    case PinType::Transform:
        return " = { { " + floatLit(v.tpos.x) + ", " + floatLit(v.tpos.y) + ", " + floatLit(v.tpos.z)
             + " }, { " + floatLit(v.trot.x) + ", " + floatLit(v.trot.y) + ", " + floatLit(v.trot.z)
             + " }, { " + floatLit(v.tscl.x) + ", " + floatLit(v.tscl.y) + ", " + floatLit(v.tscl.z) + " } }";
    case PinType::Enum:
    {
        // The authored default is the entry NAME; fall back to the first entry.
        EnumDef def;
        if (!reg.getEnum(f.typeName, def) || def.entries.empty()) return {};
        const EnumEntry* e = def.findEntry(v.s);
        if (!e) e = &def.entries.front();
        return " = " + nt.of(f.typeName) + "::" + sanitizeIdent(e->name);
    }
    case PinType::Struct: return {};                 // its own member initializers apply
    default: return {};
    }
}

} // namespace

std::string generateCppTypesHeader()
{
    auto& reg = TypeRegistry::instance();
    const auto enums   = reg.enums();
    const auto structs = reg.structs();

    // Unique sanitized names (collisions get a numeric suffix — the panel
    // already warns about them at authoring time).
    NameTable nt;
    {
        std::unordered_set<std::string> used;
        auto claim = [&](const std::string& assetPath, const std::string& name) {
            std::string base = sanitizeIdent(name), pick = base;
            for (int i = 2; used.count(pick); ++i) pick = base + std::to_string(i);
            used.insert(pick);
            nt.names[assetPath] = pick;
        };
        for (const auto& d : enums)   claim(d.assetPath, d.name);
        for (const auto& d : structs) claim(d.assetPath, d.name);
    }

    std::string out;
    out += "#pragma once\n";
    out += "// Generated by HorizonEditor from this project's Struct/Enum assets.\n";
    out += "// DO NOT EDIT — the editor rewrites this file when a definition is saved.\n";
    out += "#include <string>\n#include <vector>\n\n";
    out += "// Minimal value helpers (no engine/glm dependency in game code).\n";
    out += "struct HeVec2      { float x = 0.0f, y = 0.0f; };\n";
    out += "struct HeColor     { float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f; };\n";
    out += "struct HeVec3      { float x = 0.0f, y = 0.0f, z = 0.0f; };\n";
    out += "struct HeTransform { HeVec3 pos{}; HeVec3 rot{}; HeVec3 scl{ 1.0f, 1.0f, 1.0f }; };\n\n";

    for (const auto& d : enums)
    {
        const std::string name = nt.of(d.assetPath);
        if (name != d.name) out += "// \"" + d.name + "\" (" + d.assetPath + ")\n";
        else                out += "// " + d.assetPath + "\n";
        out += "enum class " + name + " : int\n{\n";
        std::unordered_set<std::string> usedEntries;
        for (const auto& e : d.entries)
        {
            std::string en = sanitizeIdent(e.name), pick = en;
            for (int i = 2; usedEntries.count(pick); ++i) pick = en + std::to_string(i);
            usedEntries.insert(pick);
            out += "    " + pick + " = " + std::to_string(e.value) + ",";
            if (pick != e.name) out += "   // \"" + e.name + "\"";
            out += "\n";
        }
        out += "};\n\n";
    }

    // Structs in dependency order: a struct comes after every struct it embeds.
    // The authoring-side cycle guard makes this a plain topological sort; a
    // hand-edited cycle degrades to "emit what remains, comment the rest".
    {
        std::unordered_set<std::string> emitted;
        std::vector<StructDef> pending = structs;
        bool progress = true;
        while (!pending.empty() && progress)
        {
            progress = false;
            for (auto it = pending.begin(); it != pending.end(); )
            {
                const bool ready = std::all_of(it->fields.begin(), it->fields.end(),
                    [&](const StructField& f) {
                        return f.type != PinType::Struct || f.typeName.empty()
                            || emitted.count(f.typeName) || !reg.hasStruct(f.typeName);
                    });
                if (!ready) { ++it; continue; }

                const StructDef& d = *it;
                const std::string name = nt.of(d.assetPath);
                if (name != d.name) out += "// \"" + d.name + "\" (" + d.assetPath + ")\n";
                else                out += "// " + d.assetPath + "\n";
                out += "struct " + name + "\n{\n";
                std::unordered_set<std::string> usedFields;
                for (const auto& f : d.fields)
                {
                    std::string fn = sanitizeIdent(f.name), pick = fn;
                    for (int i = 2; usedFields.count(pick); ++i) pick = fn + std::to_string(i);
                    usedFields.insert(pick);
                    out += "    " + fieldCppType(f, nt) + " " + pick
                         + fieldInitializer(f, nt, reg) + ";";
                    if (pick != f.name) out += "   // \"" + f.name + "\"";
                    out += "\n";
                }
                out += "};\n\n";
                emitted.insert(d.assetPath);
                it = pending.erase(it);
                progress = true;
            }
        }
        for (const auto& d : pending)
            out += "// SKIPPED \"" + d.name + "\" (" + d.assetPath +
                   "): circular struct reference — fix the definitions.\n";
    }
    return out;
}

bool writeCppTypesHeader(const std::filesystem::path& projectDir)
{
    const std::filesystem::path dir  = projectDir / "Source" / "Generated";
    const std::filesystem::path file = dir / "GameTypes.h";
    const std::string text = generateCppTypesHeader();

    std::error_code ec;
    // Unchanged bytes → no write (don't dirty build-system timestamps).
    {
        std::ifstream in(file, std::ios::binary);
        if (in)
        {
            std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (cur == text) return true;
        }
    }
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        f.close();
        if (f.fail()) { std::filesystem::remove(tmp, ec); return false; }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    HE_LOG_INFO(Editor, "%s", ("CppTypesHeaderGen: wrote " + file.string()).c_str());
    return true;
}

} // namespace HE
