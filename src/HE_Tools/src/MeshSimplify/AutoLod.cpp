#include "AutoLod.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace AutoLod
{
namespace
{

constexpr const char* kExt = ".hasset";

// `<stem>_LOD<n>` → true and n; anything else false.
bool parseLodStem(const std::string& stem, int* n = nullptr)
{
    const size_t at = stem.rfind("_LOD");
    if (at == std::string::npos || at + 4 >= stem.size()) return false;
    int value = 0;
    for (size_t i = at + 4; i < stem.size(); ++i)
    {
        if (stem[i] < '0' || stem[i] > '9') return false;
        value = value * 10 + (stem[i] - '0');
        if (value > 999) return false;
    }
    if (n) *n = value;
    return true;
}

} // namespace

std::string lodPath(const std::string& meshPath, int n)
{
    std::string base = meshPath;
    if (base.size() >= 7 && base.compare(base.size() - 7, 7, kExt) == 0)
        base.resize(base.size() - 7);
    return base + "_LOD" + std::to_string(n) + kExt;
}

bool isLodSidecarPath(const std::string& path)
{
    std::string base = path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() >= 7 && base.compare(base.size() - 7, 7, kExt) == 0)
        base.resize(base.size() - 7);
    return parseLodStem(base);
}

std::vector<std::string> findLodSidecars(const std::filesystem::path& contentRoot,
                                         const std::string& meshPath)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (int n = 1; n < 64; ++n)
    {
        const std::string rel = lodPath(meshPath, n);
        if (!std::filesystem::exists(contentRoot / rel, ec)) break;
        out.push_back(rel);
    }
    return out;
}

std::vector<float> ratiosFor(int levels, float step, float floor)
{
    levels = std::clamp(levels, 1, 8);
    step   = std::clamp(step, 0.05f, 0.95f);
    floor  = std::clamp(floor, 0.01f, 0.95f);
    std::vector<float> out;
    float r = 1.0f;
    for (int i = 0; i < levels; ++i)
    {
        r *= step;
        // The last level lands on the floor; the ones before it never go below it.
        out.push_back(i == levels - 1 ? floor : std::max(r, floor));
    }
    // A floor above the geometric run must not make the chain non-decreasing.
    for (size_t i = 1; i < out.size(); ++i)
        out[i] = std::min(out[i], out[i - 1]);
    return out;
}

bool buildLevel(const StaticMeshAsset& src, float ratio, const Settings& settings,
                StaticMeshAsset& out, float& worldError)
{
    worldError = 0.0f;
    if (src.cooked || src.vertices.size() < 9 || src.indices.size() < 3) return false;

    const size_t vertexCount = src.vertices.size() / 3;
    const bool   hasNormals  = src.normals.size() == src.vertices.size();
    const bool   hasUvs      = src.uvs.size() == vertexCount * 2;

    // The material slots: the asset's own table, or the one-section legacy shape.
    std::vector<MeshSection> sections = src.sections;
    if (sections.empty())
    {
        MeshSection all;
        all.indexOffset = 0;
        all.indexCount  = static_cast<uint32_t>(src.indices.size());
        all.materialPath = src.materialPath;
        all.materialId   = src.materialId;
        sections.push_back(all);
    }

    out = StaticMeshAsset{};
    out.type         = src.type;
    out.materialPath = src.materialPath;
    out.materialId   = src.materialId;

    std::vector<int32_t> remap(vertexCount, -1);
    for (const MeshSection& sec : sections)
    {
        const size_t begin = std::min<size_t>(sec.indexOffset, src.indices.size());
        const size_t end   = std::min<size_t>(begin + sec.indexCount, src.indices.size());
        const size_t count = (end - begin) / 3 * 3;

        MeshSection outSec = sec;
        outSec.indexOffset = static_cast<uint32_t>(out.indices.size());
        if (count == 0)
        {
            outSec.indexCount = 0;
            out.sections.push_back(outSec);
            continue;
        }

        // The section on its own, with only the vertices it uses.
        MeshSimplifier::Mesh local;
        std::vector<uint32_t> usedGlobal;
        local.indices.reserve(count);
        std::fill(remap.begin(), remap.end(), -1);
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        bool valid = true;
        for (size_t i = begin; i < begin + count; ++i)
        {
            const uint32_t g = src.indices[i];
            if (g >= vertexCount) { valid = false; break; }
            if (remap[g] < 0)
            {
                remap[g] = static_cast<int32_t>(local.vertices.size());
                MeshSimplifier::Vertex v;
                v.position = { src.vertices[g * 3], src.vertices[g * 3 + 1], src.vertices[g * 3 + 2] };
                if (hasNormals) v.normal = { src.normals[g * 3], src.normals[g * 3 + 1], src.normals[g * 3 + 2] };
                if (hasUvs)     v.uv     = { src.uvs[g * 2], src.uvs[g * 2 + 1] };
                lo = glm::min(lo, v.position);
                hi = glm::max(hi, v.position);
                local.vertices.push_back(v);
            }
            local.indices.push_back(static_cast<uint32_t>(remap[g]));
        }
        if (!valid) return false;

        MeshSimplifier::Settings base;
        base.targetRatio      = ratio;
        base.targetError      = settings.targetError;
        base.pruneComponents  = settings.foliage;
        base.lockMeshBorder   = !settings.foliage;
        base.minTriangleCount = std::min(count / 3, settings.minSectionTriangles);

        MeshSimplifier::Result res;
        if (settings.foliage && settings.alphaMask && settings.alphaWidth > 0 && settings.alphaHeight > 0)
        {
            MeshSimplifier::FoliageSettings fs;
            fs.base        = base;
            fs.alphaMask   = settings.alphaMask;
            fs.alphaWidth  = settings.alphaWidth;
            fs.alphaHeight = settings.alphaHeight;
            fs.minCardTriangleCount = base.minTriangleCount;
            res = MeshSimplifier::simplifyFoliage(local, fs);
        }
        else
            res = MeshSimplifier::simplify(local, base);

        // meshoptimizer reports the error relative to the extent of what it was given.
        const glm::vec3 ext = hi - lo;
        const float extent  = std::max({ ext.x, ext.y, ext.z });
        worldError = std::max(worldError, res.resultError * extent);

        const uint32_t vBase = static_cast<uint32_t>(out.vertices.size() / 3);
        for (const MeshSimplifier::Vertex& v : res.mesh.vertices)
        {
            out.vertices.insert(out.vertices.end(), { v.position.x, v.position.y, v.position.z });
            if (hasNormals) out.normals.insert(out.normals.end(), { v.normal.x, v.normal.y, v.normal.z });
            if (hasUvs)     out.uvs.insert(out.uvs.end(), { v.uv.x, v.uv.y });
        }
        for (uint32_t idx : res.mesh.indices)
            out.indices.push_back(vBase + idx);
        outSec.indexCount = static_cast<uint32_t>(res.mesh.indices.size());
        out.sections.push_back(outSec);
    }

    // The legacy shape stays legacy: an asset that never carried a table does not
    // gain one just because it was simplified.
    if (src.sections.empty()) out.sections.clear();
    return out.indices.size() >= 3;
}

