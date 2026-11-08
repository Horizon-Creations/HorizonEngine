#include <HorizonScene/RvtLandscape.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeHlodBaker.h>
#include <HorizonScene/RvtProducer.h>
#include <HorizonScene/RvtBaseBake.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtSplit.h>
#include <Renderer/IRenderer.h>
#include <algorithm>
#include <array>
#include <chrono>
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

    // Distance from a point to a rectangle (0 inside).
    float distToRect(glm::vec2 p, glm::vec2 mn, glm::vec2 mx)
    {
        const float dx = std::max({ mn.x - p.x, 0.0f, p.x - mx.x });
        const float dz = std::max({ mn.y - p.y, 0.0f, p.y - mx.y });
        return std::sqrt(dx * dx + dz * dz);
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

namespace
{
    uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    }

    // A revision for an asset a page names: moves when its content does. A texture: shape plus a strided sample of the pixels
    // (a re-import with other pixels moves it, a full hash of a 4k texture every epoch would not be cheap); a function: its graph.
    uint64_t assetRevision(ContentManager& cm, const std::string& path)
    {
        const HE::UUID id = cm.loadAsset(path);
        uint64_t h = fnv(&id.hi, sizeof id.hi);
        h = fnv(&id.lo, sizeof id.lo, h);
        if (const TextureAsset* tex = cm.getTexture(id))
        {
            const uint32_t shape[5] = { tex->width, tex->height, tex->channels, tex->layers, tex->mipLevels };
            h = fnv(shape, sizeof shape, h);
            const size_t n = tex->data.size();
            h = fnv(&n, sizeof n, h);
            const size_t step = std::max<size_t>(1, n / 4096);
            for (size_t i = 0; i < n; i += step) h = fnv(&tex->data[i], 1, h);
        }
        else if (const MaterialFunctionAsset* fn = cm.getMaterialFunction(id))
            h = fnv(fn->nodeGraphJson.data(), fn->nodeGraphJson.size(), h);
        return h;
    }
}

void RvtLandscape::refreshAnalysis(HorizonWorld& live, entt::entity terrain, Segment& seg)
{
    // The graph the landscape's material (or its parent, for an instance) is made of, cut at its RVT nodes. Re-cut only when
    // the graph text changed (one hash of the text per tick); the asset revisions are re-read when the content epoch moves.
    const auto* mc = live.registry().try_get<MaterialComponent>(terrain);
    const MaterialAsset* ma = mc ? m_cm.getMaterial(mc->materialAssetId) : nullptr;
    if (!ma) { seg.analysis.reset(); seg.reads = kRvtReadsGround; return; }
    const MaterialAsset* base = ma;
    if (!ma->parentMaterialPath.empty())
    {
        const HE::UUID pid = m_cm.loadAsset(ma->parentMaterialPath);
        if (const MaterialAsset* pm = m_cm.getMaterial(pid)) base = pm;
    }
    const std::string& json = base->nodeGraphJson;
    const uint64_t key = json.empty() ? 0 : fnv(json.data(), json.size());
    if (!seg.analysis || seg.analysis->graphKey != key)
    {
        auto an = std::make_shared<Segment::Analysis>();
        an->graphKey = key;
        if (!json.empty() && materialGraphFromJson(json, an->graph))
        {
            an->split = matSplitRvt(an->graph);
            for (const std::string& p : an->split.problems)
                HE_LOG_WARN(Scene, "RvtLandscape: material graph: %s", p.c_str());
        }
        seg.analysis = an;
    }
    Segment::Analysis& an = *seg.analysis;
    if (!an.split.hasRvtNodes)
    {
        // No RVT node: the pages are built from the whole material (the Thema 333 views) and read all of the ground. The
        // (empty) analysis stays so the graph text is not parsed again every tick.
        seg.reads = kRvtReadsGround;
        return;
    }

    // Parameter values as the material system resolves them: the asset's slots, the entity's overrides on top (the export's
    // baked-base key builds the same environment through the same function, RvtBaseBake.h).
    MatRvtEnv env = rvtMaterialEnv(*ma, mc);
    env.assetRevision = an.env.assetRevision;
    env.functionReads = an.env.functionReads;

    if (an.epoch != m_cm.contentEpoch())
    {
        an.epoch = m_cm.contentEpoch();
        env.assetRevision.clear();
        env.functionReads.clear();
        std::set<std::string> done;
        for (const MatRvtPage& pg : an.split.pages)
            for (const std::string& a : matRvtPageDeps(an.graph, pg, env).assets)
            {
                if (!done.insert(a).second) continue;
                env.assetRevision[a] = assetRevision(m_cm, a);
                // What a called function reads of the ground: one level (its own nodes); a call inside it reads everything.
                const HE::UUID fid = m_cm.loadAsset(a);
                if (const MaterialFunctionAsset* fn = m_cm.getMaterialFunction(fid))
                {
                    MaterialGraph fg;
                    uint32_t r = kRvtReadsGround;
                    if (materialGraphFromJson(fn->nodeGraphJson, fg))
                    {
                        r = 0;
                        for (const MatGraphNode& n : fg.nodes)
                            r |= n.type == MatNodeType::FunctionCall ? kRvtReadsGround : matRvtNodeGroundReads(n.type);
                    }
                    env.functionReads[a] = r;
                }
            }
        an.env.assetRevision = env.assetRevision;
        an.env.functionReads = env.functionReads;
    }
    an.env.params = std::move(env.params);
    an.env.switches = std::move(env.switches);
    const MatRvtMaterialState ms = matRvtMaterialState(an.graph, an.split, an.env);
    an.state = ms.state;
    seg.reads = ms.reads;
}

