#pragma once
// The per-frame driver of the Runtime Virtual Texture for the landscapes of a world (Thema 333 steps 4 and 5,
// docs/rvt-design-2026-11-04.md).
//
// Owned by the application (the editor, the game), called once per frame AFTER the world's terrain update, off by default.
//
// One landscape = one SEGMENT = one page volume (a landscape cut into tiles is several landscapes, each a segment); every
// segment draws from the ONE page pool and one per-frame paint budget. Per frame, and what it costs when nothing happened
// (nothing: no page is painted, no texture uploaded):
//
//   1. finds the landscapes (terrains with an Auto Landscape material), and for each one near enough to the camera
//      (Settings::activationRadiusM, with hysteresis) attaches a segment: a scratch copy of it (HlodBaker, the three unlit
//      view graphs of its material), a volume in the pool, a consumer material; far ones are not drawn from pages at all;
//   2. drains each active landscape's change journal (TerrainComponent::rvtJournal: strokes leave a rect, rebuilds leave "all"),
//      brings its scratch copy up to date and marks exactly the pages under the change stale;
//   3. asks the page manager for what the camera needs across ALL active volumes (RvtManager::updateAll: new pages, stale
//      pages, ranked together, one budget) and paints exactly those;
//   4. uploads a segment's page table when it changed;
//   5. tells the renderer (IRenderer::SetRvtLandscape) to draw a landscape's chunks with its consumer material once its first
//      fill is complete, and to stop when it goes out of range or away.
//
// Deactivating a segment (out of range) drops its pages and its table, not its materials or its scratch copy, so a camera that
// walks back and forth over the border grows nothing; a destroyed landscape frees its volume. A change of the landscape
// material's parameters or switches cannot be patched into the pages (the view graphs are built from them): the segment
// re-attaches.
#include <HorizonScene/RuntimeVirtualTexture.h>
#include <MaterialGraph/RvtSplit.h>
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class HorizonWorld;
class ContentManager;
class IRenderer;
struct MaterialComponent;
namespace HE::landscape { class HlodBaker; }
namespace HE { struct ProjectRvtSettings; }

namespace HE::landscape
{
    class RvtLandscape
    {
    public:
        struct Settings
        {
            uint32_t poolSlots        = 512;     // physical pages (3 x 136^2 x 4 B each = 0.22 MB): the memory budget; fixed at the first attach
            uint32_t maxPagesPerFrame = 8;       // painted per frame, summed over everything stale or new, all landscapes; fixed at the first attach
            float    metersPerTexel   = 0.25f;   // mip 0
            float    nearRadiusM      = 64.0f;   // half extent around the camera that is served at mip 0
            float    activationRadiusM = 600.0f; // a landscape closer than this (to its rectangle) is drawn from pages; farther ones are not
            uint32_t maxAttachesPerFrame = 1;    // attaching builds a scratch copy (100-250 ms): one per frame, nearest first
            // Native near view: the chunks closer than the distance where one mip-0 texel covers one pixel keep the landscape's
            // own material (sharp at any distance), the rest is drawn from the pages. Off = every chunk from the pages.
            bool     nativeNear       = true;
            bool     tileNativeNear   = false;   // the tile landscape's ground: off by default (it costs more there, ProjectSettings.h)
        };
        struct FrameStats
        {
            uint32_t pagesPainted  = 0;    // this frame, all segments
            uint32_t pagesStale    = 0;    // pages marked stale (in at least one layer) by the journals and page rebuilds this frame
            uint32_t layersStale[3] = { 0, 0, 0 };   // of those, how many had layer k (colour, normal, AO + roughness) marked this frame
            uint32_t tablesUploaded = 0;
            uint32_t attaches      = 0;    // segments attached this frame
            uint32_t detaches      = 0;    // segments deactivated or removed this frame
            uint32_t segments      = 0;    // landscapes known (attached or not)
            uint32_t activeSegments = 0;   // drawn from the pages now
            bool     rescanned     = false; // a segment was rebuilt from scratch (a parameter change)
            // RVT 2.0 step 6 (RvtBaseBake.h): segments attached with the export's baked base, and segments that left it.
            uint32_t bakedAttaches = 0;     // attached this frame: pages are painted from the baked pictures, the base graph is not evaluated
            uint32_t bakedDropped  = 0;     // this frame: a ground edit or a change before the Write node ended the baked base of a segment
            // RVT 2.0 step 3 (RvtSplit.h): what the tick did NOT repaint, and why.
            uint32_t journalIgnored = 0;   // terrain edits no page reads (a paint stroke under pages that only read the height, ...)
            uint32_t liveOnlyChanges = 0;  // material changes that sit after the RVT Read node: the live shader follows, no page is touched
            uint32_t pageRebuilds = 0;     // segments whose page materials were rebuilt in place (a parameter before the Write node), no re-attach
            double   paintMs       = 0.0;   // wall clock of the whole painting of this frame (what the user feels, on every backend)
            uint32_t feedbackPages = 0;    // pages the GPU feedback holds wanted now (0 on a backend without it)
            uint32_t nearChunks    = 0;    // chunks drawn with their own material (native near view)
        };

