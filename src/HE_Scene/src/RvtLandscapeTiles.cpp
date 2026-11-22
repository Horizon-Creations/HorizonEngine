// The tile landscape's ground as a user of the Runtime Virtual Texture pool (Thema 326). See RvtLandscape.h, TileGround.
#include <HorizonScene/RvtLandscape.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtSplit.h>
#include <Renderer/IRenderer.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace HE::landscape
{

uint64_t tileGroundKey(ContentManager& cm, const HE::UUID& source, const MaterialComponent* mc)
{
    const MaterialAsset* ma = cm.getMaterial(source);
    if (!ma || ma->nodeGraphJson.empty()) return 0;
    uint64_t key = 0xcbf29ce484222325ULL;
    const auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) key = (key ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ULL;
    };
    const size_t sizes[2] = { ma->nodeGraphJson.size(), ma->customShaderFragGlsl.size() };
    mix(sizes, sizeof(sizes));
    mix(ma->shaderParamData.data(), ma->shaderParamData.size() * sizeof(float));
    if (mc)
        for (const MaterialParamOverride& o : mc->paramOverrides) { mix(o.name.data(), o.name.size()); mix(o.value, sizeof(o.value)); }
    return key ? key : 1;
}

bool tileGroundGraph(ContentManager& cm, const HE::UUID& source, const MaterialComponent* mc, MaterialGraph& out)
{
    const MaterialAsset* ma = cm.getMaterial(source);
    if (!ma || ma->nodeGraphJson.empty() || !materialGraphFromJson(ma->nodeGraphJson, out)) return false;
    for (MatGraphNode& n : out.nodes)
    {
        if (n.type != MatNodeType::ParamFloat && n.type != MatNodeType::ParamColor && n.type != MatNodeType::ParamVec4
            && n.type != MatNodeType::ParamVec2 && n.type != MatNodeType::ParamBool)
            continue;
        for (size_t i = 0; i < ma->graphParamNames.size(); ++i)
            if (ma->graphParamNames[i] == n.s && ma->shaderParamData.size() >= (i + 1) * 4)
                for (size_t c = 0; c < 4; ++c) n.p[c] = ma->shaderParamData[i * 4 + c];
        if (mc)
            for (const MaterialParamOverride& o : mc->paramOverrides)
                if (o.name == n.s)
                    for (size_t c = 0; c < 4; ++c) n.p[c] = o.value[c];
    }
    return true;
}

namespace
{
    const MaterialComponent* tileGroundEntityMaterial(HorizonWorld& live)
    {
        for (auto [e, lw] : live.registry().view<LandscapeWorldComponent>().each())
            return live.registry().try_get<MaterialComponent>(e);
        return nullptr;
    }
}

bool RvtLandscape::tileGroundWanted(HorizonWorld& live)
{
    TileGround& t = m_tiles;
    const HE::LandscapeRuntime* rt = live.landscapeRuntime();
    if (!rt || !rt->active() || rt->materialId() == HE::UUID{} || !m_renderer.GetCapabilities().landscapeMaterial)
    {
        removeTileGround();
        return false;
    }
    const HE::UUID source = rt->materialId();
    // The texture group's material is named by no entity: nothing else may have loaded it yet.
    if (!m_cm.getMaterial(source) && !rt->materialPath().empty() && !m_cm.resolveMaterialRef(source, rt->materialPath()))
        m_cm.loadAsset(rt->materialPath());
    const MaterialComponent* mc = tileGroundEntityMaterial(live);
    const uint64_t key = tileGroundKey(m_cm, source, mc);
    if (key == 0) removeTileGround();
    if (key == 0 || key == t.failedKey) return false;
    if (key == t.key) return true;

    MaterialGraph g;
    MatRvtSplit split;
    MatRvtLayerViews lv;
    if (tileGroundGraph(m_cm, source, mc, g))
    {
        split = matSplitRvt(g);
        if (split.hasRvtNodes && split.ok()) lv = matRvtLayerViews(split, kAutoLandscapeRvtName);
    }
    if (!lv.ok)
    {
        // Not an RVT material (or one that cannot be cut): the ground is drawn with the material itself, as without this driver.
        HE_LOG_INFO(Scene, "RvtLandscape: the tile ground's material writes no virtual texture; it is computed in place");
        removeTileGround();
        t.failedKey = key;
        return false;
    }
    const char* names[3] = { "TileRvtBaseColor", "TileRvtNormal", "TileRvtSurface" };
    const auto put = [&](HE::UUID& id, const char* name, const MaterialGraph& graph) {
        MaterialAsset a;
        a.type = HE::AssetType::Material;
        a.name = name;
        a.nodeGraphJson = materialGraphToJson(graph);
        const bool fresh = id == HE::UUID{} || !m_cm.getMaterial(id);
        if (fresh) { id = m_cm.registerMaterial(std::move(a)); ++m_materialsRegistered; }
        else       m_cm.replaceMaterial(id, std::move(a));
        m_cm.regenerateMaterialFromGraph(id);
        if (!fresh) m_renderer.InvalidateMaterial(id);
    };
    for (int v = 0; v < 3; ++v) { put(t.view[v], names[v], lv.view[v]); t.seen[v] = false; }
    put(t.consumer, "TileRvtConsumer", split.live);
    t.key = key;
    t.failedKey = 0;
    t.writtenVolume = 0xFFFF;   // the new consumer asset has the graph's defaults in its volume vectors
    if (m_manager)
    {
        if (t.volumeLive)  m_manager->invalidateVolume(t.volume);
        if (t.pendingLive) m_manager->invalidateVolume(t.pending);
    }
    return true;
}

