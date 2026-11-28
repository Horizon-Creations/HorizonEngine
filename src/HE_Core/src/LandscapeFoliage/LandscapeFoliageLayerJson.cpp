#include "LandscapeFoliage/LandscapeFoliageLayerJson.h"

#include <ContentManager/Assets.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{

using json = nlohmann::json;

// The one thrown-away-on-failure type: parsing builds into a scratch asset and
// only a complete success is copied out, so `out` is untouched on any failure.
struct ParseError { std::string what; };

[[noreturn]] void fail(std::string what) { throw ParseError{ std::move(what) }; }

json uuidJson(const HE::UUID& u) { return { { "hi", u.hi }, { "lo", u.lo } }; }

// Absent and null are "no asset". Anything else must be the exact form the
// reference scan and the retarget recognise; a string "path" here would dangle in
// a pack (the packer rewrites paths only inside Mesh/Material/Scene/Particle blobs).
HE::UUID getUuid(const json& j, const char* key, const std::string& where)
{
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (!it->is_object()) fail(where + ": '" + key + "' is not a {hi, lo} object");
    const auto hi = it->find("hi");
    const auto lo = it->find("lo");
    if (hi == it->end() || lo == it->end() || !hi->is_number_unsigned() || !lo->is_number_unsigned())
        fail(where + ": '" + key + "' needs unsigned integers hi and lo");
    return { hi->get<uint64_t>(), lo->get<uint64_t>() };
}

void getFloat(const json& j, const char* key, float& out, const std::string& where)
{
    const auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_number()) fail(where + ": '" + key + "' is not a number");
    out = it->get<float>();
}

void getBool(const json& j, const char* key, bool& out, const std::string& where)
{
    const auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_boolean()) fail(where + ": '" + key + "' is not true/false");
    out = it->get<bool>();
}

void getString(const json& j, const char* key, std::string& out, const std::string& where)
{
    const auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_string()) fail(where + ": '" + key + "' is not a string");
    out = it->get<std::string>();
}

LandscapeFoliageLod lodFromJson(const json& j, const std::string& where)
{
    if (!j.is_object()) fail(where + " is not an object");
    LandscapeFoliageLod l;
    l.meshAssetId     = getUuid(j, "meshAssetId", where);
    l.materialAssetId = getUuid(j, "materialAssetId", where);
    getFloat(j, "distance", l.distance, where);
    getFloat(j, "density", l.density, where);
    return l;
}

LandscapeFoliageEntry entryFromJson(const json& j, size_t index)
{
    const std::string where = "entry " + std::to_string(index);
    if (!j.is_object()) fail(where + " is not an object");
    LandscapeFoliageEntry e;
    getString(j, "name", e.name, where);
    e.meshAssetId     = getUuid(j, "meshAssetId", where);
    e.materialAssetId = getUuid(j, "materialAssetId", where);

    if (const auto it = j.find("lods"); it != j.end())
    {
        if (!it->is_array()) fail(where + ": 'lods' is not an array");
        if (it->size() > kLandscapeFoliageMaxLods)
            fail(where + ": " + std::to_string(it->size()) + " lods, at most "
                 + std::to_string(kLandscapeFoliageMaxLods));
        for (size_t i = 0; i < it->size(); ++i)
            e.lods.push_back(lodFromJson((*it)[i], where + " lod " + std::to_string(i)));
    }

    if (const auto it = j.find("paintLayer"); it != j.end())
    {
        if (!it->is_number_integer()) fail(where + ": 'paintLayer' is not an integer");
        // Range is validation's business; only keep it from overflowing the int.
        e.paintLayer = static_cast<int>(std::clamp<int64_t>(it->get<int64_t>(), -1000, 1000));
    }
    getFloat(j, "slopeMin", e.slopeMin, where);
    getFloat(j, "slopeMax", e.slopeMax, where);
    getFloat(j, "heightMin", e.heightMin, where);
    getFloat(j, "heightMax", e.heightMax, where);
    getFloat(j, "density", e.density, where);
    getFloat(j, "minScale", e.minScale, where);
    getFloat(j, "maxScale", e.maxScale, where);
    getFloat(j, "drawDistance", e.drawDistance, where);
    getFloat(j, "bucketSize", e.bucketSize, where);
    getBool(j, "castsShadow", e.castsShadow, where);
    getBool(j, "contributesAO", e.contributesAO, where);
    getFloat(j, "shadowDistance", e.shadowDistance, where);
    getFloat(j, "lodHysteresis", e.lodHysteresis, where);
    return e;
}

void putUuid(json& j, const char* key, const HE::UUID& u)
{
    if (u != HE::UUID{}) j[key] = uuidJson(u);
}

} // namespace

std::string landscapeFoliageLayerToJson(const LandscapeFoliageLayerAsset& a)
{
    json root;
    root["version"]      = kLandscapeFoliageLayerVersion;
    root["description"]  = a.description;
    root["densityScale"] = a.densityScale;
    json entries = json::array();
    for (const LandscapeFoliageEntry& e : a.entries)
    {
        json j;
        j["name"] = e.name;
        putUuid(j, "meshAssetId", e.meshAssetId);
        putUuid(j, "materialAssetId", e.materialAssetId);
        if (!e.lods.empty())
        {
            json lods = json::array();
            for (const LandscapeFoliageLod& l : e.lods)
            {
                json lj;
                putUuid(lj, "meshAssetId", l.meshAssetId);
                putUuid(lj, "materialAssetId", l.materialAssetId);
                lj["distance"] = l.distance;
                lj["density"]  = l.density;
                lods.push_back(std::move(lj));
            }
            j["lods"] = std::move(lods);
        }
        j["paintLayer"]     = e.paintLayer;
        j["slopeMin"]       = e.slopeMin;
        j["slopeMax"]       = e.slopeMax;
        j["heightMin"]      = e.heightMin;
        j["heightMax"]      = e.heightMax;
        j["density"]        = e.density;
        j["minScale"]       = e.minScale;
        j["maxScale"]       = e.maxScale;
        j["drawDistance"]   = e.drawDistance;
        j["bucketSize"]     = e.bucketSize;
        j["castsShadow"]    = e.castsShadow;
        j["contributesAO"]  = e.contributesAO;
        j["shadowDistance"] = e.shadowDistance;
        j["lodHysteresis"]  = e.lodHysteresis;
        entries.push_back(std::move(j));
    }
    root["entries"] = std::move(entries);
    return root.dump(2);
}