uint64_t RvtLandscape::stateHash(HorizonWorld& live, entt::entity terrain, Segment& seg, uint64_t& fullHash, uint64_t& geoHash)
{
    // Everything the three view graphs and the volume's place are built from at attach time (HlodBaker::begin): the landscape
    // material instance's parameter block, which parameters it overrides and its static switches, and the landscape's size and
    // position.
    uint64_t h = 0xCBF29CE484222325ull;
    fullHash = h;
    geoHash = h;
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
    uint64_t geo = 0x9E3779B97F4A7C15ull;
    if (const auto* tc = live.registry().try_get<TerrainComponent>(terrain))
    {
        const glm::vec3 pos = HE::worldPositionOf(live, terrain);
        mix(h, tc->sizeX); mix(h, tc->sizeZ); mix(h, pos.x); mix(h, pos.z);
        mix(geo, tc->sizeX); mix(geo, tc->sizeZ); mix(geo, pos.x); mix(geo, pos.z);
    }
    fullHash = h;
    geoHash = h;   // a material without RVT nodes: anything that moved is a reason to start over

    // RVT 2.0: a material with an RVT Write/Read pair is two programs. The pages depend on the page subgraphs' inputs and on
    // nothing else, so only those are hashed; a parameter that sits after the Read node moves `fullHash` and not this.
    refreshAnalysis(live, terrain, seg);
    if (!seg.analysis || !seg.analysis->split.hasRvtNodes) return h;
    geoHash = geo ? geo : 1;
    // (not the graph text: an edit after the Read node changes the text and no page)
    mix(geo, seg.analysis->state);
    return geo ? geo : 1;
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
    if (!seg.baker || !seg.attached)
    {
        // RVT 2.0 step 6: the export's baked base, when the pak carries it for exactly this landscape (the key says the material's
        // page inputs and the ground are the ones it was taken of) and nothing ended it yet. The page graphs are then not built.
        HE::UUID bakedViews[3];
        bool useBaked = false;
        seg.bakedKey = 0;
        if (!seg.bakedOff && seg.analysis && seg.analysis->split.hasRvtNodes)
        {
            const uint64_t key = rvtBaseKey(m_cm, live, terrain);
            if (key != 0 && rvtBaseViewMaterials(m_cm, live, terrain, key, bakedViews)) { useBaked = true; seg.bakedKey = key; m_materialsRegistered += 3; }
        }
        auto baker = std::make_unique<HlodBaker>(live, m_cm, m_renderer);
        std::string why;
        if (!baker->begin(terrain, &why, /*switchRenderer=*/false, /*rvtPages=*/true, useBaked ? bakedViews : nullptr))
        {
            HE_LOG_INFO(Scene, "RvtLandscape: not attached (%s)", why.c_str());
            return false;
        }
        seg.baker = std::move(baker);
        seg.baked = useBaked;
        if (!useBaked) m_materialsRegistered += 3;   // the three view materials HlodBaker::begin registers (never unregistered)
    }
    const TerrainComponent& tc = live.registry().get<TerrainComponent>(terrain);
    HlodChunkRect r0;
    seg.baker->chunkRect(0, 0, r0);

    rvt::VolumeDesc vd;
    vd.origin = r0.min;
    vd.sizeM = tc.sizeX;
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

    if (!seg.baker->rvtLiveGraphJson().empty())
        refreshConsumer(seg);   // RVT 2.0: the material's own live part, the volume vectors in its Read node
    else if (seg.consumer == HE::UUID{})
    {
        MaterialAsset ca;
        ca.type = HE::AssetType::Material;
        ca.name = "RvtLandscapeConsumer";
        ca.nodeGraphJson = consumerGraphJson(m_manager->layout(seg.volume));
        seg.consumer = m_cm.registerMaterial(std::move(ca));
        m_cm.regenerateMaterialFromGraph(seg.consumer);
        m_renderer.WarmupMaterials({ seg.consumer });
        ++m_materialsRegistered;
    }
    seg.landscapeMaterial = live.registry().get<MaterialComponent>(terrain).materialAssetId;
    seg.uploadedVersion = ~0ull;
    seg.filled = false;
    seg.attached = true;
    live.registry().get<TerrainComponent>(terrain).rvtJournal.clear();   // the scratch copy was made from the ground as it is now
    HE_LOG_INFO(Scene, "RvtLandscape: segment %u attached in %.0f ms%s: %.0f m landscape, %d mips, %zu segment(s)",
                static_cast<unsigned>(entt::to_integral(terrain)),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                seg.baked ? " (pages from the export's baked base, the page graphs are not built)" : "",
                vd.sizeM, m_manager->layout(seg.volume).mipCount(), m_segments.size());
    return true;
}