void RvtLandscape::tileGroundVolumes(HorizonWorld& live, const glm::dvec2& camAbs, FrameStats& st)
{
    TileGround& t = m_tiles;
    const HE::LandscapeRuntime* rt = live.landscapeRuntime();
    if (!rt || !m_manager) return;

    rvt::VolumeDesc vd;
    vd.finestMetersPerTexel = m_settings.metersPerTexel;
    // The table is capped at 2048 pages a side (RvtLookup.h, rvtFeedbackKey): a world that fits is one volume for good; a larger
    // one gets the largest square, around the camera, on a grid of a quarter of its edge.
    const glm::dvec2 world = rt->worldSizeM();
    const double side = std::max(world.x, world.y);
    const double maxSide = 2048.0 * double(vd.pageSize) * double(vd.finestMetersPerTexel);
    const bool whole = side <= maxSide;
    vd.sizeM = float(whole ? side : maxSide);
    const double extent = double(rvt::VolumeLayout(vd).extentM());
    const auto anchorFor = [&](const glm::dvec2& c) {
        if (whole) return glm::dvec2(0.0);
        const double grid = extent * 0.25;
        return glm::dvec2(std::floor((c.x - extent * 0.5) / grid + 0.5) * grid, std::floor((c.y - extent * 0.5) / grid + 0.5) * grid);
    };
    const auto add = [&](uint16_t& id) {
        id = m_manager->addVolume(vd);   // origin (0, 0): the manager counts from the anchor
        if (static_cast<uint32_t>(m_manager->layout(id).physicalPageSize()) == m_pagePx) return true;
        m_manager->removeVolume(id);
        return false;
    };
    if (t.volumeLive && std::abs(double(m_manager->layout(t.volume).extentM()) - extent) > 1e-3)
        removeTileGround();   // another world or texel size
    if (!t.volumeLive)
    {
        if (!add(t.volume)) return;
        t.volumeLive = true;
        t.anchor = anchorFor(camAbs);
        t.updated = t.filled = false;
        t.uploadedVersion = ~0ull;
        ++st.attaches;
    }
    else if (!whole && !t.pendingLive)
    {
        const glm::dvec2 d = glm::abs(camAbs - (t.anchor + glm::dvec2(extent * 0.5)));
        if (std::max(d.x, d.y) > extent * 0.25 && add(t.pending))
        {
            t.pendingLive = true;
            t.pendingUpdated = false;
            t.pendingAnchor = anchorFor(camAbs);
        }
    }

    // What the pages were painted from changed under these tiles (streamed in, edited).
    std::vector<LandscapeTileKey> changed;
    rt->takeChangedTiles(changed);
    const double tileM = rt->quadtree().tileM > 0.0f ? double(rt->quadtree().tileM) : 0.0;
    uint32_t stale = 0;
    for (const LandscapeTileKey& k : changed)
    {
        const glm::dvec2 mn(k.x * tileM, k.z * tileM), mx(mn + glm::dvec2(tileM));
        if (t.volumeLive)  stale += m_manager->invalidateRect(t.volume, glm::vec2(mn - t.anchor), glm::vec2(mx - t.anchor));
        if (t.pendingLive) m_manager->invalidateRect(t.pending, glm::vec2(mn - t.pendingAnchor), glm::vec2(mx - t.pendingAnchor));
    }
    st.pagesStale += stale;
}