        RvtLandscape(ContentManager& cm, IRenderer& renderer);
        ~RvtLandscape();
        RvtLandscape(const RvtLandscape&) = delete;
        RvtLandscape& operator=(const RvtLandscape&) = delete;

        // Whether RVT is wanted at all (editor + game): the global switch (Preferences > Rendering in the editor, the exported
        // config.json in the game) on a backend that can hold a virtual texture (Capabilities::supportsRvt, which also covers GL
        // without enough texture units and Vulkan at the sampler minimum). HE_RVT=1 / HE_RVT=0 in the environment force it on / off
        // (the witnesses and the MCP loop rely on that). Callers hand the answer to matSetRvtEnabled too, so a material generated
        // with RVT off is the inline graph (RvtSplit.h).
        static bool     wanted(bool userSwitch, bool backendCan);
        // Pool, texel size, radii, paint budget and the native near view: the project's tuning (Config/ProjectSettings.json "rvt",
        // Project Settings > World > Virtual Texture), read by editor and game.
        static Settings settingsFromProject(const HE::ProjectRvtSettings& p);

        // The pool size and the texel size shape the pool and every volume: a change of either gives every landscape back and
        // starts over with the next tick (one rebuild, never one per frame: the editor changes them only on commit). The
        // radii and the paint budget are read every tick and change live.
        void             setSettings(const Settings& s);
        const Settings&  settings() const { return m_settings; }

        // Once per frame, after the terrain update. `live` is the world the renderer draws.
        FrameStats tick(HorizonWorld& live, const glm::vec3& cameraPos);
        // The projection the near radius is measured against (vertical field of view, image height in pixels): the native
        // near view swaps a chunk where a mip-0 texel shrinks below a pixel. Unset = 60 degrees, 1080 px.
        void setView(float fovYRadians, float viewportHeightPx) { m_fovY = fovYRadians; m_viewportH = viewportHeightPx; }
        // The distance from the camera beyond which the pages are sharp enough (one mip-0 texel per pixel). For a texel of
        // `metersPerTexel`; without one, the setting's. A volume too large for the setting has a coarser texel (attach clamps
        // it to 2048 pages a side), and its radii must follow THAT texel: with the setting's, a 100 km landscape at the
        // 1.6 cm preset swapped to 0.5 m pages from 15 m out, a blurred ring where the native material belonged.
        float nativeNearRadius(float metersPerTexel = 0.0f) const;
        // Gives every landscape back to its material: the renderer stops swapping, the scratch copies and the pool go.
        void detach();
        // Draws the landscapes from their own material for a moment (a comparison) and back, without touching anything else.
        void pauseSwap(bool paused);
        // The editor's RVT debug view is on (View Mode > Virtual Texture): the page tables carry the stale mark
        // (rvt::kPageTableStale) and are uploaded again when a page turns stale or is repainted. Off, nothing differs.
        void setDebugView(bool on) { m_debugView = on; }
        bool debugView() const { return m_debugView; }

