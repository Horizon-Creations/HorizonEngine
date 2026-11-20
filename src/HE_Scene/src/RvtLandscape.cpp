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
#include <algorithm>
#include <chrono>
#include <Project/ProjectSettings.h>   // ProjectRvtSettings
#include <cmath>
#include <functional>
#include <set>
#include <vector>

namespace HE::landscape
{
namespace
{
    // The consumer material for one volume: a lit graph whose colour, normal, AO and roughness come from the RvtSample node.
    // The volume's two parameter vectors are baked in as the defaults of two Param Vector4 nodes.
    std::string consumerGraphJson(const rvt::VolumeLayout& l, uint16_t volume)
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
        // B.y = border + 256 * volume: the lookup's feedback reports the volume with the page (RvtLookup.h, rvtFeedbackKey).
        g.findNode(pb)->p[0] = float(d.pageSize);    g.findNode(pb)->p[1] = float(d.border) + 256.0f * float(volume);
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

    // Distance from a point to a rectangle (0 inside).
    float distToRect(glm::vec2 p, glm::vec2 mn, glm::vec2 mx)
    {
        const float dx = std::max({ mn.x - p.x, 0.0f, p.x - mx.x });
        const float dz = std::max({ mn.y - p.y, 0.0f, p.y - mx.y });
        return std::sqrt(dx * dx + dz * dz);
    }