void RvtLandscape::tileGroundViews(const glm::dvec2& camAbs, std::vector<rvt::VolumeView>& views)
{
    TileGround& t = m_tiles;
    if (!t.volumeLive || !m_manager) return;
    const float nearR = m_settings.nearRadiusM * std::min(1.0f, m_settings.metersPerTexel / 0.25f);
    const float band0 = std::max(nearR, nativeNearRadius());
    const rvt::VolumeLayout& L = m_manager->layout(t.volume);
    const auto make = [&](uint16_t vol, const glm::dvec2& anchor) {
        rvt::ViewDesc view;
        view.cameraXZ = glm::vec2(camAbs - anchor);
        view.nearRadiusM = nearR;
        const rvt::VolumeLayout& T = m_manager->layout(vol);
        // The shown volume's feedback, also for its successor: the same ground is wanted there, at the same detail.
        for (const auto& f : m_feedback)   // rvtFeedbackKey: volume 5 | mip 5 | x 11 | y 11
        {
            if ((f.first >> 27) != t.volume) continue;
            int mip = int((f.first >> 22) & 31u), x = int((f.first >> 11) & 2047u), y = int(f.first & 2047u);
            if (mip >= L.mipCount() || x >= L.pagesPerSide(mip) || y >= L.pagesPerSide(mip)) continue;
            glm::vec2 mn, mx;
            L.pageRect(mip, x, y, mn, mx, false);
            const glm::vec2 c = glm::vec2(glm::dvec2((mn + mx) * 0.5f) + t.anchor - anchor);   // in the target volume
            const float d = std::max(std::abs(c.x - view.cameraXZ.x), std::abs(c.y - view.cameraXZ.y));
            mip = std::min(mip, T.mipCount() - 1);
            while (mip + 1 < T.mipCount() && d > band0 * float(1 << mip)) ++mip;
            if (c.x < 0.0f || c.y < 0.0f || c.x >= T.extentM() || c.y >= T.extentM()) continue;
            T.pageAt(mip, c, x, y);
            view.requested.push_back({ vol, uint8_t(mip), uint16_t(x), uint16_t(y) });
        }
        views.push_back({ vol, std::move(view) });
    };
    make(t.volume, t.anchor);
    t.updated = true;
    if (t.pendingLive) { make(t.pending, t.pendingAnchor); t.pendingUpdated = true; }
}