        // What a debug view shows (editor: the RVT debug view, step 5): the driver's state as of the last tick, and the
        // mip-0 page map of the nearest landscape that has a volume. A snapshot, copied: nothing in it points into the driver.
        struct DebugInfo
        {
            bool     running = false;       // the pool exists
            uint32_t segments = 0, activeSegments = 0;
            uint32_t poolSlots = 0, resident = 0, stale = 0, starved = 0;
            double   lastPaintMs = 0.0;     // painting of the last tick that painted anything
            uint32_t lastPagesPainted = 0;
            uint64_t pagesPaintedTotal = 0;
            uint32_t materialsRegistered = 0;
            int      gridSide = 0;          // mip-0 pages per side of the shown volume (0 = none)
            std::vector<uint8_t> grid;      // gridSide^2 cells, row-major: 0 missing, 1 resident, 2 resident but stale
            float    metersPerPage = 0.0f;  // mip-0 page edge in metres
        };
        DebugInfo debugInfo() const;

        bool active() const;                                  // at least one landscape is drawn from pages
        size_t segmentCount() const { return m_segments.size(); }
        bool active(entt::entity landscape) const;
        bool attached(entt::entity landscape) const;
        const rvt::RvtManager* manager() const { return m_manager.get(); }
        // The hash layer k's resident pages were last marked at (a test / witness hook), 0 for an unknown landscape.
        uint64_t layerHashOf(entt::entity landscape, int layer) const
        { const auto it = m_segments.find(landscape); return it != m_segments.end() && layer >= 0 && layer < 3 ? it->second.layerHash[layer] : 0; }
        uint16_t volumeOf(entt::entity landscape) const;
        HE::UUID consumerOf(entt::entity landscape) const;
        // Whether the segment's pages come from the export's baked base right now (a witness / test hook).
        bool bakedBase(entt::entity landscape) const { const auto it = m_segments.find(landscape); return it != m_segments.end() && it->second.baked; }
        uint64_t pagesPaintedTotal() const { return m_paintedTotal; }
        uint32_t materialsRegistered() const { return m_materialsRegistered; }   // view + consumer materials made so far (never freed)

    private:
        struct Segment
        {
            std::unique_ptr<HlodBaker> baker;     // the scratch copy; null before the first attach
            uint16_t   volume = 0;
            HE::UUID   landscapeMaterial;         // what the chunks are drawn with without the swap
            HE::UUID   consumer;                  // kept across deactivations; made anew when the volume it was baked for moved or resized
            rvt::VolumeDesc consumerDesc;         // the origin/extent/texel size baked into the consumer's parameter defaults
            uint16_t   consumerVolume = 0;        // the volume id baked into it (B.y), which the feedback keys carry
            std::unordered_set<uint32_t> nearChunks;   // chunk entities drawn with the landscape's own material (native near view)
            uint64_t   stateHash = 0;             // what the view graphs and the volume were built from
            uint64_t   geoHash = 0;               // the landscape's size and place alone (the part of stateHash a page change leaves)
            uint64_t   liveHash = 0;              // the whole material (RVT 2.0): moves with a change after the Read node too
            // RVT 2.0 step 4: the whole material with the Write/Read pair inlined (what RVT off draws), for the moments the pages
            // cannot be drawn: out of range, before the first fill, paused. A material with RVT nodes would otherwise draw a Read
            // of a pool that holds nothing there.
            HE::UUID   fallback;
            uint64_t   fallbackHash = 0;          // the liveHash it was made at
            bool       fallbackOn = false;        // the renderer draws it for this landscape now
            // RVT 2.0 step 3: the material graph cut at its RVT nodes, and what the pages read of the ground.
            struct Analysis
            {
                uint64_t         graphKey = 0;    // hash of the graph text the split was made from
                uint64_t         epoch = ~0ull;   // ContentManager::contentEpoch() the asset revisions were read at
                MaterialGraph    graph;
                MatRvtSplit      split;
                uint64_t         state = 0;       // matRvtMaterialState().state under the current values
                MatRvtLayerState layer;           // per pool layer: what it is painted from now, and what of the ground it reads
                MatRvtEnv        env;             // asset revisions, function reads (parameters are refilled every tick)
            };
            std::shared_ptr<Analysis> analysis;   // null: not an RVT-node material (the Thema 333 path: the whole material is the page input)
            uint32_t   reads = 0xFFFFFFFFu;       // kRvtReads* the pages need from the ground; everything for a legacy material
            // Per pool layer (colour, normal, AO + roughness): the hash the resident pages were last marked/painted at. A page change
            // marks stale only the layers whose hash moved (Analysis::layer), a terrain edit only the layers that read that ground.
            uint64_t   layerHash[3] = { 0, 0, 0 };
            uint64_t   uploadedVersion = ~0ull;
            // RVT 2.0 step 6: the pages are painted from the pak's baked pictures (HlodBaker::bakedViews), the page graphs are never
            // built. Ends for good (bakedOff) with the first ground edit the pages read or the first change before the Write node:
            // the baked base is a snapshot of the exported scene, and a game that edits it needs the graphs again.
            bool       baked = false;
            bool       bakedOff = false;
            uint64_t   bakedKey = 0;
            bool       attached = false;          // scratch copy + volume exist
            bool       volumeLive = false;        // the volume is in the manager (pages may exist)
            bool       filled = false;            // the first fill is complete
            bool       swapOn = false;            // the renderer draws this landscape from the pages
            bool       meshesReleased = false;    // the scratch chunks' GPU meshes were dropped after the pages went idle (HlodBaker::releaseIdleMeshes)
            uint64_t   failedHash = 0;            // an attach that failed is not retried until something it depends on changes
            bool       failed = false;
        };

