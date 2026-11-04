#include <HorizonScene/RvtLandscape.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeHlodBaker.h>
#include <HorizonScene/RvtProducer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Renderer/IRenderer.h>
#include <functional>

namespace HE::landscape
{
namespace
{
    // The consumer material for one volume: a lit graph whose colour, normal, AO and roughness come from the RvtSample node.
    // The volume's two parameter vectors are baked in as the defaults of two Param Vector4 nodes.
    std::string consumerGraphJson(const rvt::VolumeLayout& l)
    {
        using HE::MatNodeType;
        const rvt::VolumeDesc& d = l.desc();
        HE::MaterialGraph g;
        const int out = g.addNode(MatNodeType::Output);                     // lit
        const int pa  = g.addNode(MatNodeType::ParamVec4); g.findNode(pa)->s = "RvtVolumeA";
        const int pb  = g.addNode(MatNodeType::ParamVec4); g.findNode(pb)->s = "RvtVolumeB";
        const int rv  = g.addNode(MatNodeType::RvtSample);
        g.findNode(pa)->p[0] = d.origin.x;           g.findNode(pa)->p[1] = d.origin.y;
        g.findNode(pa)->p[2] = l.extentM();          g.findNode(pa)->p[3] = l.texelMeters(0);
        g.findNode(pb)->p[0] = float(d.pageSize);    g.findNode(pb)->p[1] = float(d.border);
        g.findNode(pb)->p[2] = float(l.pagesPerSide(0)); g.findNode(pb)->p[3] = float(l.mipCount());
        g.connect(pa, 0, rv, 0);
        g.connect(pb, 0, rv, 1);
        g.connect(rv, 0, out, HE::kMatOutputBaseColorPin);
        g.connect(rv, 1, out, HE::kMatOutputNormalPin);
        g.connect(rv, 2, out, HE::kMatOutputAOPin);
        g.connect(rv, 3, out, HE::kMatOutputRoughnessPin);
        return HE::materialGraphToJson(g);
    }

