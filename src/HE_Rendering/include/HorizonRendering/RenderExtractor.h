#pragma once
#include "../HE_RENDERING_API.h"
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

class HorizonWorld;
class RenderWorld;
class ContentManager;
struct EditorCameraOverride;

namespace HE
{

// ─── Cascaded-shadow-map fit (free functions so they are directly testable) ──
// The maths the extractor's cascade loop is built from. Kept out of the
// extractor itself because it is pure geometry: no ECS, no RenderWorld, no
// frame state — feed it a camera frustum and it hands back the cascade fit.

// Bounding sphere of one camera-frustum slice, in camera space: the centre sits
// `centerDistance` along the camera forward axis and the sphere has `radius`.
struct CascadeSphere
{
    float centerDistance = 0.0f;
    float radius         = 0.0f;
};

// Sphere through the slice's near + far corner rings, centre on the view axis.
// The result depends ONLY on fov/aspect/splits, never on the camera pose, so the
// radius — and with it the cascade's texel size — is constant frame-to-frame,
// which is what keeps the shadows from swimming. The radius is additionally
// quantised (1/16 unit) so small fov/aspect drift cannot wobble the texel
// quantum. tanHalfFovX/Y come from the projection matrix (1/P[0][0], 1/P[1][1]).
HE_RENDERING_API CascadeSphere fitCascadeSphere(float nearD, float farD,
                                                float tanHalfFovX, float tanHalfFovY);

// Practical split scheme: blends the logarithmic and the uniform split series by
// `lambda` (0 = uniform, 1 = logarithmic). Writes cascadeCount + 1 view-space
// distances into outSplits, with outSplits[0] = camNear and
// outSplits[cascadeCount] = shadowFar.
HE_RENDERING_API void computeCascadeSplits(float camNear, float shadowFar, int cascadeCount,
                                           float lambda, float* outSplits);

// Sub-texel NDC shift that anchors the shadow texel grid to the world: it moves
// the projection so the (fixed) world origin lands on a whole shadow texel.
// Add the result to lightProj[3][0] / lightProj[3][1]. Without it the shadow
// edges crawl as the cascade centre rides along with the camera.
HE_RENDERING_API glm::vec2 cascadeTexelSnapOffset(const glm::mat4& lightViewProj,
                                                  float shadowMapRes);

} // namespace HE

// Reads the ECS world each frame and fills a RenderWorld snapshot.
// This is the ONLY class in HorizonRendering that touches HorizonScene —
// it is compiled into the HorizonRendering DLL; backends only see the
// resulting RenderWorld.
class HE_RENDERING_API RenderExtractor {
public:
    // aspectRatio is needed to build the camera projection matrix and comes
    // from the backend's current swapchain size.
    // editorCam, when non-null and active, overrides the scene camera (used by
    // the editor scene view); its projection is built with aspectRatio so it
    // always matches the viewport.
    void extract(HorizonWorld& world, RenderWorld& outWorld, float aspectRatio,
                 const EditorCameraOverride* editorCam = nullptr);

    // Populate outWorld.uiObjects from UISystem::extract.
    // Called after extract() when viewport pixel dimensions are known.
    void extractUI(HorizonWorld& world, float vpWidth, float vpHeight, RenderWorld& outWorld);

    // Optional: when set, mesh renderables get each mesh's real object-space AABB
    // (looked up by asset UUID) as their cull bounds — less overdraw / popping and
    // a tighter shadow-frustum fit. When a mesh isn't resident yet or carries no
    // usable bounds the world bounds are left INVALID (= never culled) rather than
    // proxied by a unit cube, so a large mesh can't disappear while in view; the
    // backend fills in the real bounds once it resolves the asset.
    void setContentManager(ContentManager* cm) { m_contentManager = cm; }

    // Day-night cycle: when enabled, the extractor drives the sun from the time
    // of day (0..1: 0.25 sunrise, 0.5 noon, 0.75 sunset, 0/1 midnight) instead of
    // the scene light's authored direction — it rotates the first directional
    // light (so shading + shadows follow), adds a second moon light on the
    // opposite arc, and fades each out as its luminary sets. Sun/moon colour and
    // brightness are caller-supplied. Render-time only; the scene ECS is never
    // modified.
    void setDayNight(bool enabled, float timeOfDay,
                     const glm::vec3& sunColor, float sunIntensity,
                     const glm::vec3& moonColor, float moonIntensity,
                     float cloudCoverage)
    {
        m_dayNight       = enabled;
        m_timeOfDay      = timeOfDay;
        m_sunColor       = sunColor;
        m_sunIntensity   = sunIntensity;
        m_moonColor      = moonColor;
        m_moonIntensity  = moonIntensity;
        m_cloudCoverage  = cloudCoverage;
    }

private:
    // Overwrites the extracted sun/moon directional lights (and out.ambient /
    // out.sunDirection) from the day-night state pushed via setDayNight.
    // A member because that state lives on the extractor; every other extract()
    // phase is a free function in the .cpp, since none of them need `this`.
    void applyDayNight(RenderWorld& out) const;

    ContentManager* m_contentManager = nullptr;
    bool      m_dayNight       = false;
    float     m_timeOfDay      = 0.5f;
    glm::vec3 m_sunColor       = glm::vec3(1.0f, 0.97f, 0.90f);
    float     m_sunIntensity   = 2.2f;
    glm::vec3 m_moonColor      = glm::vec3(0.55f, 0.65f, 0.95f);
    float     m_moonIntensity  = 0.66f;
    float     m_cloudCoverage  = 0.5f;
};