        bool attach(HorizonWorld& live, entt::entity terrain, Segment& seg);
        void deactivate(entt::entity terrain, Segment& seg);       // out of range: pages and table go, materials stay
        void remove(entt::entity terrain, Segment& seg);           // the landscape is gone: the volume goes too
        // Returns the hash of what the PAGES are built from. For a material with RVT nodes that is the page subgraphs' inputs
        // (matRvtPageStateHash) and nothing after the Read node; otherwise the whole material. `fullHash` is always the whole
        // material, so the caller can tell "a live-only change" from "a page change".
        // `geoHash` is the landscape's size and place alone: a segment whose geometry is unchanged keeps its scratch copy and
        // volume across a page change (the views are rebuilt in place, the resident pages repainted).
        uint64_t stateHash(HorizonWorld& live, entt::entity terrain, Segment& seg, uint64_t& fullHash, uint64_t& geoHash);
        void refreshAnalysis(HorizonWorld& live, entt::entity terrain, Segment& seg);
        // The consumer material of an RVT-node landscape: its live graph (the part after the Read node, with this landscape's
        // parameters) and the volume vectors the Read node takes. Made on the first call, replaced in place afterwards (the
        // renderer is told only when the shader text changed: a parameter change flows through the parameter block).
        void refreshConsumer(Segment& seg);
        // The inline fallback for a landscape whose material has RVT nodes; kept up to date with the material's values.
        void refreshFallback(HorizonWorld& live, entt::entity terrain, Segment& seg, uint64_t liveHash);
        // Tells the renderer what to draw for the landscape: the consumer (pages) or, where a fallback exists, the fallback.
        void applySwap(entt::entity terrain, Segment& seg, bool pages);
        // Marks the resident pages of the segment's volume stale in `layers` (bit k = pool layer k), all of them or the ones under the
        // world rect (minX, minZ, maxX, maxZ). Returns how many pages that touched; the per-layer counts go into st.layersStale.
        uint32_t markStale(Segment& seg, uint8_t layers, FrameStats& st, const glm::vec4* rectXZ);
        bool ensurePool();
        void note(const std::string& why);

        ContentManager& m_cm;
        IRenderer&      m_renderer;
        Settings        m_settings;