    // Brings a scratch copy up to date with what the journal says changed in the live landscape (the journal is not cleared here).
    void syncScratch(HlodBaker& baker, const TerrainComponent& tc)
    {
        const TerrainComponent::RvtJournal& j = tc.rvtJournal;
        if (j.all) baker.syncTerrain(tc, true, glm::vec2(0.0f), glm::vec2(0.0f));
        else       baker.syncTerrain(tc, false, glm::vec2(j.minX, j.minZ), glm::vec2(j.maxX, j.maxZ));
    }
}

RvtLandscape::RvtLandscape(ContentManager& cm, IRenderer& renderer) : m_cm(cm), m_renderer(renderer) {}

RvtLandscape::~RvtLandscape() { detach(); }

void RvtLandscape::note(const std::string& why)
{
    // Says once per change why the driver is idle (a silent driver is indistinguishable from a broken one), and every 600th
    // call as a sign of life.
    if (m_lastNote != why || m_ticks % 600 == 0)
    {
        m_lastNote = why;
        HE_LOG_INFO(Scene, "RvtLandscape: %s (tick %llu)", why.c_str(), static_cast<unsigned long long>(m_ticks));
    }
}

uint64_t RvtLandscape::stateHash(HorizonWorld& live, entt::entity terrain) const
{
    // Everything the three view graphs and the volume's place are built from at attach time (HlodBaker::begin): the landscape
    // material instance's parameter block, which parameters it overrides and its static switches, and the landscape's size and
    // position.
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* mc = live.registry().try_get<MaterialComponent>(terrain);
    if (!mc) return h;
    mix(h, mc->materialAssetId.hi); mix(h, mc->materialAssetId.lo);
    for (const MaterialParamOverride& ov : mc->paramOverrides)   // the landscape's own overrides are part of the picture
    {
        mix(h, std::hash<std::string>{}(ov.name));
        for (float f : ov.value) mix(h, f);
    }
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
    if (const auto* tc = live.registry().try_get<TerrainComponent>(terrain))
    {
        const glm::vec3 pos = HE::worldPositionOf(live, terrain);
        mix(h, tc->sizeX); mix(h, tc->sizeZ); mix(h, pos.x); mix(h, pos.z);
        // A mixed landscape binds one shared mix material, so the group whose parameter defaults the views start from comes
        // from the terrain (HlodBaker::begin reads baseGroup), not from the material id above.
        mix(h, tc->groupMix); mix(h, tc->baseGroup);
    }
    return h;
}

bool RvtLandscape::ensurePool()
{
    if (m_poolMade) return true;
    m_pagePx = static_cast<uint32_t>(rvt::VolumeLayout(rvt::VolumeDesc{}).physicalPageSize());
    if (!m_renderer.CreateRvtPool(m_settings.poolSlots, m_pagePx))
    {
        HE_LOG_WARN(Scene, "RvtLandscape: the renderer could not make a pool of %u pages", m_settings.poolSlots);
        return false;
    }
    m_manager = std::make_unique<rvt::RvtManager>(m_settings.poolSlots, m_settings.maxPagesPerFrame, /*pinnedMips=*/1);
    m_poolMade = true;
    HE_LOG_INFO(Scene, "RvtLandscape: pool of %u pages of %u px (%.0f MB), %u painted per frame", m_settings.poolSlots, m_pagePx,
                double(m_settings.poolSlots) * m_pagePx * m_pagePx * 12.0 / (1024.0 * 1024.0), m_settings.maxPagesPerFrame);
    return true;
}

bool RvtLandscape::attach(HorizonWorld& live, entt::entity terrain, Segment& seg)
{
    if (!ensurePool()) return false;
    const auto t0 = std::chrono::steady_clock::now();
    TerrainComponent& liveTc = live.registry().get<TerrainComponent>(terrain);
    if (seg.baker && seg.attached)
    {
        // Re-attach after deactivate(): the scratch copy is from before the edits made while this segment was out of range
        // (tick() only drains the journal of live segments), so it takes them now. The fresh volume has no pages to make stale.
        if (liveTc.rvtJournal.any) syncScratch(*seg.baker, liveTc);
    }
    else
    {
        auto baker = std::make_unique<HlodBaker>(live, m_cm, m_renderer);
        std::string why;
        if (!baker->begin(terrain, &why, /*switchRenderer=*/false))
        {
            HE_LOG_INFO(Scene, "RvtLandscape: not attached (%s)", why.c_str());
            return false;
        }
        seg.baker = std::move(baker);
        m_materialsRegistered += 3;                // the three view materials HlodBaker::begin registers (never unregistered)
    }
    const TerrainComponent& tc = liveTc;
    HlodChunkRect r0;
    seg.baker->chunkRect(0, 0, r0);

    rvt::VolumeDesc vd;
    vd.origin = r0.min;
    vd.sizeM = tc.sizeX;                           // the volume is a square; sizeX == sizeZ here, HlodBaker::begin refuses non-square chunks
    vd.finestMetersPerTexel = m_settings.metersPerTexel;
    seg.volume = m_manager->addVolume(vd);
    seg.volumeLive = true;
    if (static_cast<uint32_t>(m_manager->layout(seg.volume).physicalPageSize()) != m_pagePx)
    {
        HE_LOG_WARN(Scene, "RvtLandscape: a volume with another page size than the pool's is not supported");
        m_manager->removeVolume(seg.volume);
        seg.volumeLive = false;
        return false;
    }

    // The consumer has the volume's origin, extent and page counts baked in, so a landscape that moved or was resized (the volume
    // is rebuilt by remove()+attach() then) needs a new one; the old one is already out of the renderer (deactivate) and its id is
    // simply not used again, like every material this driver makes.
    const rvt::VolumeDesc& cd = seg.consumerDesc;
    const bool consumerStale = seg.consumer != HE::UUID{} &&
        (cd.origin != vd.origin || cd.sizeM != vd.sizeM || cd.finestMetersPerTexel != vd.finestMetersPerTexel ||
         seg.consumerVolume != seg.volume);   // the feedback key carries the volume id baked into the consumer
    if (seg.consumer == HE::UUID{} || consumerStale)
    {
        seg.consumerDesc = vd;
        seg.consumerVolume = seg.volume;
        MaterialAsset ca;
        ca.type = HE::AssetType::Material;
        ca.name = "RvtLandscapeConsumer";
        ca.nodeGraphJson = consumerGraphJson(m_manager->layout(seg.volume), seg.volume);
        seg.consumer = m_cm.registerMaterial(std::move(ca));
        m_cm.regenerateMaterialFromGraph(seg.consumer);
        m_renderer.WarmupMaterials({ seg.consumer });
        ++m_materialsRegistered;
    }
    seg.landscapeMaterial = live.registry().get<MaterialComponent>(terrain).materialAssetId;
    seg.uploadedVersion = ~0ull;
    seg.filled = false;
    seg.attached = true;
    liveTc.rvtJournal.clear();   // the scratch copy is the ground as it is now (made from it, or synced above)
    HE_LOG_INFO(Scene, "RvtLandscape: segment %u attached in %.0f ms: %.0f m landscape, %d mips, %zu segment(s)",
                static_cast<unsigned>(entt::to_integral(terrain)),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                vd.sizeM, m_manager->layout(seg.volume).mipCount(), m_segments.size());
    return true;
}

void RvtLandscape::deactivate(entt::entity terrain, Segment& seg)
{
    // Out of range: the pages and the table go (they are the memory); the scratch copy and the materials stay, so a camera that
    // walks over the border back and forth builds nothing and registers nothing.
    m_renderer.SetRvtLandscape(static_cast<uint32_t>(entt::to_integral(terrain)), HE::UUID{}, HE::UUID{});
    if (!seg.nearChunks.empty()) m_renderer.SetRvtNearChunks(static_cast<uint32_t>(entt::to_integral(terrain)), {});
    seg.nearChunks.clear();
    if (seg.consumer != HE::UUID{}) m_renderer.ReleaseRvtConsumer(seg.consumer);
    if (seg.volumeLive && m_manager) m_manager->removeVolume(seg.volume);
    seg.volumeLive = false;
    seg.filled = false;
    seg.swapOn = false;
    seg.uploadedVersion = ~0ull;
}

void RvtLandscape::remove(entt::entity terrain, Segment& seg)
{
    deactivate(terrain, seg);
    seg.baker.reset();
    seg.attached = false;
}

bool RvtLandscape::active() const
{
    for (const auto& kv : m_segments) if (kv.second.swapOn) return true;
    return false;
}
bool RvtLandscape::active(entt::entity e) const { const auto it = m_segments.find(e); return it != m_segments.end() && it->second.swapOn; }
bool RvtLandscape::attached(entt::entity e) const { const auto it = m_segments.find(e); return it != m_segments.end() && it->second.attached; }
uint16_t RvtLandscape::volumeOf(entt::entity e) const { const auto it = m_segments.find(e); return it != m_segments.end() ? it->second.volume : uint16_t(0); }
HE::UUID RvtLandscape::consumerOf(entt::entity e) const { const auto it = m_segments.find(e); return it != m_segments.end() ? it->second.consumer : HE::UUID{}; }

void RvtLandscape::pauseSwap(bool paused)
{
    m_paused = paused;
    for (auto& kv : m_segments)
    {
        Segment& s = kv.second;
        if (!s.swapOn) continue;
        m_renderer.SetRvtLandscape(static_cast<uint32_t>(entt::to_integral(kv.first)),
                                   paused ? HE::UUID{} : s.landscapeMaterial, paused ? HE::UUID{} : s.consumer);
    }
}

float RvtLandscape::nativeNearRadius() const
{
    // One mip-0 texel per pixel: a texel of t metres at distance d covers t * H / (2 d tan(fov/2)) pixels.
    const float t = std::max(m_settings.metersPerTexel, 1e-3f);
    return t * std::max(m_viewportH, 1.0f) / (2.0f * std::tan(std::clamp(m_fovY, 0.05f, 3.0f) * 0.5f));
}

void RvtLandscape::updateNearChunks(HorizonWorld& live, const glm::vec3& cameraPos, FrameStats& st)
{
    auto& reg = live.registry();
    const float r = nativeNearRadius();
    // HE_RVT_NATIVE_NEAR=0 / 1 overrides the setting (A/B, and the witnesses whose small landscape lies inside the radius).
    static const int envNear = [] { const char* v = std::getenv("HE_RVT_NATIVE_NEAR"); return (v && *v) ? (*v == '0' ? 0 : 1) : -1; }();
    const bool nativeNear = envNear >= 0 ? envNear == 1 : m_settings.nativeNear;
    for (auto& kv : m_segments)
    {
        Segment& seg = kv.second;
        std::unordered_set<uint32_t> next;
        if (nativeNear && seg.volumeLive && reg.valid(kv.first) && reg.all_of<TerrainComponent>(kv.first))
        {
            const TerrainComponent& tc = reg.get<TerrainComponent>(kv.first);
            const uint32_t n = std::max(tc.builtChunksPerSide, 1u);
            const glm::vec3 pos = HE::worldPositionOf(live, kv.first);
            const float cw = tc.sizeX / float(n), ch = tc.sizeZ / float(n);
            for (auto [e, cc] : reg.view<TerrainChunkComponent>().each())
            {
                if (cc.terrain != kv.first) continue;
                const glm::vec2 mn(pos.x - tc.sizeX * 0.5f + float(cc.cx) * cw, pos.z - tc.sizeZ * 0.5f + float(cc.cz) * ch);
                const float dxz = distToRect(glm::vec2(cameraPos.x, cameraPos.z), mn, mn + glm::vec2(cw, ch));
                const float dy  = std::max(0.0f, cameraPos.y - pos.y - tc.heightScale);   // above the highest the ground can reach
                const float d   = std::sqrt(dxz * dxz + dy * dy);
                const uint32_t id = static_cast<uint32_t>(entt::to_integral(e));
                // Hysteresis: a chunk on the line does not flip every frame.
                if (d < r || (seg.nearChunks.count(id) && d < r * 1.15f)) next.insert(id);
            }
        }
        if (next != seg.nearChunks)
        {
            seg.nearChunks = std::move(next);
            m_renderer.SetRvtNearChunks(static_cast<uint32_t>(entt::to_integral(kv.first)),
                                        std::vector<uint32_t>(seg.nearChunks.begin(), seg.nearChunks.end()));
        }
        st.nearChunks += static_cast<uint32_t>(seg.nearChunks.size());
    }
}

RvtLandscape::Settings RvtLandscape::settingsFromProject(const HE::ProjectRvtSettings& p)
{
    Settings s;
    s.poolSlots         = static_cast<uint32_t>(p.poolPages);
    s.maxPagesPerFrame  = static_cast<uint32_t>(p.maxPagesPerFrame);
    s.metersPerTexel    = p.metersPerTexel;
    s.nearRadiusM       = p.nearRadiusM;
    s.activationRadiusM = p.activationRadiusM;
    s.nativeNear        = p.nativeNear;
    return s;
}

bool RvtLandscape::wanted(const HE::ProjectRvtSettings& p)
{
    static const int env = [] { const char* v = std::getenv("HE_RVT"); return (v && *v) ? (*v == '0' ? 0 : 1) : -1; }();
    return env >= 0 ? env == 1 : p.enabled;
}

void RvtLandscape::setSettings(const Settings& s)
{
    const bool rebuild = m_poolMade && (s.poolSlots != m_settings.poolSlots || s.metersPerTexel != m_settings.metersPerTexel);
    m_settings = s;
    if (rebuild) detach();   // the next tick attaches again with the new pool and texel size
    if (m_manager) m_manager->setMaxRendersPerFrame(s.maxPagesPerFrame);
}

RvtLandscape::DebugInfo RvtLandscape::debugInfo() const
{
    DebugInfo d;
    d.running = m_poolMade && m_manager;
    d.segments = static_cast<uint32_t>(m_segments.size());
    for (const auto& kv : m_segments) if (kv.second.swapOn) ++d.activeSegments;
    d.lastPaintMs = m_lastPainting.paintMs;
    d.lastPagesPainted = m_lastPainting.pagesPainted;
    d.pagesPaintedTotal = m_paintedTotal;
    d.materialsRegistered = m_materialsRegistered;
    if (!d.running) return d;
    const rvt::Stats ms = m_manager->stats();
    d.poolSlots = ms.poolSlots; d.resident = ms.resident; d.stale = ms.dirty; d.starved = ms.starved;
    const auto it = m_segments.find(m_debugEntity);
    if (it != m_segments.end() && it->second.volumeLive && m_manager->hasVolume(it->second.volume))
    {
        const uint16_t vol = it->second.volume;
        const rvt::VolumeLayout& L = m_manager->layout(vol);
        d.gridSide = L.pagesPerSide(0);
        d.metersPerPage = L.pageMeters(0);
        d.grid.assign(size_t(d.gridSide) * size_t(d.gridSide), 0);
        for (int y = 0; y < d.gridSide; ++y)
            for (int x = 0; x < d.gridSide; ++x)
            {
                const rvt::PageKey k{ vol, uint8_t(0), uint16_t(x), uint16_t(y) };
                if (m_manager->isResident(k)) d.grid[size_t(y) * size_t(d.gridSide) + size_t(x)] = m_manager->isDirty(k) ? 2 : 1;
            }
    }
    return d;
}

void RvtLandscape::detach()
{
    for (auto& kv : m_segments) remove(kv.first, kv.second);
    m_segments.clear();
    if (m_poolMade) m_renderer.DestroyRvtPool();
    m_poolMade = false;
    m_manager.reset();
    m_lastStarved = 0;
    m_starvedWarned = false;
}

RvtLandscape::FrameStats RvtLandscape::tick(HorizonWorld& live, const glm::vec3& cameraPos)
{
    FrameStats st;
    ++m_ticks;
    if (!m_renderer.GetCapabilities().supportsRvt) { note("idle: the renderer cannot hold a virtual texture (Capabilities::supportsRvt)"); return st; }
    auto& reg = live.registry();
    const glm::vec2 cam(cameraPos.x, cameraPos.z);

    // The landscapes: terrains (not chunks) with a material, nearest first.
    struct Land { entt::entity e; float dist; };
    std::vector<Land> lands;
    std::set<entt::entity> present;
    size_t terrains = 0;
    for (auto [e, tc] : reg.view<TerrainComponent>().each())
    {
        if (reg.all_of<TerrainChunkComponent>(e)) continue;
        ++terrains;
        if (!reg.all_of<MaterialComponent>(e)) continue;
        const glm::vec3 pos = HE::worldPositionOf(live, e);
        const glm::vec2 mn(pos.x - tc.sizeX * 0.5f, pos.z - tc.sizeZ * 0.5f), mx(pos.x + tc.sizeX * 0.5f, pos.z + tc.sizeZ * 0.5f);
        lands.push_back({ e, distToRect(cam, mn, mx) });
        present.insert(e);
    }
    std::sort(lands.begin(), lands.end(), [](const Land& a, const Land& b) { return a.dist < b.dist; });

    // Landscapes that are gone free their volume.
    for (auto it = m_segments.begin(); it != m_segments.end();)
    {
        if (present.count(it->first)) { ++it; continue; }
        remove(it->first, it->second);
        ++st.detaches;
        it = m_segments.erase(it);
    }
    if (lands.empty())
    {
        if (m_poolMade) { m_renderer.DestroyRvtPool(); m_poolMade = false; m_manager.reset(); m_lastStarved = 0; m_starvedWarned = false; }
        note("idle: no landscape with a material in the world (" + std::to_string(terrains) + " landscape(s), 0 with a material)");
        return st;
    }

    // Activation: near ones are attached (one build per frame, nearest first), far ones give their pages back.
    for (const Land& l : lands)
    {
        Segment& seg = m_segments[l.e];
        const uint64_t h = stateHash(live, l.e);
        if (seg.attached && h != seg.stateHash)
        {
            // The view graphs were built from the old values: nothing to patch, start over.
            remove(l.e, seg);
            ++st.detaches;
            st.rescanned = true;
        }
        const bool near = l.dist <= m_settings.activationRadiusM * (seg.volumeLive ? 1.2f : 1.0f);
        if (!near)
        {
            if (seg.volumeLive) { deactivate(l.e, seg); ++st.detaches; }
            continue;
        }
        if (seg.volumeLive) continue;
        if (seg.failed && seg.failedHash == h) continue;                  // it failed for exactly this: not again every frame
        if (st.attaches >= m_settings.maxAttachesPerFrame) continue;
        if (!attach(live, l.e, seg)) { seg.failed = true; seg.failedHash = h; continue; }
        seg.failed = false;
        seg.stateHash = h;
        ++st.attaches;
    }
    st.segments = static_cast<uint32_t>(m_segments.size());
    if (!m_manager) { note("idle: no landscape near enough (activation radius " + std::to_string(static_cast<int>(m_settings.activationRadiusM)) + " m)"); return st; }

    // The journals: scratch copies up to date, the pages under each change stale.
    std::unordered_map<uint16_t, entt::entity> byVolume;
    for (auto& kv : m_segments)
    {
        Segment& seg = kv.second;
        if (!seg.volumeLive) continue;
        byVolume[seg.volume] = kv.first;
        TerrainComponent& tc = reg.get<TerrainComponent>(kv.first);
        if (!tc.rvtJournal.any) continue;
        const TerrainComponent::RvtJournal j = tc.rvtJournal;
        const glm::vec3 pos = HE::worldPositionOf(live, kv.first);
        syncScratch(*seg.baker, tc);
        if (j.all) st.pagesStale += m_manager->invalidateVolume(seg.volume);
        else
        {
            st.pagesStale += m_manager->invalidateRect(seg.volume, glm::vec2(pos.x + j.minX, pos.z + j.minZ),
                                                       glm::vec2(pos.x + j.maxX, pos.z + j.maxZ));
        }
        tc.rvtJournal.clear();
    }

    // The GPU feedback (a backend that has it): the pages pixels asked for, kept wanted for a while after their last report.
    {
        constexpr uint64_t kFeedbackTicks = 120;
        m_feedbackScratch.clear();
        if (m_renderer.ReadRvtFeedback(m_feedbackScratch))
        {
            for (uint32_t k : m_feedbackScratch) m_feedback[k] = m_ticks;
            for (auto it = m_feedback.begin(); it != m_feedback.end();)
                it = m_ticks - it->second > kFeedbackTicks ? m_feedback.erase(it) : std::next(it);
        }
        else m_feedback.clear();
        st.feedbackPages = static_cast<uint32_t>(m_feedback.size());
    }

    // What the camera needs across all active volumes, ranked together, one budget; exactly that is painted.
    std::vector<rvt::VolumeView> views;
    for (const auto& kv : byVolume)
    {
        rvt::ViewDesc view;
        view.cameraXZ = cam;
        view.nearRadiusM = m_settings.nearRadiusM;
        for (const auto& f : m_feedback)   // rvtFeedbackKey: volume 5 | mip 5 | x 11 | y 11
            if ((f.first >> 27) == kv.first)
                view.requested.push_back({ kv.first, uint8_t((f.first >> 22) & 31u), uint16_t((f.first >> 11) & 2047u), uint16_t(f.first & 2047u) });
        views.push_back({ kv.first, std::move(view) });
    }
    m_manager->beginFrame();
    const auto paintT0 = std::chrono::steady_clock::now();
    if (!views.empty())
    {
        const std::vector<rvt::PageRender> renders = m_manager->updateAll(views);
        // All the pages of this tick between one Begin and one End: a backend whose page is the general still frame keeps one
        // page-size set across them instead of building and repairing it around every page. Closed on every way out.
        struct PageBracket
        {
            IRenderer& r; bool open = false;
            PageBracket(IRenderer& rr, bool on, uint32_t px) : r(rr), open(on) { if (open) r.BeginRvtPages(px); }
            ~PageBracket() { if (open) r.EndRvtPages(); }
        } bracket(m_renderer, !renders.empty(), static_cast<uint32_t>(m_pagePx));
        std::unordered_map<uint16_t, std::vector<rvt::PageRender>> perVolume;
        for (const rvt::PageRender& r : renders) perVolume[r.key.volume].push_back(r);
        for (auto& kv : perVolume)
        {
            Segment& seg = m_segments[byVolume[kv.first]];
            seg.baker->paintBegin();
            st.pagesPainted += rvt::paintPages(*seg.baker, kv.second, m_pagePx);
            seg.baker->paintEnd();
        }
        m_paintedTotal += st.pagesPainted;
    }
    st.paintMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - paintT0).count();
    if (st.pagesPainted) m_lastPainting = st;
    // The pages went quiet: let go of the scratch landscape's chunk meshes on the GPU (HE_RVT_IDLE_RELEASE = seconds, 0 = never).
    // The next page draw uploads them again, which is one hitch of the size of the geometry, so not at the first quiet frame.
    {
        static const double idleS = [] { const char* v = std::getenv("HE_RVT_IDLE_RELEASE"); return (v && *v) ? std::atof(v) : 5.0; }();
        const auto now = std::chrono::steady_clock::now();
        if (st.pagesPainted)
        {
            m_lastPaintAt = now;
            for (auto& kv : m_segments) kv.second.meshesReleased = false;
        }
        else if (idleS > 0.0 && std::chrono::duration<double>(now - m_lastPaintAt).count() >= idleS)
            for (auto& kv : m_segments)
            {
                Segment& seg = kv.second;
                if (!seg.baker || !seg.volumeLive || seg.meshesReleased) continue;
                const uint32_t n = seg.baker->releaseIdleMeshes();
                seg.meshesReleased = true;
                HE_LOG_INFO(Scene, "RvtLandscape: %u scratch chunk mesh(es) released from the GPU after %.0f s without a page", n, idleS);
            }
    }
    m_debugEntity = entt::null;
    for (const Land& l : lands)                      // nearest first
        if (const auto f = m_segments.find(l.e); f != m_segments.end() && f->second.volumeLive) { m_debugEntity = l.e; break; }