void RvtLandscape::refreshConsumer(Segment& seg)
{
    const std::string& json = seg.baker ? seg.baker->rvtLiveGraphJson() : std::string();
    if (json.empty() || !m_manager || !seg.volumeLive) return;
    std::string oldGlsl;
    if (seg.consumer != HE::UUID{})
        if (const MaterialAsset* o = m_cm.getMaterial(seg.consumer)) oldGlsl = o->customShaderFragGlsl;
    MaterialAsset ca;
    ca.type = HE::AssetType::Material;
    ca.name = "RvtLandscapeConsumer";
    ca.nodeGraphJson = json;
    const bool fresh = seg.consumer == HE::UUID{};
    if (fresh) { seg.consumer = m_cm.registerMaterial(std::move(ca)); ++m_materialsRegistered; }
    else         m_cm.replaceMaterial(seg.consumer, std::move(ca));
    m_cm.regenerateMaterialFromGraph(seg.consumer);

    // The two volume vectors of the Read node are parameters the engine fills (RvtSplit.h, matRvtVolumeParamName).
    if (MaterialAsset* ma = m_cm.getMaterialMutable(seg.consumer))
    {
        const rvt::VolumeLayout& l = m_manager->layout(seg.volume);
        const rvt::VolumeDesc& d = l.desc();
        const float a[4] = { d.origin.x, d.origin.y, l.extentM(), l.texelMeters(0) };
        const float b[4] = { float(d.pageSize), float(d.border), float(l.pagesPerSide(0)), float(l.mipCount()) };
        auto put = [&](const std::string& name, const float* v) {
            const auto it = std::find(ma->graphParamNames.begin(), ma->graphParamNames.end(), name);
            if (it == ma->graphParamNames.end()) return;
            const size_t slot = static_cast<size_t>(it - ma->graphParamNames.begin());
            if (ma->shaderParamData.size() < slot * 4 + 4) ma->shaderParamData.resize(slot * 4 + 4, 0.0f);
            for (size_t c = 0; c < 4; ++c) ma->shaderParamData[slot * 4 + c] = v[c];
        };
        put(matRvtVolumeParamName(kAutoLandscapeRvtName, 'A'), a);
        put(matRvtVolumeParamName(kAutoLandscapeRvtName, 'B'), b);
    }
    // Only a changed shader text needs the renderer to build again; a parameter change reaches the draw through the block.
    const MaterialAsset* now = m_cm.getMaterial(seg.consumer);
    if (fresh || !now || now->customShaderFragGlsl != oldGlsl)
    {
        if (!fresh) m_renderer.InvalidateMaterial(seg.consumer);
        m_renderer.WarmupMaterials({ seg.consumer });
    }
}

