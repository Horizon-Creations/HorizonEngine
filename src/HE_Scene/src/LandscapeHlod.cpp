#include "HorizonScene/LandscapeHlod.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeHlodPlan.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureArrayBuild.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Types/Enums.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;

namespace HE::landscape
{
    namespace
    {
        constexpr uint16_t kTypeMaterial = static_cast<uint16_t>(HE::AssetType::Material);
        constexpr uint16_t kTypeTexture  = static_cast<uint16_t>(HE::AssetType::Texture);

        uint64_t mix(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h *= 0xFF51AFD7ED558CCDull;
            return h ^ (h >> 33);
        }

        std::string hexOf(const HE::UUID& id)
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(id.hi),
                          static_cast<unsigned long long>(id.lo));
            return buf;
        }

        bool readFile(const fs::path& p, std::vector<uint8_t>& out)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        }

        // The picture as a 2D texture with its whole mip chain (the 2x2 box filter every backend then samples
        // alike): a one-slice "array" is the engine's builder for that.
        bool makeTexture(const std::vector<uint8_t>& rgba, uint32_t side, bool srgb, const std::string& name,
                         const std::string& path, const HE::UUID& id, TextureAsset& out, std::string* why)
        {
            TextureAsset base;
            base.type = HE::AssetType::Texture;
            base.name = name; base.path = path; base.id = id;
            base.width = base.height = side; base.channels = 4; base.mipLevels = 1;
            base.format = ::TextureFormat::RGBA8; base.srgb = srgb;
            base.data = rgba;
            std::vector<const TextureAsset*> slices{ &base };
            TextureAsset built;
            if (!buildTextureArray(slices, built, /*bakeMips=*/true, why)) return false;
            built.type = HE::AssetType::Texture;
            built.name = name; built.path = path; built.id = id;
            out = std::move(built);
            return true;
        }

        MaterialGraph buildFarGraph(const std::string& albedoPath, const std::string& normalPath,
                                    const std::string& surfacePath, const HlodChunkRect& rect, uint32_t side)
        {
            MaterialGraph g;
            const int out = g.addNode(MatNodeType::Output);
            {
                MatGraphNode* o = g.findNode(out);
                o->p[0] = 1.0f;                                          // lit
                o->p[1] = static_cast<float>(MatBlendMode::Opaque);
            }
            auto constF = [&](float v) { const int n = g.addNode(MatNodeType::ConstFloat); g.findNode(n)->p[0] = v; return n; };
            auto op2 = [&](MatNodeType t, int a, int aPin, int b, int bPin) {
                const int n = g.addNode(t); g.connect(a, aPin, n, 0); g.connect(b, bPin, n, 1); return n; };

            // uv = (world.xz - rect.min) / rect.size, kept half a texel inside [0, 1] so a bilinear tap at the chunk's
            // border never reaches the other side of the (repeating) texture.
            const int wpos   = g.addNode(MatNodeType::WorldPos);
            const int wsplit = g.addNode(MatNodeType::SplitRGBA);
            g.connect(wpos, 0, wsplit, 0);
            const int zero = constF(0.0f);
            const int xz = g.addNode(MatNodeType::CombineRGBA);
            g.connect(wsplit, 0, xz, 0); g.connect(wsplit, 2, xz, 1); g.connect(zero, 0, xz, 2); g.connect(zero, 0, xz, 3);
            const int minC = g.addNode(MatNodeType::CombineRGBA);
            g.connect(constF(rect.min.x), 0, minC, 0); g.connect(constF(rect.min.y), 0, minC, 1);
            g.connect(zero, 0, minC, 2); g.connect(zero, 0, minC, 3);
            const int d  = op2(MatNodeType::Subtract, xz, 0, minC, 0);
            const float s = static_cast<float>(side);
            const float scale = (1.0f - 1.0f / s) / std::max(rect.size.x, 1e-3f);
            const int sc = op2(MatNodeType::Multiply, d, 0, constF(scale), 0);
            const int uv = op2(MatNodeType::Add, sc, 0, constF(0.5f / s), 0);

            auto sample = [&](const std::string& path) {
                const int n = g.addNode(MatNodeType::TextureSample);
                g.findNode(n)->s = path;
                g.connect(uv, 0, n, 0);
                return n;
            };
            const int albedo = sample(albedoPath);
            g.connect(albedo, 0, out, kMatOutputBaseColorPin);

            // world normal = rgb * 2 - 1
            const int nsmp  = sample(normalPath);
            const int n2    = op2(MatNodeType::Multiply, nsmp, 0, constF(2.0f), 0);
            const int n1    = op2(MatNodeType::Subtract, n2, 0, constF(1.0f), 0);
            const int nrm   = g.addNode(MatNodeType::Normalize3);
            g.connect(n1, 0, nrm, 0);
            g.connect(nrm, 0, out, kMatOutputNormalPin);

            const int ssmp  = sample(surfacePath);
            const int ssplit = g.addNode(MatNodeType::SplitRGBA);
            g.connect(ssmp, 0, ssplit, 0);
            g.connect(ssplit, 0, out, kMatOutputAOPin);          // R = AO
            g.connect(ssplit, 1, out, kMatOutputRoughnessPin);   // G = roughness
            g.connect(constF(0.0f), 0, out, kMatOutputMetallicPin);
            return g;
        }
    }

    HE::UUID hlodAssetId(const HE::UUID& terrainEntityId, uint32_t cx, uint32_t cz, HlodAsset what)
    {
        uint64_t a = mix(terrainEntityId.hi, 0x4C414E44ull /* LAND */);
        a = mix(a, (static_cast<uint64_t>(cx) << 32) | cz);
        a = mix(a, static_cast<uint64_t>(what));
        uint64_t b = mix(terrainEntityId.lo, 0x484C4F44ull /* HLOD */);
        b = mix(b, (static_cast<uint64_t>(cz) << 32) | cx);
        b = mix(b, static_cast<uint64_t>(what) + 7);
        HE::UUID id;
        id.hi = a; id.lo = b;
        return id;
    }

    std::string hlodAssetPath(const HE::UUID& terrainEntityId, uint32_t cx, uint32_t cz, HlodAsset what)
    {
        static const char* kNames[4] = { "material", "albedo", "normal", "surface" };
        return std::string(kHlodAssetFolder) + hexOf(terrainEntityId) + "_" + std::to_string(cx) + "_" + std::to_string(cz)
             + "_" + kNames[static_cast<uint32_t>(what)] + ".hasset";
    }

    bool buildHlodAssets(const HlodChunkImages& images, const HlodChunkRect& rect, const HE::UUID& terrainEntityId,
                         uint32_t cx, uint32_t cz, std::vector<HlodAssetBlob>& out, std::string* why)
    {
        auto fail = [&](const char* m) { if (why) *why = m; return false; };
        const uint32_t side = images.side;
        const size_t bytes = static_cast<size_t>(side) * side * 4;
        if (side == 0 || images.albedo.size() != bytes || images.normal.size() != bytes || images.surface.size() != bytes)
            return fail("the pictures do not match their side");

        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / ("he_hlod_" + hexOf(hlodAssetId(terrainEntityId, cx, cz, HlodAsset::Material)).substr(0, 16));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{ root };
        auto cm = std::make_unique<ContentManager>(root.string());

        const HlodAsset kinds[3] = { HlodAsset::Albedo, HlodAsset::Normal, HlodAsset::Surface };
        const std::vector<uint8_t>* pics[3] = { &images.albedo, &images.normal, &images.surface };
        const bool srgb[3] = { true, false, false };
        const char* names[3] = { "Landscape Far Albedo", "Landscape Far Normal", "Landscape Far Surface" };
        std::vector<HlodAssetBlob> blobs;
        std::string paths[3];
        HE::UUID ids[3];
        for (int i = 0; i < 3; ++i)
        {
            paths[i] = hlodAssetPath(terrainEntityId, cx, cz, kinds[i]);
            ids[i]   = hlodAssetId(terrainEntityId, cx, cz, kinds[i]);
            TextureAsset t;
            std::string err;
            if (!makeTexture(*pics[i], side, srgb[i], names[i], paths[i], ids[i], t, &err)) { if (why) *why = err; return false; }
            const fs::path file = root / paths[i];
            fs::create_directories(file.parent_path(), ec);
            if (!cm->writeAssetTo(t, file.string())) return fail("could not write a texture");
            HlodAssetBlob b;
            b.id = ids[i]; b.relPath = paths[i]; b.type = kTypeTexture;
            if (!readFile(file, b.bytes)) return fail("could not read a texture back");
            blobs.push_back(std::move(b));
        }

        // The material: the graph, regenerated through the loader so the GLSL, the G-buffer variant and the GI
        // approximation are what the editor would have saved (the mesh bake pass does the same).
        const MaterialGraph graph = buildFarGraph(paths[0], paths[1], paths[2], rect, side);
        const HE::MatShaderGen gen = HE::generateFragment(graph);
        MaterialAsset mat;
        mat.type = HE::AssetType::Material;
        mat.name = "Landscape Far Material";
        mat.path = hlodAssetPath(terrainEntityId, cx, cz, HlodAsset::Material);
        mat.id   = hlodAssetId(terrainEntityId, cx, cz, HlodAsset::Material);
        mat.nodeGraphJson = HE::materialGraphToJson(graph);
        mat.customShaderFragGlsl = gen.glsl;
        mat.customShaderGBufGlsl = gen.glslGBuffer;
        mat.customShaderVertGlsl = gen.vertexBody;
        mat.customShaderDispGlsl = gen.displacementBody;
        mat.blendMode = gen.blendMode;
        mat.graphTexturePaths = gen.textures;   // the pak pass turns the paths into ids, as it does for every material
        const fs::path mfile = root / mat.path;
        fs::create_directories(mfile.parent_path(), ec);
        if (!cm->writeAssetTo(mat, mfile.string())) return fail("could not write the material");
        // Load = regenerate from the graph; save that copy so the tail carries it all.
        const MaterialAsset* back = cm->getMaterial(cm->loadAsset(mat.path));
        if (back) { MaterialAsset copy = *back; if (!cm->writeAssetTo(copy, mfile.string())) return fail("could not rewrite the material"); }
        HlodAssetBlob mb;
        mb.id = mat.id; mb.relPath = mat.path; mb.type = kTypeMaterial;
        if (!readFile(mfile, mb.bytes)) return fail("could not read the material back");
        blobs.push_back(std::move(mb));

        for (HlodAssetBlob& b : blobs) out.push_back(std::move(b));
        return true;
    }

    HlodExportStats bakeWorldLandscapes(HorizonWorld& source, HorizonWorld* restoreTo, ContentManager& cm, IRenderer& renderer,
                                        const HlodExportOptions& options, std::vector<HlodAssetBlob>& out,
                                        std::vector<HE::UUID>& seen,
                                        const std::function<void(const std::string&)>& log,
                                        const std::function<bool()>& cancelled)
    {
        HlodExportStats stats;
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
            if (terrainId == HE::UUID{}) { ++stats.skipped; say("Far landscape: a landscape without an id was left as it is"); continue; }
            if (std::find(seen.begin(), seen.end(), terrainId) != seen.end()) continue;   // an earlier scene had it

            HlodBaker baker(source, cm, renderer, restoreTo);
            std::string why;
            if (!baker.begin(land, &why))
            {
                ++stats.skipped;
                say("Far landscape: left as it is (" + why + ")");
                continue;
            }
            seen.push_back(terrainId);

            // The size of the pictures: what the screen wants where the first far level takes over, capped, and
            // halved until the whole export's share of the budget holds (HlodBakePlan, the same planner the design
            // doc measured).
            const TerrainComponent& tc = reg.get<TerrainComponent>(land);
            const uint32_t cps = baker.chunksPerSide();
            const float chunkM = tc.sizeX / static_cast<float>(cps);
            HlodBakeInput in;
            in.tileM          = chunkM;
            in.levels         = 1;
            in.nearM          = chunkM * 6.0f * std::max(0.1f, tc.lodDistanceScale);   // where TerrainSystem's LOD 1 starts
            in.fovYDeg        = options.fovYDeg;
            in.screenHeightPx = options.screenHeightPx;
            in.maxTexels      = options.maxTexels;
            in.minTexels      = options.minTexels;
            in.tileCount      = static_cast<uint64_t>(cps) * cps;
            in.budgetBytes    = options.budgetBytes > spent ? options.budgetBytes - spent : (options.budgetBytes ? 1 : 0);
            const HlodBakePlan plan = planHlodBake(in);
            const uint32_t side = static_cast<uint32_t>(plan.levels[0].texels);
            stats.plannedBytes += plan.totalBytes;
            spent += plan.totalBytes;
            if (plan.levels[0].downscaled || !plan.withinBudget) ++stats.downscaled;

            uint32_t baked = 0;
            for (uint32_t cz = 0; cz < cps; ++cz)
                for (uint32_t cx = 0; cx < cps; ++cx)
                {
                    if (cancelled && cancelled()) break;
                    HlodChunkImages img;
                    HlodChunkRect rect;
                    std::string err;
                    if (baker.bake(cx, cz, side, static_cast<uint32_t>(std::max(1, options.supersample)), img)
                        && baker.chunkRect(cx, cz, rect)
                        && buildHlodAssets(img, rect, terrainId, cx, cz, out, &err))
                        ++baked;
                    else
                        say("Far landscape: chunk " + std::to_string(cx) + "," + std::to_string(cz) + " not baked (" + err + ")");
                }
            stats.chunks += baked;
            ++stats.terrains;
            say("Far landscape: " + std::to_string(baked) + " of " + std::to_string(cps * cps) + " chunk(s) baked at "
                + std::to_string(side) + " px (" + std::to_string(plan.levels[0].rasterM).substr(0, 5) + " m per texel, "
                + (plan.levels[0].downscaled ? "below what the screen would want" : "what the screen wants")
                + "), about " + std::to_string(plan.totalBytes >> 10) + " KiB cooked");
        }
        stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return stats;
    }

    size_t loadHlodAssets(ContentManager& cm, const std::vector<HlodAssetBlob>& blobs)
    {
        size_t n = 0;
        for (const HlodAssetBlob& b : blobs)
            if (cm.loadAssetFromMemory(b.bytes) != HE::UUID{}) ++n;
        return n;
    }
}
