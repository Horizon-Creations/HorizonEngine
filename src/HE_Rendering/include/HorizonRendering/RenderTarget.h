#pragma once
#include <cstdint>

// ─── Render-target abstraction ──────────────────────────────────────────────
// Backend-agnostic description of where a render pass draws and what it samples.
// The RenderGraph hands each pass's RenderPassIO to the backend, which creates
// and binds the matching target. id 0 (kBackbufferTarget) is the active window
// or editor-viewport target the backend is already rendering into; any other id
// is an offscreen target the backend allocates on demand (shadow maps, HDR
// scene color, …). Creation of offscreen targets lands with the passes that
// need them (ShadowPass, HDR PostProcess); this header defines the contract so
// all backends speak the same language.

enum class RenderTargetFormat : uint8_t
{
    RGBA8,     // standard LDR color
    RGBA16F,   // HDR color (tone-mapped down to the backbuffer later)
    Depth,     // depth-only (shadow maps, depth pre-pass)
};

enum class RenderTargetSize : uint8_t
{
    Viewport,  // matches the current output size
    Fixed,     // explicit width/height (e.g. a 2048² shadow map)
};

using RenderTargetId = uint32_t;
constexpr RenderTargetId kBackbufferTarget = 0; // the active window / viewport target
constexpr RenderTargetId kShadowMapTarget  = 1; // directional-light depth map
constexpr RenderTargetId kSceneColorTarget = 2; // HDR scene color (pre-tonemap)

struct RenderTargetDesc
{
    RenderTargetId     id        = kBackbufferTarget;
    RenderTargetFormat format    = RenderTargetFormat::RGBA8;
    RenderTargetSize   sizeMode  = RenderTargetSize::Viewport;
    uint32_t           width     = 0;     // used when sizeMode == Fixed
    uint32_t           height    = 0;
    bool               depth     = true;  // also allocate a depth attachment
    const char*        debugName = "backbuffer";
};

struct RenderPassIO
{
    RenderTargetDesc output;             // where this pass renders
    RenderTargetId   inputs[4] = {};     // offscreen targets this pass samples
    uint32_t         inputCount = 0;
};
