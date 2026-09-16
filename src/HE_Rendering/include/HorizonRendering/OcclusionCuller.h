#pragma once
#include <cstdint>
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <vector>

class ContentManager; // global namespace (HE_Core's ContentManager is not namespaced)

// ─── CPU software occlusion culling ──────────────────────────────────────────
// Refines the FrustumCuller's result: an object whose whole bounding box sits
// behind opaque geometry that is closer to the camera is dropped from the
// visible set before sorting and drawing. Backend-independent (works on the
// extracted RenderWorld and the mesh CPU data the ContentManager already holds)
// and latency-free — GPU occlusion queries (GL_ANY_SAMPLES_PASSED /
// MTLVisibilityResult) answer one frame late, so a fast pan pops objects in;
// this answers for THIS frame's camera.
//
// How: a small depth buffer (`bufferWidth` × aspect) is rasterized on the CPU
// from the frustum-visible OCCLUDERS — opaque, big on screen, not too many
// triangles. Every pixel keeps the NEAREST occluder, but each triangle writes
// the FARTHEST depth over the pixel's footprint (centre depth ± half the depth
// gradient), so the buffer is conservative: it never claims a surface closer
// than it is. Then every frustum-visible object's 8 AABB corners are projected,
// the screen rect grown by one pixel, and the object is hidden only when its
// NEAREST corner is farther than the buffer at EVERY pixel of that rect.
// Result: a visible object is never culled (the rules err toward "keep"); a
// hidden one usually is. The picture with culling on is byte-identical to the
// picture with it off — that is the oracle the headless witness checks.
//
// Occluder eligibility (anything else could be see-through — an occluder that
// is not solid culls things that are in fact visible): the object's effective
// material is opaque (opacity × instanceTint.a ≥ the sorter's opaque threshold,
// blend mode Opaque — NOT Masked, whose shader discards texels — and no
// World-Position-Offset vertex body, which moves the surface on the GPU), and
// `contributesAO` (particles/icons opt out). A multi-section mesh is checked per
// section; only the opaque sections rasterize. Back faces DO rasterize — the
// scene passes draw without face culling, so a back face really hides things.
//
// Depth here is the clip-space w (view depth); 1/w is interpolated affinely in
// screen space, which is exact for a perspective projection. The GL-convention
// matrix in RenderWorld::camera is used as is: only x, y and w are read, never
// z, so the backends' clip-space remaps do not matter.
//
// Known limits of the pixel-centre sampling (accepted for v1, both are below
// one buffer pixel — ~7 screen pixels at 1080p with the default width): a gap
// between two SEPARATE occluders narrower than a buffer pixel can close, and an
// object whose nearest point touches an occluder's depth crease inside one
// pixel can be judged by the nearer face. Shared edges WITHIN one mesh are
// exact (see the rasterizer's canonical edge function).
class HE_RENDERING_API OcclusionCuller {
public:
    struct Settings
    {
        bool  enabled              = false;
        // Depth buffer width in pixels; the height follows the camera aspect.
        int   bufferWidth          = 256;
        // Fraction of the screen an occluder's projected bounds must cover.
        // Small things rarely hide anything and cost triangles to rasterize.
        float minOccluderScreenArea = 0.01f;
        // A mesh above this triangle count is never an occluder (CPU budget).
        int   maxOccluderTriangles = 16384;
        // At most this many occluders per frame, biggest on screen first.
        int   maxOccluders         = 48;
    };

    struct Stats
    {
        uint32_t occluders         = 0; // objects rasterized into the buffer
        uint32_t occluderTriangles = 0;
        uint32_t tested            = 0; // frustum-visible objects tested
        uint32_t culled            = 0; // …of which hidden by an occluder
    };

    void            setSettings(const Settings& s) { m_settings = s; }
    const Settings& settings() const               { return m_settings; }

    // Refine `visible` (the FrustumCuller's per-object result, mirrors
    // world.objects) in place. Returns the number of objects newly hidden.
    // `cm` may be null — then nothing has mesh data, nothing is an occluder,
    // and the call is a no-op. Never loads: only getStaticMesh/getMaterial,
    // which do not move the asset vectors.
    uint32_t refine(const RenderWorld& world, const ContentManager* cm,
                    std::vector<uint8_t>& visible);

    const Stats& stats() const { return m_stats; }

    // The rasterized buffer of the last refine (for tests and debugging):
    // one float per pixel, 1/w of the nearest conservative occluder surface,
    // 0 where nothing was drawn. Row 0 is the TOP of the screen.
    int                       width()  const { return m_width;  }
    int                       height() const { return m_height; }
    const std::vector<float>& depth()  const { return m_depth;  }

private:
    Settings m_settings;
    Stats    m_stats;
    int      m_width  = 0;
    int      m_height = 0;
    std::vector<float> m_depth;    // width*height, 1/w (0 = far / empty)
    std::vector<float> m_tileMin;  // per 8×8 tile: min of m_depth
    std::vector<float> m_tileMax;  // per 8×8 tile: max of m_depth
    int      m_tilesX = 0;
    int      m_tilesY = 0;
    std::vector<glm::vec4> m_clipScratch; // per-occluder transformed vertices
};
