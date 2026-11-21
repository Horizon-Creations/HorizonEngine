#include "HorizonScene/RvtBaseBake.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeHlodBaker.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Types/Enums.h>
#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;

namespace HE::landscape
{
    namespace
    {
        uint64_t mix(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h *= 0xFF51AFD7ED558CCDull;
            return h ^ (h >> 33);
        }
        uint64_t mixF(uint64_t h, float f)
        {
            uint32_t u; std::memcpy(&u, &f, sizeof u);
            return mix(h, u);
        }
        // A word-at-a-time hash of a buffer (a sculpted landscape's heights are megabytes): cheap, not cryptographic.
        uint64_t hashBytes(uint64_t h, const void* data, size_t n)
        {
            const unsigned char* p = static_cast<const unsigned char*>(data);
            h = mix(h, n);
            size_t i = 0;
            for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
            uint64_t tail = 0;
            if (i < n) { std::memcpy(&tail, p + i, n - i); h = (h ^ tail) * 0x100000001B3ull; }
            return h;
        }
        std::string hexOf(uint64_t hi, uint64_t lo)
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
            return buf;
        }
        bool readFile(const fs::path& p, std::vector<uint8_t>& out)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        }
        constexpr uint16_t kTypeTexture = static_cast<uint16_t>(HE::AssetType::Texture);
        const char* const kKindNames[3] = { "rvt_albedo", "rvt_normal", "rvt_surface" };
    }

    HE::UUID rvtBaseAssetId(const HE::UUID& terrainEntityId, uint64_t key, RvtBaseAsset what)
    {
        uint64_t a = mix(terrainEntityId.hi, 0x52565442ull /* RVTB */);
        a = mix(a, key);
        a = mix(a, static_cast<uint64_t>(what) + 1);
        uint64_t b = mix(terrainEntityId.lo, 0x42415345ull /* BASE */);
        b = mix(b, ~key);
        b = mix(b, static_cast<uint64_t>(what) + 11);
        HE::UUID id;
        id.hi = a; id.lo = b;
        return id;
    }

    std::string rvtBaseAssetPath(const HE::UUID& terrainEntityId, uint64_t key, RvtBaseAsset what)
    {
        char k[24];
        std::snprintf(k, sizeof k, "%016llx", static_cast<unsigned long long>(key));
        return std::string(kHlodAssetFolder) + hexOf(terrainEntityId.hi, terrainEntityId.lo) + "_" + k + "_"
             + kKindNames[static_cast<uint32_t>(what)] + ".hasset";
    }

    MatRvtEnv rvtMaterialEnv(const MaterialAsset& ma, const MaterialComponent* mc)
    {
        MatRvtEnv env;
        for (size_t i = 0; i < ma.graphParamNames.size(); ++i)
        {
            std::array<float, 4> v{ 0, 0, 0, 0 };
            for (size_t c = 0; c < 4; ++c) if (i * 4 + c < ma.shaderParamData.size()) v[c] = ma.shaderParamData[i * 4 + c];
            env.params[ma.graphParamNames[i]] = v;
        }
        if (mc)
            for (const MaterialParamOverride& ov : mc->paramOverrides)
                env.params[ov.name] = { ov.value[0], ov.value[1], ov.value[2], ov.value[3] };
        for (size_t i = 0; i < ma.instanceSwitchNames.size() && i < ma.instanceSwitchValues.size(); ++i)
            env.switches[ma.instanceSwitchNames[i]] = ma.instanceSwitchValues[i] != 0;
        return env;
    }

    uint64_t rvtBaseKey(ContentManager& cm, HorizonWorld& world, entt::entity terrain)
    {
        auto& reg = world.registry();
        if (!reg.valid(terrain)) return 0;
        const auto* mc = reg.try_get<MaterialComponent>(terrain);
        const auto* tc = reg.try_get<TerrainComponent>(terrain);
        if (!mc || !tc || mc->materialAssetId == HE::UUID{}) return 0;
        cm.ensureResident(mc->materialAssetId);
        // Everything off the instance first: the next loadAsset (the parent) invalidates the getter pointer.
        MatRvtEnv env;
        std::string parentPath;
        std::string json;
        if (const MaterialAsset* ma = cm.getMaterial(mc->materialAssetId))
        {
            env = rvtMaterialEnv(*ma, mc);
            parentPath = ma->parentMaterialPath;
            json = ma->nodeGraphJson;
        }
        else
            return 0;
        if (!parentPath.empty())
        {
            const HE::UUID pid = cm.loadAsset(parentPath);
            if (const MaterialAsset* pm = cm.getMaterial(pid)) json = pm->nodeGraphJson;
        }
        MaterialGraph graph;
        if (json.empty() || !materialGraphFromJson(json, graph)) return 0;
        const MatRvtSplit split = matSplitRvt(graph);
        if (!split.hasRvtNodes || !split.ok()) return 0;
        const MatRvtMaterialState ms = matRvtMaterialState(graph, split, env);   // no asset revisions: see the header
        const glm::vec3 pos = HE::worldPositionOf(world, terrain);
        uint64_t h = mix(0x52565442415345ull, ms.state);
        h = mixF(h, tc->sizeX); h = mixF(h, tc->sizeZ); h = mixF(h, pos.x); h = mixF(h, pos.z);
        // The ground the pictures were taken of: what the pages read of it (heights, layer weights). A script that moved a
        // vertex or painted a layer before the first frame of the game gives another key, so the pictures are not used.
        h = mix(h, tc->resolution); h = mixF(h, tc->heightScale); h = mix(h, static_cast<uint64_t>(static_cast<int64_t>(tc->seed)));
        h = mix(h, static_cast<uint64_t>(tc->octaves)); h = mixF(h, tc->frequency); h = mixF(h, tc->lacunarity); h = mixF(h, tc->gain);
        h = mixF(h, tc->uvTiling); h = mix(h, tc->heightmapTexture.hi); h = mix(h, tc->heightmapTexture.lo);
        h = mix(h, tc->weightRes); h = mix(h, tc->groupMix ? 1u : 0u); h = mix(h, static_cast<uint64_t>(tc->baseGroup + 1));
        h = hashBytes(h, tc->sculptHeights.data(), tc->sculptHeights.size() * sizeof(float));
        h = hashBytes(h, tc->layerWeights.data(), tc->layerWeights.size());
        h = hashBytes(h, tc->layerWeights2.data(), tc->layerWeights2.size());
        h = hashBytes(h, tc->layerWeights3.data(), tc->layerWeights3.size());
        return h ? h : 1;
    }

    uint32_t rvtBaseSide(float sizeM, const RvtBaseOptions& o, uint64_t budgetLeft, bool* downscaled)
    {
        const float wanted = std::max(1.0f, sizeM / std::max(0.01f, o.metersPerTexel));
        uint32_t side = 1;
        while (static_cast<float>(side) < wanted) side <<= 1;                       // the live page resolution, rounded up
        const uint32_t cap = static_cast<uint32_t>(std::max(16, o.maxTexels));
        uint32_t capP = 1; while (capP * 2 <= cap) capP <<= 1;                      // largest power of two <= maxTexels
        const uint32_t lo = static_cast<uint32_t>(std::max(16, o.minTexels));
        side = std::min(side, capP);
        if (o.budgetBytes)
            while (side > lo && rvtBaseCookedBytes(side) > budgetLeft) side >>= 1;
        if (downscaled) *downscaled = static_cast<float>(side) < wanted;
        return side;
    }

    bool buildRvtBaseAssets(const HlodChunkImages& images, const HE::UUID& terrainEntityId, uint64_t key,
                            std::vector<HlodAssetBlob>& out, std::string* why)
    {
        auto fail = [&](const char* m) { if (why) *why = m; return false; };
        const uint32_t side = images.side;
        const size_t bytes = static_cast<size_t>(side) * side * 4;
        if (side == 0 || images.albedo.size() != bytes || images.normal.size() != bytes || images.surface.size() != bytes)
            return fail("the pictures do not match their side");

        std::error_code ec;
        const HE::UUID tag = rvtBaseAssetId(terrainEntityId, key, RvtBaseAsset::Albedo);
        const fs::path root = fs::temp_directory_path(ec) / ("he_rvtbase_" + hexOf(tag.hi, tag.lo).substr(0, 16));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{ root };
        auto cm = std::make_unique<ContentManager>(root.string());

        const std::vector<uint8_t>* pics[3] = { &images.albedo, &images.normal, &images.surface };
        const bool srgb[3] = { true, false, false };
        const char* names[3] = { "Landscape Base Albedo", "Landscape Base Normal", "Landscape Base Surface" };
        std::vector<HlodAssetBlob> blobs;
        for (int i = 0; i < 3; ++i)
        {
            const RvtBaseAsset kind = static_cast<RvtBaseAsset>(i);
            TextureAsset t;
            t.type = HE::AssetType::Texture;
            t.name = names[i];
            t.path = rvtBaseAssetPath(terrainEntityId, key, kind);
            t.id   = rvtBaseAssetId(terrainEntityId, key, kind);
            t.width = t.height = side; t.channels = 4; t.mipLevels = 1;
            t.format = ::TextureFormat::RGBA8; t.srgb = srgb[i];
            t.data = *pics[i];
            const fs::path file = root / t.path;
            fs::create_directories(file.parent_path(), ec);
            if (!cm->writeAssetTo(t, file.string())) return fail("could not write a texture");
            HlodAssetBlob b;
            b.id = t.id; b.relPath = t.path; b.type = kTypeTexture;
            if (!readFile(file, b.bytes)) return fail("could not read a texture back");
            blobs.push_back(std::move(b));
        }
        for (HlodAssetBlob& b : blobs) out.push_back(std::move(b));
        return true;
    }

    MaterialGraph rvtBaseViewGraph(const std::string& texturePath, glm::vec2 rectMin, float sizeM)
    {
        MaterialGraph g;
        const int out = g.addNode(MatNodeType::Output);
        g.findNode(out)->p[0] = 0.0f;                                   // unlit: the frame IS the value
        auto constF = [&](float v) { const int n = g.addNode(MatNodeType::ConstFloat); g.findNode(n)->p[0] = v; return n; };
        auto op2 = [&](MatNodeType t, int a, int aPin, int b, int bPin) {
            const int n = g.addNode(t); g.connect(a, aPin, n, 0); g.connect(b, bPin, n, 1); return n; };

        // uv = (world.xz - rectMin) / size: the picture covers the landscape edge to edge, texel i's centre at (i + 0.5) / side,
        // the way the bake rendered it.
        const int wpos   = g.addNode(MatNodeType::WorldPos);
        const int wsplit = g.addNode(MatNodeType::SplitRGBA);
        g.connect(wpos, 0, wsplit, 0);
        const int zero = constF(0.0f);
        const int xz = g.addNode(MatNodeType::CombineRGBA);
        g.connect(wsplit, 0, xz, 0); g.connect(wsplit, 2, xz, 1); g.connect(zero, 0, xz, 2); g.connect(zero, 0, xz, 3);
        const int minC = g.addNode(MatNodeType::CombineRGBA);
        g.connect(constF(rectMin.x), 0, minC, 0); g.connect(constF(rectMin.y), 0, minC, 1);
        g.connect(zero, 0, minC, 2); g.connect(zero, 0, minC, 3);
        const int d  = op2(MatNodeType::Subtract, xz, 0, minC, 0);
        const int uv = op2(MatNodeType::Multiply, d, 0, constF(1.0f / std::max(sizeM, 1e-3f)), 0);
        const int smp = g.addNode(MatNodeType::TextureSample);
        g.findNode(smp)->s = texturePath;
        g.connect(uv, 0, smp, 0);
        g.connect(smp, 0, out, kMatOutputBaseColorPin);
        return g;
    }

    bool rvtBaseViewMaterials(ContentManager& cm, HorizonWorld& world, entt::entity terrain, uint64_t key, HE::UUID (&views)[3])
    {
        auto& reg = world.registry();
        const auto* tc = reg.try_get<TerrainComponent>(terrain);
        if (!tc || key == 0) return false;
        const HE::UUID tid = world.entityId(terrain);
        if (tid == HE::UUID{}) return false;
        uint32_t side = 0;
        for (int i = 0; i < 3; ++i)
        {
            const HE::UUID id = rvtBaseAssetId(tid, key, static_cast<RvtBaseAsset>(i));
            if (!cm.ensureResident(id)) return false;                     // a miss is a hash lookup and says nothing
            const TextureAsset* t = cm.getTexture(id);
            if (!t || t->width == 0 || t->width != t->height) return false;
            if (i == 0) side = t->width;
            else if (t->width != side) return false;
        }
        const glm::vec3 pos = HE::worldPositionOf(world, terrain);
        const glm::vec2 mn(pos.x - tc->sizeX * 0.5f, pos.z - tc->sizeZ * 0.5f);
        static const char* const kViewNames[3] = { "RvtBaseBaseColor", "RvtBaseNormal", "RvtBaseSurface" };
        for (int i = 0; i < 3; ++i)
        {
            MaterialAsset am;
            am.type = HE::AssetType::Material;
            am.name = kViewNames[i];
            am.nodeGraphJson = materialGraphToJson(rvtBaseViewGraph(rvtBaseAssetPath(tid, key, static_cast<RvtBaseAsset>(i)), mn, tc->sizeX));
            views[i] = cm.registerMaterial(std::move(am));
            cm.regenerateMaterialFromGraph(views[i]);
        }
        return true;
    }

    RvtBaseStats bakeRvtBaseWorld(HorizonWorld& source, HorizonWorld* restoreTo, ContentManager& cm, IRenderer& renderer,
                                  const RvtBaseOptions& options, std::vector<HlodAssetBlob>& out, std::vector<HE::UUID>& seen,
                                  const std::function<void(const std::string&)>& log, const std::function<bool()>& cancelled)
    {
        RvtBaseStats stats;
        const auto t0 = std::chrono::steady_clock::now();
        const auto say = [&](const std::string& s) { if (log) log(s); };
        auto& reg = source.registry();
        std::vector<entt::entity> lands;
        for (auto [e, tc] : reg.view<TerrainComponent>().each())
            if (!reg.all_of<TerrainChunkComponent>(e)) lands.push_back(e);

        uint64_t spent = 0;
        for (const entt::entity land : lands)
        {
            if (cancelled && cancelled()) break;
            const HE::UUID terrainId = source.entityId(land);
            if (terrainId == HE::UUID{}) continue;
            if (std::find(seen.begin(), seen.end(), terrainId) != seen.end()) continue;   // an earlier scene had it
            const uint64_t key = rvtBaseKey(cm, source, land);
            if (key == 0) continue;                                                       // no RVT Write / Read: nothing to bake
            const TerrainComponent& tc = reg.get<TerrainComponent>(land);
            if (std::fabs(tc.sizeX - tc.sizeZ) > 1e-3f) { ++stats.skipped; say("RVT base: left as it is (a landscape that is not square)"); continue; }

            const uint64_t left = options.budgetBytes > spent ? options.budgetBytes - spent : (options.budgetBytes ? 1 : 0);
            bool down = false;
            const uint32_t side = rvtBaseSide(tc.sizeX, options, left, &down);
            const uint64_t planned = rvtBaseCookedBytes(side);
            if (options.budgetBytes && planned > left) { ++stats.skipped; say("RVT base: left as it is (the budget is spent)"); continue; }

            HlodBaker baker(source, cm, renderer, restoreTo);
            std::string why;
            if (!baker.begin(land, &why, /*switchRenderer=*/true, /*rvtPages=*/true))
            {
                ++stats.skipped;
                say("RVT base: left as it is (" + why + ")");
                continue;
            }
            seen.push_back(terrainId);

            // The whole landscape in tiles of `tileTexels`: one render call per tile, composed into one picture per layer.
            const glm::vec3 pos = HE::worldPositionOf(source, land);
            const glm::vec2 mn(pos.x - tc.sizeX * 0.5f, pos.z - tc.sizeZ * 0.5f);
            const uint32_t tile = std::min<uint32_t>(side, static_cast<uint32_t>(std::max(64, options.tileTexels)));
            const uint32_t tiles = (side + tile - 1) / tile;
            HlodChunkImages full;
            full.side = side;
            const size_t bytes = static_cast<size_t>(side) * side * 4;
            full.albedo.assign(bytes, 0); full.normal.assign(bytes, 0); full.surface.assign(bytes, 0);
            bool ok = true;
            const float tileM = tc.sizeX * static_cast<float>(tile) / static_cast<float>(side);
            for (uint32_t ty = 0; ty < tiles && ok; ++ty)
                for (uint32_t tx = 0; tx < tiles && ok; ++tx)
                {
                    if (cancelled && cancelled()) { ok = false; break; }
                    HlodChunkRect rect;
                    rect.min  = mn + glm::vec2(static_cast<float>(tx), static_cast<float>(ty)) * tileM;
                    rect.size = glm::vec2(tileM);
                    HlodChunkImages img;
                    if (!baker.bakeRect(rect, tile, 1, img)) { ok = false; break; }
                    const uint32_t w = std::min(tile, side - tx * tile), h = std::min(tile, side - ty * tile);
                    auto blit = [&](const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
                        for (uint32_t y = 0; y < h; ++y)
                            std::memcpy(&dst[(static_cast<size_t>(ty * tile + y) * side + tx * tile) * 4],
                                        &src[static_cast<size_t>(y) * tile * 4], static_cast<size_t>(w) * 4);
                    };
                    blit(img.albedo, full.albedo); blit(img.normal, full.normal); blit(img.surface, full.surface);
                }
            std::string err;
            if (ok && buildRvtBaseAssets(full, terrainId, key, out, &err))
            {
                ++stats.landscapes;
                if (down) ++stats.downscaled;
                stats.plannedBytes += planned;
                spent += planned;
                say("RVT base: " + std::to_string(side) + " x " + std::to_string(side) + " px for " + std::to_string(static_cast<int>(tc.sizeX))
                    + " m (" + std::to_string(tc.sizeX / static_cast<float>(side)).substr(0, 5) + " m per texel, "
                    + (down ? "below the live pages' resolution" : "the live pages' resolution") + "), about "
                    + std::to_string(planned >> 10) + " KiB cooked, key " + std::to_string(key));
            }
            else
            {
                ++stats.skipped;
                say("RVT base: not baked (" + (err.empty() ? std::string("a tile could not be rendered") : err) + ")");
            }
        }
        stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return stats;
    }
}