        std::unique_ptr<rvt::RvtManager>  m_manager;      // one, shared: the pool is one
        uint32_t        m_pagePx = 0;
        bool            m_poolMade = false;
        bool            m_paused = false;
        bool            m_debugView = false;
        FrameStats      m_lastPainting;              // the last tick that painted a page (for the debug view)
        entt::entity    m_debugEntity = entt::null;  // the nearest landscape with a volume, as of the last tick
        std::map<entt::entity, Segment>   m_segments;
        uint64_t        m_paintedTotal = 0;
        std::chrono::steady_clock::time_point m_lastPaintAt = std::chrono::steady_clock::now();   // the last tick that painted a page
        uint64_t        m_ticks = 0;
        uint32_t        m_materialsRegistered = 0;
        std::string     m_lastNote;
        uint32_t        m_lastStarved = 0;
        bool            m_starvedWarned = false;
        float           m_fovY = 1.0471976f;         // setView
        float           m_viewportH = 1080.0f;
        // The GPU feedback, kept for a while: a frame samples one pixel in 64, so a page stays wanted for kFeedbackTicks ticks
        // after it was last reported (key -> tick). Without that, a page some pixels need would be evictable 63 frames in 64.
        std::unordered_map<uint32_t, uint64_t> m_feedback;
        std::vector<uint32_t> m_feedbackScratch;
        void updateNearChunks(HorizonWorld& live, const glm::vec3& cameraPos, FrameStats& st);

        // ── The tile landscape's ground (LandscapeRuntime, Thema 326; RvtLandscapeTiles.cpp) ──
        // One more user of the same pool. No scratch copy: a page is the live ground, drawn by the renderer's landscape provider
        // in an otherwise empty world. The volume is the whole world where the table can hold it, else a square around the camera
        // that is replaced, once its successor is filled, when the camera leaves its inner half.
        struct TileGround
        {
            uint64_t   key = 0;                  // what the views and the consumer were built from (tileGroundKey); 0 = nothing built
            uint64_t   failedKey = 0;            // a material that cannot be split is not tried again until it changes
            HE::UUID   view[3], consumer;        // kept for good (same ids, new graphs)
            bool       seen[3] = { false, false, false };
            uint16_t   volume = 0, pending = 0;
            bool       volumeLive = false, pendingLive = false;
            bool       updated = false, pendingUpdated = false;   // the manager has looked at it (missing() means something)
            glm::dvec2 anchor{ 0.0 }, pendingAnchor{ 0.0 };       // absolute XZ of the min corner; the manager counts from there (floats)
            glm::dvec2 writtenAt{ 1e300 };       // anchor - world origin the consumer's volume vector holds
            uint16_t   writtenVolume = 0xFFFF;
            uint64_t   uploadedVersion = ~0ull;
            bool       filled = false, swapOn = false;
            std::shared_ptr<HorizonWorld> scratch;   // empty: what a page camera sees besides the ground
        };
        TileGround m_tiles;
        bool tileGroundWanted(HorizonWorld& live);                                  // builds the materials; false = no tile ground to serve
        void tileGroundVolumes(HorizonWorld& live, const glm::dvec2& camAbs, FrameStats& st);
        void tileGroundViews(const glm::dvec2& camAbs, std::vector<rvt::VolumeView>& views);
        bool tileGroundOwns(uint16_t volume) const { return (m_tiles.volumeLive && m_tiles.volume == volume) || (m_tiles.pendingLive && m_tiles.pending == volume); }
        uint32_t paintTileGround(HorizonWorld& live, uint16_t volume, const std::vector<rvt::PageRender>& pages);
        void tileGroundTables(HorizonWorld& live, FrameStats& st);
        void removeTileGround();
    };

    // The tile ground's material as one graph with every parameter at the value the ground is drawn with (the asset's, then the
    // landscape entity's overrides): what the page views, the consumer and the RVT-free stand-in are cut from. `key` changes
    // whenever the result would; 0 = the material is not there or has no graph.
    uint64_t tileGroundKey(ContentManager& cm, const HE::UUID& source, const MaterialComponent* mc);
    bool     tileGroundGraph(ContentManager& cm, const HE::UUID& source, const MaterialComponent* mc, MaterialGraph& out);
}
