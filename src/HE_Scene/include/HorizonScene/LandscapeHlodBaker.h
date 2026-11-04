#pragma once
// The far-landscape bake (Thema 228, docs/landscape-hlod-baked-material-2026-11-02.md): a landscape chunk,
// photographed from straight above in three unlit views of its own material (albedo, world normal,
// AO + roughness). The far LOD levels then draw those three small pictures instead of running the
// material's graph (noise, texture bombing, layer blends) for every pixel of a mountain a kilometre away.
//
// The baker works on a scratch world holding a copy of ONE landscape, so nothing else of the scene (props,
// foliage, sky, other landscapes) is in the picture and the live world is not touched; the renderer's world
// is switched for the duration and back at end(). The pictures come from IRenderer::RenderSceneImage with
// EditorCameraOverride::rawOutput, so an unlit view's colour is returned unchanged.
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <Types/UUID.h>

class HorizonWorld;
class ContentManager;
class IRenderer;

namespace HE::landscape
{
    // Three RGBA8 pictures of one chunk, `side` x `side`, row 0 = the chunk's min-Z edge, column 0 = min-X
    // (the way a texture is uploaded, so v = (z - minZ) / size).
    struct HlodChunkImages
    {
        uint32_t             side = 0;
        std::vector<uint8_t> albedo;    // the final albedo, sRGB-encoded (upload it as an sRGB texture)
        std::vector<uint8_t> normal;    // world normal x 0.5 + 0.5, rgb
        std::vector<uint8_t> surface;   // r = AO, g = roughness
    };

    // World-space XZ rectangle of a chunk.
    struct HlodChunkRect
    {
        glm::vec2 min{0.0f};
        glm::vec2 size{0.0f};
    };

    class HlodBaker
    {
    public:
        // `source` holds the landscape (the open scene, or a scene loaded for the export); `restoreTo` is the world
        // the renderer draws when the baker is done (the editor's own; null = `source`).
        HlodBaker(HorizonWorld& source, ContentManager& cm, IRenderer& renderer, HorizonWorld* restoreTo = nullptr);
        ~HlodBaker();
        HlodBaker(const HlodBaker&) = delete;
        HlodBaker& operator=(const HlodBaker&) = delete;

        // Copies `terrain` into the scratch world, builds the three view materials from its (Auto Landscape)
        // material with the terrain's own parameter overrides and switches, builds the chunks and waits for
        // the three pipelines. False with the reason in `why`: not an Auto Landscape material, a rotated or
        // scaled landscape, a renderer without RenderSceneImage.
        bool begin(entt::entity terrain, std::string* why = nullptr);
        // Switches the renderer back to the live world and drops the scratch world. Also called by the destructor.
        void end();

        uint32_t chunksPerSide() const { return m_chunksPerSide; }
        bool     chunkRect(uint32_t cx, uint32_t cz, HlodChunkRect& out) const;

        // Renders the three views of chunk (cx, cz) at side x side texels, each from a picture `supersample`
        // times larger per axis, box filtered down. Main thread; blocks until the GPU has finished.
        bool bake(uint32_t cx, uint32_t cz, uint32_t side, uint32_t supersample, HlodChunkImages& out);
        // Same for any square world rectangle (the whole landscape in one picture is the test's reference).
        bool bakeRect(const HlodChunkRect& rect, uint32_t side, uint32_t supersample, HlodChunkImages& out);
        // Runtime Virtual Textures (Thema 333): the same three views of `rect`, `px` x `px`, but each goes straight into
        // slice `slot` of the renderer's RVT pool (layer 0 BaseColor, 1 Normal, 2 Surface), no readback, no filter.
        bool renderRvtPage(const HlodChunkRect& rect, uint32_t px, uint32_t slot);
        // The scratch landscape drawn top-down with ANY material (a witness: the RVT consumer against the analytic
        // ramp), `px` x `px` RGBA8 read back, linear (no sRGB encode).
        bool renderMaterialImage(const HE::UUID& material, const HlodChunkRect& rect, uint32_t px, std::vector<uint8_t>& rgba);

        // The material graph groups the parameters of the landscape's material came from (index into
        // kAutoLandscapeGroups), for the caller's log line.
        int group() const { return m_group; }

        // A witness hook (HE_DUMP_RVTTEST): the three view materials the baker draws (BaseColor, Normal, Surface),
        // replaced by materials of the caller's, e.g. one whose colour is a function of the world position.
        void setViewMaterials(const HE::UUID (&mats)[3]) { for (int i = 0; i < 3; ++i) { m_viewMat[i] = mats[i]; m_rvtSeen[i] = false; } }
        void viewMaterials(HE::UUID (&mats)[3]) const { for (int i = 0; i < 3; ++i) mats[i] = m_viewMat[i]; }

    private:
        bool renderView(const HE::UUID& material, const HlodChunkRect& rect, uint32_t px, bool srgb,
                        std::vector<uint8_t>& rgba, int rvtSlot = -1, int rvtLayer = 0, bool invalidate = true);

        HorizonWorld&   m_live;      // the landscape is read from here
        HorizonWorld&   m_restore;   // and the renderer goes back to this one
        ContentManager& m_cm;
        IRenderer&      m_renderer;
        std::unique_ptr<HorizonWorld> m_scratch;
        entt::entity    m_terrain = entt::null;     // the copy, in m_scratch
        HE::UUID        m_viewMat[3]{};             // BaseColor, Normal, Surface
        uint32_t        m_chunksPerSide = 0;
        float           m_topY = 0.0f, m_bottomY = 0.0f;   // world heights the camera has to cover
        std::vector<HlodChunkRect> m_rects;          // [cz * chunksPerSide + cx]
        int             m_group = -1;
        bool            m_rvtSeen[3] = { false, false, false };   // a view material the renderer has already resolved for pages
        bool            m_active = false;
    };

    // Box filter: `src` is (side*ss) x (side*ss) RGBA8, the result side x side.
    std::vector<uint8_t> boxDownsampleRgba8(const std::vector<uint8_t>& src, uint32_t side, uint32_t ss);
}
