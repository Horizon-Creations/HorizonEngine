// Dumps HE::api::registry() as JSON: every Engine Call row with its pins, the
// C++ callee, whether Lua/Python see it as horizon.<group>.<fn>, and the editor's
// description text (HcNodeDocs, compiled in — it only needs EngineApi.h).
//
// Built and run by dump_engine_api.sh against an existing build tree; the
// registry is only knowable by running it, because many rows are added through
// helpers (unary("math.sin", …)) that a grep over EngineApi.cpp does not see.

#include "HcNodeDocs.h"

#include <HorizonScene/EngineApi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
const char* pinName(HorizonCode::PinType t)
{
    static const char* kNames[] = { "Exec", "Float", "Bool",  "Int",    "String", "Vec2", "Color",
                                    "Ref",  "Transform", "Enum", "Struct", "Vec3",   "Vec4" };
    const auto i = static_cast<size_t>(t);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;
        }
    }
    return o;
}

std::string esc(const char* s) { return s ? esc(std::string(s)) : std::string(); }

void pins(const std::vector<HE::api::ApiParam>& v)
{
    std::printf("[");
    for (size_t i = 0; i < v.size(); ++i)
        std::printf("%s{\"name\":\"%s\",\"type\":\"%s\",\"array\":%s,\"self\":%s}", i ? "," : "",
                    esc(v[i].name).c_str(), pinName(v[i].type), v[i].isArray ? "true" : "false",
                    v[i].selfDefault ? "true" : "false");
    std::printf("]");
}
} // namespace

int main()
{
    const auto& reg = HE::api::registry();
    std::printf("[\n");
    for (size_t i = 0; i < reg.size(); ++i)
    {
        const HE::api::ApiFn& f = reg[i];
        const std::string id = f.id;
        const std::string group = id.substr(0, id.find('.'));
        std::printf("%s{\"id\":\"%s\",\"group\":\"%s\",\"category\":\"%s\",\"display\":\"%s\","
                    "\"exec\":%s,\"script\":%s,\"cpp\":\"%s\",\"doc\":\"%s\",\"params\":",
                    i ? ",\n" : "", esc(f.id).c_str(), esc(group).c_str(), esc(f.category).c_str(),
                    esc(f.displayName).c_str(), f.isExec ? "true" : "false",
                    HE::api::isScriptGroup(group) ? "true" : "false", esc(f.cppCall).c_str(),
                    esc(HE::Ed::NodeDocs::engineCall(id)).c_str());
        pins(f.params);
        std::printf(",\"results\":");
        pins(f.results);
        std::printf("}");
    }
    std::printf("\n]\n");
    return 0;
}