    // The page tables (one per volume, when a page came or went), the first-fill latch and the swap.
    for (auto& kv : m_segments)
    {
        Segment& seg = kv.second;
        if (!seg.volumeLive) continue;
        const uint64_t tv = m_manager->tableVersion(seg.volume);
        if (tv != seg.uploadedVersion)
        {
            std::vector<std::vector<uint32_t>> mips;
            for (int m = 0; m < m_manager->layout(seg.volume).mipCount(); ++m) mips.push_back(m_manager->pageTable(seg.volume, m));
            if (m_renderer.SetRvtPageTable(seg.consumer, mips)) { seg.uploadedVersion = tv; ++st.tablesUploaded; }
        }
        // Drawn from the pages once the first fill is complete (before that the coarse fallback would show a blurry landscape
        // for a few frames), and it stays on afterwards: a stale or missing page degrades, it does not switch back.
        if (!seg.filled && seg.uploadedVersion == tv && m_manager->missing(seg.volume) == 0) seg.filled = true;
        const bool want = seg.filled && !m_paused;
        if (want != seg.swapOn)
        {
            m_renderer.SetRvtLandscape(static_cast<uint32_t>(entt::to_integral(kv.first)),
                                       want ? seg.landscapeMaterial : HE::UUID{}, want ? seg.consumer : HE::UUID{});
            seg.swapOn = want;
        }
        if (seg.swapOn) ++st.activeSegments;
    }
    updateNearChunks(live, cameraPos, st);