bool landscapeFoliageLayerFromJson(const std::string& text, LandscapeFoliageLayerAsset& out,
                                   std::string* error)
{
    try
    {
        const json root = json::parse(text, nullptr, /*allow_exceptions=*/true);
        if (!root.is_object()) fail("the document is not a JSON object");

        const auto ver = root.find("version");
        if (ver == root.end() || !ver->is_number_integer())
            fail("'version' is missing or not an integer");
        if (ver->get<int64_t>() != kLandscapeFoliageLayerVersion)
            fail("version " + std::to_string(ver->get<int64_t>()) + " is not readable by this build (reads "
                 + std::to_string(kLandscapeFoliageLayerVersion) + ")");

        LandscapeFoliageLayerAsset scratch;
        getString(root, "description", scratch.description, "document");
        getFloat(root, "densityScale", scratch.densityScale, "document");

        if (const auto it = root.find("entries"); it != root.end())
        {
            if (!it->is_array()) fail("'entries' is not an array");
            if (it->size() > kLandscapeFoliageMaxEntries)
                fail(std::to_string(it->size()) + " entries, at most "
                     + std::to_string(kLandscapeFoliageMaxEntries));
            for (size_t i = 0; i < it->size(); ++i)
                scratch.entries.push_back(entryFromJson((*it)[i], i));
        }

        out.description  = std::move(scratch.description);
        out.densityScale = scratch.densityScale;
        out.entries      = std::move(scratch.entries);
        return true;
    }
    catch (const ParseError& e)
    {
        if (error) *error = e.what;
    }
    catch (const json::exception& e)
    {
        if (error) *error = std::string("not JSON: ") + e.what();
    }
    return false;
}

std::vector<std::string> validateLandscapeFoliageLayer(const LandscapeFoliageLayerAsset& a)
{
    std::vector<std::string> problems;
    auto add = [&problems](size_t i, const LandscapeFoliageEntry* e, const std::string& msg)
    {
        std::string line = "entry " + std::to_string(i);
        if (e && !e->name.empty()) line += " '" + e->name + "'";
        problems.push_back(line + ": " + msg);
    };
    if (!(a.densityScale >= 0.0f)) problems.push_back("densityScale is negative");

    for (size_t i = 0; i < a.entries.size(); ++i)
    {
        const LandscapeFoliageEntry& e = a.entries[i];
        if (e.meshAssetId == HE::UUID{})             add(i, &e, "no mesh");
        if (e.paintLayer < -1 || e.paintLayer > kLandscapeFoliageMaxPaintLayer)
            add(i, &e, "paintLayer " + std::to_string(e.paintLayer) + " is outside -1..7");
        if (!(e.slopeMin >= 0.0f && e.slopeMax <= 1.0f && e.slopeMin <= e.slopeMax))
            add(i, &e, "slope range must satisfy 0 <= slopeMin <= slopeMax <= 1");
        if (!(e.heightMin <= e.heightMax))           add(i, &e, "heightMin is above heightMax");
        if (!(e.density >= 0.0f))                    add(i, &e, "density is negative");
        if (!(e.minScale > 0.0f && e.minScale <= e.maxScale))
            add(i, &e, "scale range must satisfy 0 < minScale <= maxScale");
        if (!(e.drawDistance > 0.0f))                add(i, &e, "drawDistance must be positive");
        if (!(e.bucketSize > 0.0f))                  add(i, &e, "bucketSize must be positive");
        if (!(e.shadowDistance >= 0.0f))             add(i, &e, "shadowDistance is negative");
        if (!(e.lodHysteresis >= 0.0f))              add(i, &e, "lodHysteresis is negative");
        float prev = 0.0f;
        for (size_t k = 0; k < e.lods.size(); ++k)
        {
            const LandscapeFoliageLod& l = e.lods[k];
            const std::string at = "lod " + std::to_string(k) + ": ";
            if (l.meshAssetId == HE::UUID{})          add(i, &e, at + "no mesh");
            if (!(l.density >= 0.0f && l.density <= 1.0f)) add(i, &e, at + "density is outside 0..1");
            if (!(l.distance > prev))
                add(i, &e, at + "distance must be above the previous level's and positive");
            prev = std::max(prev, l.distance);
        }
    }
    return problems;
}

void landscapeFoliageLayerAssetRefs(const LandscapeFoliageLayerAsset& a, std::vector<HE::UUID>& out)
{
    auto add = [&out](const HE::UUID& id)
    {
        if (id == HE::UUID{}) return;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (const LandscapeFoliageEntry& e : a.entries)
    {
        add(e.meshAssetId);
        add(e.materialAssetId);
        for (const LandscapeFoliageLod& l : e.lods)
        {
            add(l.meshAssetId);
            add(l.materialAssetId);
        }
    }
}

} // namespace HE
