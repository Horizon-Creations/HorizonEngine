#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>
#include <vector>

// ─── Motion trail (weapon swipe, tyre mark in the air, projectile streak) ────
// The entity drops a point behind itself every `minVertexDistance` metres; the
// points age out after `lifetime` seconds, and what is left is triangulated into
// a camera-facing band every frame (docs/rope-trail-plan.md).
//
// Every frame is the whole point, and it is also why a trail is NOT a runtime
// mesh like a rope: replacing a mesh asset invalidates the renderer's BLAS and
// discards the scene's software BVH, so a per-frame rebuild would tear down the
// GI accelerator of the entire scene sixty times a second. Trails go through a
// RibbonBatch instead — CPU vertices into a dynamic vertex buffer, like the
// debug lines, with no asset and no cache invalidation.
//
// There are deliberately no colour-over-life fields here. The vertex format has
// no colour attribute (Assets.h), so the AGE travels in uv.v — 0 at the tip, 1
// at the tail — and a material graph turns that into colour, opacity or a fade.
// Same division of labour as the particle system, where the gradient also lives
// in the graph rather than in the component.
enum class TrailAlignment : uint8_t { Camera = 0, Frame = 1 };

struct TrailComponent
{
    bool visible  = true;
    // false stops NEW points; the ones already dropped still age out, which is
    // what "stop the trail" has to mean — clearing it instead would leave a
    // frozen band hanging in mid-air.
    bool emitting = true;

    float lifetime          = 0.5f;   // seconds before a point disappears
    float minVertexDistance = 0.05f;  // metres of travel before the next point
    int   maxPoints         = 64;     // ring-buffer cap
    float startWidth        = 0.2f;   // at the tip (the youngest point)
    float endWidth          = 0.0f;   // at the tail
    TrailAlignment alignment = TrailAlignment::Camera;

    HE::UUID materialAssetId;         // its graph reads uv.v = age

    // ── Runtime state (never serialised) ────────────────────────────────────
    // WORLD positions, oldest first, tip last. World, because that is what a
    // trail is: it stays where it was laid down while the entity moves on.
    struct Point
    {
        glm::vec3 worldPos { 0.0f };
        float     age = 0.0f;         // seconds since it was dropped
    };
    std::vector<Point> points;
    glm::vec3 lastEmitPos { 0.0f };
    bool      hasLastEmit = false;
};
