#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// HE::Enums  —  single source of truth for all engine-wide enum types
//
// Usage:  #include <Types/Enums.h>
//
// All enums live in the HE namespace so they are unambiguous across modules.
// Original headers forward to this file, so existing code keeps compiling.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE
{
    // ── Rendering ─────────────────────────────────────────────────────────────

    // The one and only name for "which RHI". Window uses it to pick SDL flags /
    // swap-chain creation, RendererFactory to pick a backend implementation, and
    // the asset pipeline to tag precompiled shader variants (1u << value).
    //
    // The numeric values are PERSISTED — as int in config.json ("RHI"), as the
    // uint8 `backend` field of MaterialShaderVariant/ParticleShaderVariant inside
    // .hpak/.hasset chunks, and as a bitmask (1u << value) in export profiles'
    // shaderBackends. Never reorder; only append.
    enum class RendererBackend : uint8_t
    {
        OpenGL,  // 0
        Vulkan,  // 1
        D3D11,   // 2
        D3D12,   // 3
        Metal,   // 4 — appended last
    };

    enum class ShaderType : uint8_t
    {
        Vertex,
        Fragment,
        Compute,
	};

    // Which lighting architecture the scene pass uses. Forward shades every
    // fragment in the geometry pass (today's path, every backend); Deferred
    // writes a G-buffer and shades once per visible pixel in a fullscreen
    // resolve (Metal + OpenGL, Capabilities::supportsDeferredRendering).
    // The numeric values are PERSISTED as int in config.json ("RenderPath") —
    // never reorder; only append.
    enum class RenderPath : uint8_t
    {
        Forward  = 0,
        Deferred = 1,
    };

    // ── Window ────────────────────────────────────────────────────────────────

    enum class WindowMode : uint8_t
    {
        Windowed,
        Fullscreen,
        Borderless,
    };

    // ── Diagnostics ───────────────────────────────────────────────────────────

    enum class LogLevel : uint8_t
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Critical,
        // Filter sentinel only — never the level of an actual record. Setting a
        // category's verbosity to Off silences it completely (see Diagnostics/Log.h).
        Off,
    };

    // ── Assets ────────────────────────────────────────────────────────────────

    enum class AssetType : uint32_t
    {
        Unknown,
        StaticMesh,
        SkeletalMesh,
        Texture,
        Material,
        Scene,
        Script,
        Audio,
        Font,
        Shader,
        Prefab,
        AnimationClip,
        PropertyAnimClip, // property/transform/material animation clips
        MaterialFunction, // reusable material sub-graph (node editor), inlined at codegen
        Widget,           // UI widget tree (UMG-style widget editor), instantiated at play start
        HorizonCodeClass, // standalone HorizonCode graph (visual-scripting class)
        InputAction,      // named logical input (Button or Axis), referenced by mapping contexts
        InputMappingContext, // key/axis bindings that drive InputAction states at runtime
        ParticleSystem,   // particle emitter node graph (Emitter Output + math/const/random nodes)
        AnimatorStateMachine, // authored states/transitions graph, referenced by AnimatorStateMachineComponent
        StructType,       // user-defined struct (named typed fields) — see HE::TypeRegistry
        EnumType,         // user-defined enum (named int-backed entries) — see HE::TypeRegistry
        SaveGameTemplate  // savegame field schema (typed fields + defaults), consumed by HE::api::save
    };

    enum class TextureFormat : uint32_t
    {
        RGBA8 = 0,
        BC1   = 1,   // DXT1 — opaque, compressed
        BC3   = 2,   // DXT5 — alpha, compressed
        BC7   = 3,   // high quality, compressed
    };

    // ── Scene ─────────────────────────────────────────────────────────────────

    enum class LightType : uint8_t
    {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
    };

    enum class RigidBodyType : uint8_t
    {
        Static    = 0,
        Dynamic   = 1,
        Kinematic = 2,
    };

    enum class ColliderShape : uint8_t
    {
        Box     = 0,
        Sphere  = 1,
        Capsule = 2,
    };

    enum class SerializeFormat : uint8_t
    {
        JSON,    // editor — human-readable, versioned
        Binary,  // packaged game — compact, fast to load
    };

    // NOTE: the gameplay scripting language enum is HE::ScriptLanguage, and it
    // lives in Scripting/ScriptTypes.h rather than here — it is the scripting
    // subsystem's vocabulary, not a general engine enum. Do NOT add a second
    // spelling of it here: two visible ScriptLanguages would let unqualified uses
    // inside namespace HE silently bind to the wrong one. Its values are baked
    // into script instance ids and the CHUNK_SLNG byte, so they must never be
    // renumbered either.

} // namespace HE