Chain build(const StaticMeshAsset& lod0, const Settings& settings, std::vector<StaticMeshAsset>* meshes)
{
    Chain chain;
    if (lod0.cooked || lod0.vertices.empty() || lod0.indices.size() < 3)
    {
        chain.error = "The mesh has no readable triangles (packed meshes cannot be simplified).";
        return chain;
    }
    Level base;
    base.path      = lod0.path;
    base.id        = lod0.id;
    base.triangles = static_cast<uint32_t>(lod0.indices.size() / 3);
    chain.levels.push_back(base);

    std::vector<float> ratios = settings.ratios;
    std::sort(ratios.begin(), ratios.end(), std::greater<float>());
    uint32_t prevTris = base.triangles;
    for (float ratio : ratios)
    {
        if (!(ratio > 0.0f && ratio < 1.0f)) continue;
        StaticMeshAsset level;
        float err = 0.0f;
        if (!buildLevel(lod0, ratio, settings, level, err)) continue;
        const uint32_t tris = static_cast<uint32_t>(level.indices.size() / 3);
        // Could not be reduced (further): a copy of the level above is not a level.
        if (static_cast<float>(tris) > static_cast<float>(prevTris) * settings.minStepRatio) continue;
        Level l;
        l.triangles  = tris;
        l.worldError = err;
        // The error is measured against LOD0 and grows with each level; keep it so.
        if (chain.levels.size() > 1) l.worldError = std::max(l.worldError, chain.levels.back().worldError);
        chain.levels.push_back(l);
        prevTris = tris;
        if (meshes) meshes->push_back(std::move(level));
    }
    if (chain.levels.size() == 1)
        chain.error = "The mesh could not be reduced any further (too few triangles, or its open edges are locked).";
    return chain;
}

Chain generate(const StaticMeshAsset& lod0, const std::filesystem::path& contentRoot, const Settings& settings)
{
    std::vector<StaticMeshAsset> meshes;
    Chain chain = build(lod0, settings, &meshes);
    if (!chain.error.empty()) return chain;
    if (lod0.path.empty())
    {
        chain.error = "The mesh has no asset path to put the levels next to.";
        return chain;
    }
    std::string stem = lod0.path;
    {
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        if (stem.size() > 7 && stem.compare(stem.size() - 7, 7, kExt) == 0) stem.resize(stem.size() - 7);
    }
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        StaticMeshAsset& m = meshes[i];
        const int n = static_cast<int>(i) + 1;
        m.path = lodPath(lod0.path, n);
        m.name = stem + "_LOD" + std::to_string(n);
        m.type = HE::AssetType::StaticMesh;
        m.id   = HE::UUID{};      // writeAsset keeps the UUID of a file that is already there
        m.sourcePath.clear();     // a sidecar is not importable: no "Reimport" on LOD1
        if (!Importer::writeAsset(m, contentRoot))
        {
            chain.error = "Failed to write " + m.path;
            chain.levels.resize(i + 1);
            return chain;
        }
        Level& l = chain.levels[i + 1];
        l.path = m.path;
        l.id   = m.id;
    }
    HE_LOG_INFO(Tool, "%s",
        ("AutoLod: " + lod0.path + " -> " + std::to_string(chain.levels.size() - 1) + " level(s), "
         + std::to_string(chain.levels.front().triangles) + " -> "
         + std::to_string(chain.levels.back().triangles) + " tris").c_str());
    return chain;
}

} // namespace AutoLod