void RvtLandscape::refreshFallback(HorizonWorld& live, entt::entity terrain, Segment& seg, uint64_t liveHash)
{
    if (!seg.analysis || !seg.analysis->split.hasRvtNodes) return;
    if (seg.fallback != HE::UUID{} && seg.fallbackHash == liveHash) return;
    const auto* mc = live.registry().try_get<MaterialComponent>(terrain);
    if (!mc) return;
    const Segment::Analysis& an = *seg.analysis;
    // The whole material as RVT off draws it, with the values the landscape resolves (the asset's slots, the entity's overrides).
    MaterialGraph g = matInlineRvt(an.graph);
    for (MatGraphNode& n : g.nodes)
    {
        if (n.type == MatNodeType::ParamFloat || n.type == MatNodeType::ParamColor || n.type == MatNodeType::ParamVec4
            || n.type == MatNodeType::ParamVec2 || n.type == MatNodeType::ParamBool)
        {
            if (const auto it = an.env.params.find(n.s); it != an.env.params.end())
                for (size_t c = 0; c < 4; ++c) n.p[c] = it->second[c];
        }
        else if (n.type == MatNodeType::StaticSwitch)
        {
            if (const auto it = an.env.switches.find(n.s); it != an.env.switches.end()) n.p[0] = it->second ? 1.0f : 0.0f;
        }
    }
    std::string oldGlsl;
    if (seg.fallback != HE::UUID{})
        if (const MaterialAsset* o = m_cm.getMaterial(seg.fallback)) oldGlsl = o->customShaderFragGlsl;
    MaterialAsset fa;
    fa.type = HE::AssetType::Material;
    fa.name = "RvtLandscapeFallback";
    fa.nodeGraphJson = materialGraphToJson(g);
    const bool fresh = seg.fallback == HE::UUID{};
    if (fresh) { seg.fallback = m_cm.registerMaterial(std::move(fa)); ++m_materialsRegistered; }
    else         m_cm.replaceMaterial(seg.fallback, std::move(fa));
    m_cm.regenerateMaterialFromGraph(seg.fallback);
    seg.fallbackHash = liveHash;
    seg.landscapeMaterial = mc->materialAssetId;
    const MaterialAsset* now = m_cm.getMaterial(seg.fallback);
    if (fresh || !now || now->customShaderFragGlsl != oldGlsl)
    {
        if (!fresh) m_renderer.InvalidateMaterial(seg.fallback);
        m_renderer.WarmupMaterials({ seg.fallback });
    }
}

void RvtLandscape::applySwap(entt::entity terrain, Segment& seg, bool pages)
{
    const uint32_t id = static_cast<uint32_t>(entt::to_integral(terrain));
    if (pages)
    {
        m_renderer.SetRvtLandscape(id, seg.landscapeMaterial, seg.consumer);
        seg.fallbackOn = false;
    }
    else if (seg.fallback != HE::UUID{})
    {
        m_renderer.SetRvtLandscape(id, seg.landscapeMaterial, seg.fallback);
        seg.fallbackOn = true;
    }
    else
    {
        m_renderer.SetRvtLandscape(id, HE::UUID{}, HE::UUID{});
        seg.fallbackOn = false;
    }
}

void RvtLandscape::deactivate(entt::entity terrain, Segment& seg)
{
    // Out of range: the pages and the table go (they are the memory); the scratch copy and the materials stay, so a camera that
    // walks over the border back and forth builds nothing and registers nothing.
    applySwap(terrain, seg, false);
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
    seg.baked = false;
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
        applySwap(kv.first, s, !paused);
    }
}

