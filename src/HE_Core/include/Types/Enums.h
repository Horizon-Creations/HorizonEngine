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

    enum class RendererBackend : uint8_t
    {
        OpenGL,
        Vulkan,
        D3D11,
        D3D12,
        Metal,   // appended last — value is persisted as int in config.json
    };

    // Subset used by Window to select SDL flags / swap-chain creation.
    // Identical to RendererBackend — kept as an alias to avoid redundant casts.
    using GraphicsAPI = RendererBackend;

    enum class ShaderType : uint8_t
    {
        Vertex,
        Fragment,
        Compute,
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
    };

    enum class OS : uint8_t
    {
        Windows,
        Linux,
        macOS,
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
        AnimatorStateMachine // authored states/transitions graph, referenced by AnimatorStateMachineComponent
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

    // NOTE: the gameplay scripting language enum deliberately does NOT live here.
    // It is ::ScriptLanguage in Scripting/ScriptTypes.h, at global scope, because
    // its values are baked into instance ids and the CHUNK_SLNG byte. Re-adding an
    // HE::ScriptLanguage would silently shadow it for any code inside namespace HE.

} // namespace HE