uint32_t RvtLandscape::paintTileGround(HorizonWorld& live, uint16_t volume, const std::vector<rvt::PageRender>& pages)
{
    TileGround& t = m_tiles;
    const HE::LandscapeRuntime* rt = live.landscapeRuntime();
    if (!rt || pages.empty()) return 0;
    const glm::dvec2 anchor = (t.pendingLive && volume == t.pending) ? t.pendingAnchor : t.anchor;
    // An empty world at the live one's floating origin: the page camera sees the ground (the renderer's landscape provider, which
    // is the live runtime's) and nothing that stands on it; the materials' World Position is the live one's.
    if (!t.scratch) t.scratch = std::make_shared<HorizonWorld>();
    const glm::dvec3 origin = live.origin();
    t.scratch->setOrigin(origin);
    m_renderer.SetWorld(t.scratch.get());
    const float topY = float(double(rt->config().heightMinM) + double(rt->config().heightScaleM) - origin.y) + 10.0f;
    const float depth = rt->config().heightScaleM + 40.0f;
    uint32_t painted = 0;
    std::vector<uint8_t> none;
    for (const rvt::PageRender& p : pages)
    {
        const glm::dvec2 c = glm::dvec2((p.rectMin + p.rectMax) * 0.5f) + anchor;
        EditorCameraOverride cam;
        cam.active          = true;
        cam.orthographic    = true;
        cam.rawOutput       = true;
        cam.editorIcons     = false;
        cam.orthoHalfHeight = (p.rectMax.y - p.rectMin.y) * 0.5f;
        const glm::vec3 eye(float(c.x - origin.x), topY, float(c.y - origin.z));
        cam.position  = eye;
        cam.view      = glm::lookAt(eye, eye + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        cam.nearPlane = 0.1f;
        cam.farPlane  = depth;
        cam.rvtSlot   = int(p.slot);
        cam.rvtTileGround = true;
        bool ok = true;
        for (int v = 0; v < 3 && ok; ++v)
        {
            if (!(p.layers & (1u << v))) continue;
            cam.rawSrgb = v == 0;
            cam.rvtLayer = v;
            cam.rvtMaterial = t.view[v];
            if (!t.seen[v]) { m_renderer.InvalidateMaterial(t.view[v]); t.seen[v] = true; }
            ok = m_renderer.RenderSceneImage(cam, m_pagePx, m_pagePx, none);
        }
        if (ok) ++painted;
    }
    m_renderer.SetWorld(&live);
    return painted;
}

void RvtLandscape::tileGroundTables(HorizonWorld& live, FrameStats& st)
{
    TileGround& t = m_tiles;
    if (!t.volumeLive || !m_manager) return;
    // The successor is complete: it becomes the volume in one step (table, volume vectors, rectangle), the old pages go.
    if (t.pendingLive && t.pendingUpdated && m_manager->missing(t.pending) == 0)
    {
        m_manager->removeVolume(t.volume);
        t.volume = t.pending;
        t.anchor = t.pendingAnchor;
        t.pendingLive = false;
        t.uploadedVersion = ~0ull;
        ++st.detaches;
    }
    const uint64_t tv = m_manager->tableVersion(t.volume) + (m_debugView ? ((m_manager->dirtyVersion() + 1) << 40) : 0ull);
    if (tv != t.uploadedVersion)
    {
        std::vector<std::vector<uint32_t>> mips;
        for (int m = 0; m < m_manager->layout(t.volume).mipCount(); ++m) mips.push_back(m_manager->pageTable(t.volume, m, m_debugView));
        if (m_renderer.SetRvtPageTable(t.consumer, mips)) { t.uploadedVersion = tv; ++st.tablesUploaded; }
    }
    if (!t.filled && t.updated && t.uploadedVersion == tv && m_manager->missing(t.volume) == 0) t.filled = true;

    // The consumer's volume vectors (RvtSplit.h, matRvtVolumeParamName). A is relative to the floating origin, like the position
    // the lookup is given (vWorldPos), so it follows a move of either.
    const rvt::VolumeLayout& l = m_manager->layout(t.volume);
    const glm::dvec2 rel = t.anchor - glm::dvec2(live.origin().x, live.origin().z);
    if (rel != t.writtenAt || t.writtenVolume != t.volume)
        if (MaterialAsset* ma = m_cm.getMaterialMutable(t.consumer))
        {
            const rvt::VolumeDesc& d = l.desc();
            const float a[4] = { float(rel.x), float(rel.y), l.extentM(), l.texelMeters(0) };
            const float b[4] = { float(d.pageSize), float(d.border) + 256.0f * float(t.volume), float(l.pagesPerSide(0)), float(l.mipCount()) };
            const auto put = [&](const std::string& name, const float* v) {
                const auto it = std::find(ma->graphParamNames.begin(), ma->graphParamNames.end(), name);
                if (it == ma->graphParamNames.end()) return;
                const size_t slot = static_cast<size_t>(it - ma->graphParamNames.begin());
                if (ma->shaderParamData.size() < slot * 4 + 4) ma->shaderParamData.resize(slot * 4 + 4, 0.0f);
                for (size_t c = 0; c < 4; ++c) ma->shaderParamData[slot * 4 + c] = v[c];
            };
            put(matRvtVolumeParamName(kAutoLandscapeRvtName, 'A'), a);
            put(matRvtVolumeParamName(kAutoLandscapeRvtName, 'B'), b);
            t.writtenAt = rel;
            t.writtenVolume = t.volume;
        }

    const bool want = t.filled && !m_paused;
    const glm::dvec2 mx = t.anchor + glm::dvec2(double(l.extentM()));
    m_renderer.SetRvtTileGround(want ? t.consumer : HE::UUID{}, t.anchor, mx);
    t.swapOn = want;
    if (want) ++st.activeSegments;
}

void RvtLandscape::removeTileGround()
{
    TileGround& t = m_tiles;
    if (!t.volumeLive && !t.pendingLive && !t.swapOn) return;
    m_renderer.SetRvtTileGround(HE::UUID{}, glm::dvec2(0.0), glm::dvec2(0.0));
    if (t.consumer != HE::UUID{}) m_renderer.ReleaseRvtConsumer(t.consumer);
    if (m_manager)
    {
        if (t.volumeLive)  m_manager->removeVolume(t.volume);
        if (t.pendingLive) m_manager->removeVolume(t.pending);
    }
    t.volumeLive = t.pendingLive = t.updated = t.pendingUpdated = t.filled = t.swapOn = false;
    t.uploadedVersion = ~0ull;
    t.writtenVolume = 0xFFFF;
}

} // namespace HE::landscape