    template <typename T> void mix(uint64_t& h, const T& v) { h ^= std::hash<T>{}(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); }
    void mixFloats(uint64_t& h, const std::vector<float>& v) { for (float f : v) mix(h, f); mix(h, v.size()); }
    void mixStrings(uint64_t& h, const std::vector<std::string>& v) { for (const std::string& s : v) mix(h, s); mix(h, v.size()); }
}

RvtLandscape::RvtLandscape(ContentManager& cm, IRenderer& renderer) : m_cm(cm), m_renderer(renderer) {}

RvtLandscape::~RvtLandscape() { detach(); }

uint64_t RvtLandscape::materialStateHash(HorizonWorld& live, entt::entity terrain) const
{
    // Everything the three view graphs are built from at attach time (HlodBaker::begin): the landscape material instance's
    // parameter block, which parameters it overrides and its static switches, plus the entity's own overrides.
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* mc = live.registry().try_get<MaterialComponent>(terrain);
    if (!mc) return h;
    mix(h, mc->materialAssetId.hi); mix(h, mc->materialAssetId.lo);
    if (const MaterialAsset* ma = m_cm.getMaterial(mc->materialAssetId))
    {
        mix(h, ma->parentMaterialPath);
        mixStrings(h, ma->graphParamNames);
        mixFloats(h, ma->shaderParamData);
        mixStrings(h, ma->instanceOverriddenParams);
        mixStrings(h, ma->instanceSwitchNames);
        for (float f : ma->instanceSwitchValues) mix(h, f);
        mix(h, ma->customShaderFragGlsl.size());
    }
    return h;
}

bool RvtLandscape::attach(HorizonWorld& live, entt::entity terrain)
{
    auto baker = std::make_unique<HlodBaker>(live, m_cm, m_renderer);
    std::string why;
    if (!baker->begin(terrain, &why, /*switchRenderer=*/false))
    {
        HE_LOG_INFO(Scene, "RvtLandscape: not attached (%s)", why.c_str());
        return false;
    }
    const TerrainComponent& tc = live.registry().get<TerrainComponent>(terrain);
    HlodChunkRect r0;
    baker->chunkRect(0, 0, r0);

    rvt::VolumeDesc vd;
    vd.origin = r0.min;
    vd.sizeM = tc.sizeX;
    vd.finestMetersPerTexel = m_settings.metersPerTexel;
    auto manager = std::make_unique<rvt::RvtManager>(m_settings.poolSlots, m_settings.maxPagesPerFrame, /*pinnedMips=*/1);
    const uint16_t vol = manager->addVolume(vd);
    const uint32_t px = static_cast<uint32_t>(manager->layout(vol).physicalPageSize());
    if (!m_renderer.CreateRvtPool(m_settings.poolSlots, px))
    {
        HE_LOG_WARN(Scene, "RvtLandscape: the renderer could not make a pool of %u pages", m_settings.poolSlots);
        return false;
    }

    MaterialAsset ca;
    ca.type = HE::AssetType::Material;
    ca.name = "RvtLandscapeConsumer";
    ca.nodeGraphJson = consumerGraphJson(manager->layout(vol));
    const HE::UUID consumer = m_cm.registerMaterial(std::move(ca));
    m_cm.regenerateMaterialFromGraph(consumer);
    m_renderer.WarmupMaterials({ consumer });

    m_baker = std::move(baker);
    m_manager = std::move(manager);
    m_volume = vol;
    m_pagePx = px;
    m_terrain = terrain;
    m_consumer = consumer;
    m_landscapeMaterial = live.registry().get<MaterialComponent>(terrain).materialAssetId;
    m_uploadedVersion = ~0ull;
    m_filled = false;
    m_attached = true;
    live.registry().get<TerrainComponent>(terrain).rvtJournal.clear();   // the scratch copy was made from the ground as it is now
    HE_LOG_INFO(Scene, "RvtLandscape: attached, %u pages of %u px, %d mips over %.0f m", m_settings.poolSlots, px,
                m_manager->layout(vol).mipCount(), vd.sizeM);
    return true;
}

void RvtLandscape::detach()
{
    if (m_active) m_renderer.SetRvtLandscape(HE::UUID{}, HE::UUID{});
    if (m_attached) m_renderer.DestroyRvtPool();
    m_active = false;
    m_attached = false;
    m_filled = false;
    m_manager.reset();
    m_baker.reset();
    m_terrain = entt::null;
    m_consumer = HE::UUID{};
    m_landscapeMaterial = HE::UUID{};
}

RvtLandscape::FrameStats RvtLandscape::tick(HorizonWorld& live, const glm::vec3& cameraPos)
{
    FrameStats st;
    if (!m_renderer.GetCapabilities().supportsRvt) return st;

    // The landscape: the first terrain (not a chunk) with a material.
    entt::entity land = entt::null;
    auto& reg = live.registry();
    for (auto [e, tc] : reg.view<TerrainComponent>().each())
        if (!reg.all_of<TerrainChunkComponent>(e) && reg.all_of<MaterialComponent>(e)) { land = e; break; }
    if (land == entt::null)
    {
        if (m_attached) detach();
        return st;
    }
    if (m_attached && (land != m_terrain || !reg.valid(m_terrain))) detach();
    const uint64_t mh = materialStateHash(live, land);
    if (m_attached && mh != m_materialHash)
    {
        // The view graphs were built from the old values: nothing to patch, start over.
        detach();
        st.rescanned = true;
    }
    if (!m_attached)
    {
        if (land == m_failedTerrain && mh == m_failedHash) return st;     // it failed for exactly this: not again every frame
        if (!attach(live, land)) { m_failedTerrain = land; m_failedHash = mh; return st; }
        m_materialHash = mh;
        m_failedTerrain = entt::null;
    }
    st.attached = true;

    // 2. the journal: scratch copy up to date, the pages under the change stale
    TerrainComponent& tc = reg.get<TerrainComponent>(land);
    if (tc.rvtJournal.any)
    {
        const TerrainComponent::RvtJournal j = tc.rvtJournal;
        const glm::vec3 pos = HE::worldPositionOf(live, land);
        if (j.all)
        {
            m_baker->syncTerrain(tc, true, glm::vec2(0.0f), glm::vec2(0.0f));
            st.pagesStale = m_manager->invalidateVolume(m_volume);
        }
        else
        {
            m_baker->syncTerrain(tc, false, glm::vec2(j.minX, j.minZ), glm::vec2(j.maxX, j.maxZ));
            st.pagesStale = m_manager->invalidateRect(m_volume, glm::vec2(pos.x + j.minX, pos.z + j.minZ),
                                                      glm::vec2(pos.x + j.maxX, pos.z + j.maxZ));
        }
        tc.rvtJournal.clear();
    }

    // 3. what the camera needs, stale first among equals; exactly that is painted
    rvt::ViewDesc view;
    view.cameraXZ = glm::vec2(cameraPos.x, cameraPos.z);
    view.nearRadiusM = m_settings.nearRadiusM;
    m_manager->beginFrame();
    const std::vector<rvt::PageRender> renders = m_manager->update(m_volume, view);
    if (!renders.empty())
    {
        m_baker->paintBegin();
        st.pagesPainted = rvt::paintPages(*m_baker, renders, m_pagePx);
        m_baker->paintEnd();
        m_paintedTotal += st.pagesPainted;
    }

    // 4. the page table, when a page came or went
    const uint64_t tv = m_manager->tableVersion(m_volume);
    if (tv != m_uploadedVersion)
    {
        std::vector<std::vector<uint32_t>> mips;
        for (int m = 0; m < m_manager->layout(m_volume).mipCount(); ++m) mips.push_back(m_manager->pageTable(m_volume, m));
        if (m_renderer.SetRvtPageTable(mips)) { m_uploadedVersion = tv; st.tableUploaded = true; }
    }

    // 5. the renderer draws from the pages once the first fill is complete (before that the coarse fallback would show a
    // blurry landscape for a few frames), and stays on afterwards: a stale or missing page degrades, it does not switch back
    if (!m_filled && m_uploadedVersion == tv && m_manager->stats().wantedMissing == 0) m_filled = true;
    if (m_filled != m_active)
    {
        m_renderer.SetRvtLandscape(m_filled ? m_landscapeMaterial : HE::UUID{}, m_filled ? m_consumer : HE::UUID{});
        m_active = m_filled;
    }
    st.active = m_active;
    return st;
}

} // namespace HE::landscape
