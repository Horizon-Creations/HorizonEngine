#pragma once
// The per-frame driver of the Runtime Virtual Texture for ONE landscape (Thema 333 step 4, docs/rvt-design-2026-11-04.md).
//
// Owned by the application (the editor, the game), called once per frame AFTER the world's terrain update, off by default.
// What it does, in order, and what it costs when nothing happened (nothing: no page is painted, no texture uploaded):
//
//   1. finds the landscape (the first terrain with an Auto Landscape material) and attaches to it: a scratch copy of it
//      (HlodBaker, the three unlit view graphs of its material), a page volume over it, a consumer material;
//   2. drains the landscape's change journal (TerrainComponent::rvtJournal: strokes leave a rect, rebuilds leave "all"),
//      brings the scratch copy up to date and marks exactly the pages under the change stale;
//   3. asks the page manager for what the camera needs (RvtManager::update): new pages, stale pages, within the budget,
//      and paints exactly those (paintPages);
//   4. uploads the page table when it changed;
//   5. tells the renderer (IRenderer::SetRvtLandscape) to draw the landscape's chunks with the consumer material once the
//      first fill is complete, and to stop when the landscape goes away.
//
// A change of the landscape material's parameters or switches cannot be patched into the pages: the view graphs are built
// from them, so the driver re-attaches (a rebuild of the scratch copy and every page).
#include <HorizonScene/RuntimeVirtualTexture.h>
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <memory>
#include <string>

class HorizonWorld;
class ContentManager;
class IRenderer;
namespace HE::landscape { class HlodBaker; }

namespace HE::landscape
{
    class RvtLandscape
    {
    public:
        struct Settings
        {
            uint32_t poolSlots        = 512;     // physical pages (3 x 136^2 x 4 B each = 0.22 MB): the memory budget
            uint32_t maxPagesPerFrame = 8;       // painted per frame, summed over everything stale or new
            float    metersPerTexel   = 0.25f;   // mip 0
            float    nearRadiusM      = 64.0f;   // half extent around the camera that is served at mip 0
        };
        struct FrameStats
        {
            uint32_t pagesPainted = 0;     // this frame
            uint32_t pagesStale   = 0;     // marked stale by the journal this frame
            bool     tableUploaded = false;
            bool     rescanned    = false; // the scratch copy was rebuilt from scratch (a parameter change)
            bool     attached     = false;
            bool     active       = false; // the renderer draws the landscape from the pages
        };

        RvtLandscape(ContentManager& cm, IRenderer& renderer);
        ~RvtLandscape();
        RvtLandscape(const RvtLandscape&) = delete;
        RvtLandscape& operator=(const RvtLandscape&) = delete;

        void             setSettings(const Settings& s) { m_settings = s; }
        const Settings&  settings() const { return m_settings; }

        // Once per frame, after the terrain update. `live` is the world the renderer draws.
        FrameStats tick(HorizonWorld& live, const glm::vec3& cameraPos);
        // Gives the landscape back to its material: the renderer stops swapping, the scratch copy and the pool go.
        void detach();

        bool active() const { return m_active; }
        bool attached() const { return m_attached; }
        const HE::UUID& consumerMaterial() const { return m_consumer; }
        const rvt::RvtManager* manager() const { return m_manager.get(); }
        uint16_t volume() const { return m_volume; }
        uint64_t pagesPaintedTotal() const { return m_paintedTotal; }

    private:
        bool attach(HorizonWorld& live, entt::entity terrain);
        uint64_t materialStateHash(HorizonWorld& live, entt::entity terrain) const;

        ContentManager& m_cm;
        IRenderer&      m_renderer;
        Settings        m_settings;

        std::unique_ptr<HlodBaker>        m_baker;
        std::unique_ptr<rvt::RvtManager>  m_manager;
        uint16_t        m_volume = 0;
        entt::entity    m_terrain = entt::null;
        HE::UUID        m_landscapeMaterial;
        HE::UUID        m_consumer;
        uint32_t        m_pagePx = 0;
        uint64_t        m_materialHash = 0;
        uint64_t        m_uploadedVersion = ~0ull;
        uint64_t        m_paintedTotal = 0;
        bool            m_attached = false;
        bool            m_filled   = false;     // the first fill is complete: the renderer may draw from the pages
        bool            m_active   = false;
        uint64_t        m_failedHash = 0;       // an attach that failed is not retried until something it depends on changes
        entt::entity    m_failedTerrain = entt::null;
    };
}
