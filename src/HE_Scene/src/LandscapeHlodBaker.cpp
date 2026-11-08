#include "HorizonScene/LandscapeHlodBaker.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TerrainSystem.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Renderer/IRenderer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace HE::landscape
{
    namespace
    {
        // Applies the landscape instance's own parameter values and switch overrides to a freshly built view
        // graph, so the picture shows THIS landscape's material and not the group's defaults.
        void applyInstanceOverrides(MaterialGraph& g, const std::vector<std::string>& names, const std::vector<float>& data,
                                    const std::vector<std::string>& overridden,
                                    const std::vector<std::string>& switchNames, const std::vector<uint8_t>& switchValues)
        {
            for (MatGraphNode& n : g.nodes)
            {
                if (n.type == MatNodeType::ParamFloat || n.type == MatNodeType::ParamColor ||
                    n.type == MatNodeType::ParamVec4 || n.type == MatNodeType::ParamVec2 || n.type == MatNodeType::ParamBool)
                {
                    if (std::find(overridden.begin(), overridden.end(), n.s) == overridden.end()) continue;
                    const auto it = std::find(names.begin(), names.end(), n.s);
                    if (it == names.end()) continue;
                    const size_t slot = static_cast<size_t>(it - names.begin());
                    if (slot * 4 + 3 >= data.size()) continue;
                    for (int c = 0; c < 4; ++c) n.p[c] = data[slot * 4 + static_cast<size_t>(c)];
                }
                else if (n.type == MatNodeType::StaticSwitch)
                {
                    for (size_t i = 0; i < switchNames.size() && i < switchValues.size(); ++i)
                        if (switchNames[i] == n.s) n.p[0] = switchValues[i] ? 1.0f : 0.0f;
                }
            }
        }

        // Lowest and highest local y of a chunk's mesh (SoA or cooked).
        bool meshHeightRange(const ContentManager& cm, const HE::UUID& id, float& lo, float& hi)
        {
            const StaticMeshAsset* m = cm.getStaticMesh(id);
            if (!m) return false;
            bool any = false;
            if (m->cooked)
            {
                for (size_t i = 1; i < m->interleaved.size(); i += 8) { lo = std::min(lo, m->interleaved[i]); hi = std::max(hi, m->interleaved[i]); any = true; }
            }
            else
            {
                for (size_t i = 1; i < m->vertices.size(); i += 3) { lo = std::min(lo, m->vertices[i]); hi = std::max(hi, m->vertices[i]); any = true; }
            }
            return any;
        }
    }

    std::vector<uint8_t> boxDownsampleRgba8(const std::vector<uint8_t>& src, uint32_t side, uint32_t ss)
    {
        if (ss <= 1) return src;
        std::vector<uint8_t> out(static_cast<size_t>(side) * side * 4);
        const uint32_t w = side * ss;
        const uint32_t area = ss * ss;
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x)
            {
                uint32_t acc[4] = { 0, 0, 0, 0 };
                for (uint32_t dy = 0; dy < ss; ++dy)
                    for (uint32_t dx = 0; dx < ss; ++dx)
                    {
                        const uint8_t* p = &src[(static_cast<size_t>(y * ss + dy) * w + (x * ss + dx)) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    }
                uint8_t* o = &out[(static_cast<size_t>(y) * side + x) * 4];
                for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((acc[c] + area / 2) / area);
            }
        return out;
    }

    HlodBaker::HlodBaker(HorizonWorld& source, ContentManager& cm, IRenderer& renderer, HorizonWorld* restoreTo)
        : m_live(source), m_restore(restoreTo ? *restoreTo : source), m_cm(cm), m_renderer(renderer) {}

    HlodBaker::~HlodBaker() { end(); }

    bool HlodBaker::begin(entt::entity terrain, std::string* why)
    {
        auto fail = [&](const char* msg) { if (why) *why = msg; return false; };
        end();
        auto& reg = m_live.registry();
        if (!reg.valid(terrain)) return fail("no such entity");
        const auto* tc  = reg.try_get<TerrainComponent>(terrain);
        const auto* tfc = reg.try_get<TransformComponent>(terrain);
        const auto* mc  = reg.try_get<MaterialComponent>(terrain);
        if (!tc || !tfc) return fail("not a landscape");
        if (!mc || mc->materialAssetId == HE::UUID{}) return fail("the landscape uses the default material");
        if (tfc->rotation != glm::vec3(0.0f) || tfc->scale != glm::vec3(1.0f))
            return fail("a rotated or scaled landscape is not baked");

        // Everything read off the live material BEFORE any loadAsset (the next load invalidates getter pointers).
        const HE::UUID matId = mc->materialAssetId;
        m_cm.ensureResident(matId);   // a scene loaded for the export has not pulled its materials in
        std::string parentPath;
        std::vector<std::string> names, overridden, swNames;
        std::vector<float> data;
        std::vector<uint8_t> swValues;
        if (const MaterialAsset* ma = m_cm.getMaterial(matId))
        {
            parentPath = ma->parentMaterialPath;
            names = ma->graphParamNames;
            data = ma->shaderParamData;
            overridden = ma->instanceOverriddenParams;
            swNames = ma->instanceSwitchNames;
            swValues = ma->instanceSwitchValues;
        }
        else
            return fail("material not resident");
        HE::UUID baseId = matId;
        if (!parentPath.empty()) baseId = m_cm.loadAsset(parentPath);
        m_group = autoLandscapeGroupOfMaterial(baseId);
        if (m_group < 0) return fail("the landscape material is not an Auto Landscape material");

        // The three views, with this landscape's parameters.
        const AutoLandscapeView views[3] = { AutoLandscapeView::BaseColor, AutoLandscapeView::Normal, AutoLandscapeView::Surface };
        const char* viewNames[3] = { "HlodBakeBaseColor", "HlodBakeNormal", "HlodBakeSurface" };
        for (int v = 0; v < 3; ++v)
        {
            AutoLandscapeGraph built = buildAutoLandscapeGraph(views[v], static_cast<AutoLandscapeGroupId>(m_group));
            applyInstanceOverrides(built.graph, names, data, overridden, swNames, swValues);
            MaterialAsset am;
            am.type = HE::AssetType::Material;
            am.name = viewNames[v];
            am.nodeGraphJson = materialGraphToJson(built.graph);
            m_viewMat[v] = m_cm.registerMaterial(std::move(am));
            m_cm.regenerateMaterialFromGraph(m_viewMat[v]);
        }

        // The scratch world: one landscape at the live one's world position, the chunks built the ordinary way.
        m_scratch = std::make_unique<HorizonWorld>();
        auto& sreg = m_scratch->registry();
        m_terrain = m_scratch->createEntity("HlodBakeLandscape");
        TransformComponent stf = *tfc;
        stf.position = HE::worldPositionOf(m_live, terrain);
        stf.dirty = true;
        sreg.emplace_or_replace<TransformComponent>(m_terrain, stf);
        TerrainComponent copy = *tc;
        copy.dirty = true;
        sreg.emplace<TerrainComponent>(m_terrain, std::move(copy));
        sreg.emplace<MaterialComponent>(m_terrain, MaterialComponent{ m_viewMat[0] });
        TerrainSystem::updateTerrains(*m_scratch, m_cm, &m_renderer);
        HE::propagateTransformsFull(*m_scratch);

        // Chunk rectangles (world XZ) and the height range the camera has to cover.
        const TerrainComponent& stc = sreg.get<TerrainComponent>(m_terrain);
        uint32_t maxC = 0;
        for (auto [ce, cc] : sreg.view<TerrainChunkComponent>().each())
            maxC = std::max(maxC, std::max(cc.cx, cc.cz));
        m_chunksPerSide = maxC + 1;
        if (sreg.view<TerrainChunkComponent>().empty()) { end(); return fail("the landscape built no chunks"); }
        const float chunkX = stc.sizeX / static_cast<float>(m_chunksPerSide);
        const float chunkZ = stc.sizeZ / static_cast<float>(m_chunksPerSide);
        if (std::fabs(chunkX - chunkZ) > 1e-3f) { end(); return fail("non-square chunks are not baked"); }
        m_rects.assign(static_cast<size_t>(m_chunksPerSide) * m_chunksPerSide, HlodChunkRect{});
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (auto [ce, cc, tf] : sreg.view<TerrainChunkComponent, TransformComponent>().each())
        {
            HlodChunkRect r;
            r.size = glm::vec2(chunkX, chunkZ);
            r.min  = glm::vec2(stf.position.x - stc.sizeX * 0.5f + static_cast<float>(cc.cx) * chunkX,
                               stf.position.z - stc.sizeZ * 0.5f + static_cast<float>(cc.cz) * chunkZ);
            m_rects[static_cast<size_t>(cc.cz) * m_chunksPerSide + cc.cx] = r;
            if (const auto* mesh = sreg.try_get<MeshComponent>(ce))
                meshHeightRange(m_cm, mesh->meshAssetId, lo, hi);
        }
        if (lo > hi) { end(); return fail("the chunk meshes carry no heights"); }
        m_bottomY = stf.position.y + lo;
        m_topY    = stf.position.y + hi;

        // The three pipelines build in the background; a view that is not ready yet is simply not drawn.
        m_renderer.WarmupMaterials({ m_viewMat[0], m_viewMat[1], m_viewMat[2] });
        const auto t0 = std::chrono::steady_clock::now();
        for (;;)
        {
            const IRenderer::ShaderBuildStatus st = m_renderer.PumpShaderBuilds();
            if (st.pending == 0) break;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) { end(); return fail("the view pipelines did not finish building"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        m_renderer.SetWorld(m_scratch.get());
        m_active = true;
        return true;
    }

    void HlodBaker::end()
    {
        if (m_active) m_renderer.SetWorld(&m_restore);
        m_active = false;
        m_scratch.reset();
        m_terrain = entt::null;
        m_rects.clear();
        m_chunksPerSide = 0;
    }

    bool HlodBaker::chunkRect(uint32_t cx, uint32_t cz, HlodChunkRect& out) const
    {
        if (cx >= m_chunksPerSide || cz >= m_chunksPerSide) return false;
        out = m_rects[static_cast<size_t>(cz) * m_chunksPerSide + cx];
        return true;
    }

    bool HlodBaker::renderView(const HE::UUID& material, const HlodChunkRect& rect, uint32_t px, bool srgb,
                               std::vector<uint8_t>& rgba)
    {
        auto& reg = m_scratch->registry();
        reg.get<MaterialComponent>(m_terrain).materialAssetId = material;
        for (auto [ce, cc, mc] : reg.view<TerrainChunkComponent, MaterialComponent>().each())
            mc.materialAssetId = material;
        m_renderer.InvalidateMaterial(material);

        EditorCameraOverride cam;
        cam.active          = true;
        cam.orthographic    = true;
        cam.rawOutput       = true;
        cam.rawSrgb         = srgb;
        cam.editorIcons     = false;
        cam.orthoHalfHeight = rect.size.y * 0.5f;
        const glm::vec3 eye(rect.min.x + rect.size.x * 0.5f, m_topY + 10.0f, rect.min.y + rect.size.y * 0.5f);
        cam.position  = eye;
        cam.view      = glm::lookAt(eye, eye + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        cam.nearPlane = 0.1f;
        cam.farPlane  = (m_topY - m_bottomY) + 30.0f;
        return m_renderer.RenderSceneImage(cam, px, px, rgba) && rgba.size() == static_cast<size_t>(px) * px * 4;
    }

    bool HlodBaker::bake(uint32_t cx, uint32_t cz, uint32_t side, uint32_t supersample, HlodChunkImages& out)
    {
        HlodChunkRect rect;
        if (!chunkRect(cx, cz, rect)) return false;
        return bakeRect(rect, side, supersample, out);
    }

    bool HlodBaker::bakeRect(const HlodChunkRect& rect, uint32_t side, uint32_t supersample, HlodChunkImages& out)
    {
        if (!m_active || side == 0 || std::fabs(rect.size.x - rect.size.y) > 1e-3f) return false;
        const uint32_t ss = std::max(1u, supersample);
        const uint32_t px = side * ss;
        std::vector<uint8_t> a, n, s;
        if (!renderView(m_viewMat[0], rect, px, /*srgb=*/true,  a)) return false;
        if (!renderView(m_viewMat[1], rect, px, /*srgb=*/false, n)) return false;
        if (!renderView(m_viewMat[2], rect, px, /*srgb=*/false, s)) return false;
        out.side    = side;
        out.albedo  = boxDownsampleRgba8(a, side, ss);
        out.normal  = boxDownsampleRgba8(n, side, ss);
        out.surface = boxDownsampleRgba8(s, side, ss);
        return true;
    }
}