bool RvtLandscape::wanted(bool userSwitch, bool backendCan)
{
    // Read here and not cached with the switch: only the environment is constant for the process, the switch is not.
    static const int env = [] { const char* v = std::getenv("HE_RVT"); return (v && *v) ? (*v == '0' ? 0 : 1) : -1; }();
    return backendCan && (env >= 0 ? env == 1 : userSwitch);
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
        uint64_t full = 0, geo = 0;
        const uint64_t h = stateHash(live, l.e, seg, full, geo);
        const bool rvtNodes = seg.analysis && seg.analysis->split.hasRvtNodes;
        if (seg.attached && seg.baked)
        {
            // The baked base is a snapshot of the exported ground. An edit the pages read (a sculpt stroke, a paint stroke) ends it for
            // good, whether or not the landscape is near enough to hold a volume right now (a pending edit would otherwise repaint
            // the stale picture when the camera comes back): the segment starts over from the page graphs next to this.
            using J = TerrainComponent::RvtJournal;
            const uint8_t kinds = static_cast<uint8_t>((matRvtEditReaches(seg.reads, kRvtEditSculpt) ? J::kHeight : 0)
                                                     | (matRvtEditReaches(seg.reads, kRvtEditPaint) ? J::kWeights : 0));
            if (reg.get<TerrainComponent>(l.e).rvtJournal.selected(kinds).any)
            {
                HE_LOG_INFO(Scene, "RvtLandscape: segment %u leaves the export's baked base (the ground was edited)",
                            static_cast<unsigned>(entt::to_integral(l.e)));
                seg.bakedOff = true;
                remove(l.e, seg);
                ++st.detaches;
                ++st.bakedDropped;
                st.rescanned = true;
            }
        }
        if (seg.attached && h != seg.stateHash)
        {
            if (rvtNodes && seg.volumeLive && geo == seg.geoHash && seg.baker && seg.baker->rebuildRvtViews())
            {
                // RVT 2.0: a parameter before the Write node moved, the landscape itself did not. The scratch copy, the volume and
                // the pool stay: the three page materials are rebuilt in place from the material's Write inputs, and the resident
                // pages are marked stale, so the budget repaints them from the nearest out. (The pool has one stale flag per page,
                // not per layer: a changed roughness repaints the colour layer of the page too.)
                refreshConsumer(seg);
                st.pagesStale += m_manager ? m_manager->invalidateVolume(seg.volume) : 0;
                seg.stateHash = h;
                ++st.pageRebuilds;
            }
            else
            {
                // The view graphs were built from the old values: nothing to patch, start over. (A baked base cannot be patched
                // either: what it was taken of moved, so it is finished.)
                if (seg.baked)
                {
                    HE_LOG_INFO(Scene, "RvtLandscape: segment %u leaves the export's baked base (a page input changed)",
                                static_cast<unsigned>(entt::to_integral(l.e)));
                    seg.bakedOff = true;
                    ++st.bakedDropped;
                }
                remove(l.e, seg);
                ++st.detaches;
                st.rescanned = true;
            }
        }
        else if (seg.attached && full != seg.liveHash)
        {
            // The material changed, but only after the Read node: the live shader follows, no page does.
            ++st.liveOnlyChanges;
            if (seg.baker && seg.baker->rebuildRvtViews(/*views=*/false)) refreshConsumer(seg);
        }
        seg.liveHash = full;
        if (rvtNodes)
        {
            refreshFallback(live, l.e, seg, full);
            if (!seg.swapOn && !seg.fallbackOn && seg.fallback != HE::UUID{}) applySwap(l.e, seg, false);
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
        seg.geoHash = geo;
        ++st.attaches;
        if (seg.baked) ++st.bakedAttaches;
    }
    st.segments = static_cast<uint32_t>(m_segments.size());
    // A landscape whose material has RVT nodes is drawn by its fallback until its pages are ready, and the renderer applies the
    // swap only while a pool exists: make the pool for the first such landscape even when none is near enough to attach yet.
    if (!m_manager)
        for (const auto& kv : m_segments)
            if (kv.second.analysis && kv.second.analysis->split.hasRvtNodes && ensurePool()) break;
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
        // Only the kinds of edit a page reads count (RVT 2.0): a sculpt stroke under pages that read nothing of the height is
        // not a reason to paint anything, nor to bring the scratch copy up to date (nothing in it is read).
        using J = TerrainComponent::RvtJournal;
        const uint8_t kinds = static_cast<uint8_t>((matRvtEditReaches(seg.reads, kRvtEditSculpt) ? J::kHeight : 0)
                                                 | (matRvtEditReaches(seg.reads, kRvtEditPaint) ? J::kWeights : 0));
        const J::Part j = tc.rvtJournal.selected(kinds);
        if (!j.any)
        {
            ++st.journalIgnored;
            tc.rvtJournal.clear();
            continue;
        }
        const glm::vec3 pos = HE::worldPositionOf(live, kv.first);
        if (j.all)
        {
            seg.baker->syncTerrain(tc, true, glm::vec2(0.0f), glm::vec2(0.0f));
            st.pagesStale += m_manager->invalidateVolume(seg.volume);
        }
        else
        {
            seg.baker->syncTerrain(tc, false, glm::vec2(j.minX, j.minZ), glm::vec2(j.maxX, j.maxZ));
            st.pagesStale += m_manager->invalidateRect(seg.volume, glm::vec2(pos.x + j.minX, pos.z + j.minZ),
                                                       glm::vec2(pos.x + j.maxX, pos.z + j.maxZ));
        }
        tc.rvtJournal.clear();
    }

    // What the camera needs across all active volumes, ranked together, one budget; exactly that is painted.
    std::vector<rvt::VolumeView> views;
    for (const auto& kv : byVolume)
    {
        rvt::ViewDesc view;
        view.cameraXZ = cam;
        view.nearRadiusM = m_settings.nearRadiusM;
        views.push_back({ kv.first, view });
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
        // The debug view's stale marks change with the dirty flags, which a normal table does not show: fold their version in
        // (and the switch itself) so toggling the view or invalidating a page uploads the table again, and only then.
        const uint64_t tv = m_manager->tableVersion(seg.volume)
                          + (m_debugView ? ((m_manager->dirtyVersion() + 1) << 40) : 0ull);
        if (tv != seg.uploadedVersion)
        {
            std::vector<std::vector<uint32_t>> mips;
            for (int m = 0; m < m_manager->layout(seg.volume).mipCount(); ++m) mips.push_back(m_manager->pageTable(seg.volume, m, m_debugView));
            if (m_renderer.SetRvtPageTable(seg.consumer, mips)) { seg.uploadedVersion = tv; ++st.tablesUploaded; }
        }
        // Drawn from the pages once the first fill is complete (before that the coarse fallback would show a blurry landscape
        // for a few frames), and it stays on afterwards: a stale or missing page degrades, it does not switch back.
        if (!seg.filled && seg.uploadedVersion == tv && m_manager->missing(seg.volume) == 0) seg.filled = true;
        const bool want = seg.filled && !m_paused;
        if (want != seg.swapOn)
        {
            applySwap(kv.first, seg, want);
            seg.swapOn = want;
        }
        if (seg.swapOn) ++st.activeSegments;
    }

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
    {
        uint32_t bakedSegs = 0;
        for (const auto& kv : m_segments) if (kv.second.baked && kv.second.volumeLive) ++bakedSegs;
        HE_LOG_INFO(Scene, "RvtLandscape: %u page(s) painted, %u marked stale, %u table(s) uploaded, %u attached (%u from the baked base), %u released, %u/%u segment(s) drawn from pages (%u from the baked base), %u resident, %u stale, painting took %.1f ms",
                    st.pagesPainted, st.pagesStale, st.tablesUploaded, st.attaches, st.bakedAttaches, st.detaches, st.activeSegments, st.segments, bakedSegs, ms.resident, ms.dirty, st.paintMs);
    }
    return st;
}

} // namespace HE::landscape