    const rvt::Stats ms = m_manager->stats();
    // One warning per episode: a pool that is too small starves every frame, and the number alone changes the text each time.
    if (ms.starved > m_lastStarved)
    {
        if (!m_starvedWarned)
            HE_LOG_WARN(Scene, "RvtLandscape: the pool is full (%u of %u pages): %u request(s) found no slot; raise poolSlots or lower the activation radius",
                        ms.resident, ms.poolSlots, ms.starved - m_lastStarved);
        m_starvedWarned = true;
        m_lastStarved = ms.starved;
    }
    else m_starvedWarned = false;
    note("running");

    // Activity only: an idle frame says nothing, so a log of a quiet scene is quiet and a stroke, a camera move or a tile
    // coming into range shows up.
    if (st.pagesPainted || st.pagesStale || st.tablesUploaded || st.rescanned || st.attaches || st.detaches)
        HE_LOG_INFO(Scene, "RvtLandscape: %u page(s) painted, %u marked stale, %u table(s) uploaded, %u attached, %u released, %u/%u segment(s) drawn from pages, %u resident, %u stale, painting took %.1f ms; %u page(s) wanted by feedback, %u chunk(s) native near (radius %.0f m)",
                    st.pagesPainted, st.pagesStale, st.tablesUploaded, st.attaches, st.detaches, st.activeSegments, st.segments, ms.resident, ms.dirty, st.paintMs,
                    st.feedbackPages, st.nearChunks, double(nativeNearRadius()));
    return st;
}

} // namespace HE::landscape
