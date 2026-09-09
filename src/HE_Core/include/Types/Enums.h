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
        Metal,   // 4
        // 5 — appended last. Not a GPU backend at all: a CPU rasterizer that
        // draws NOTHING BUT UI (docs/he-apps-plan.md Block G). It is what an
        // application with "Advanced Shader Effects" switched off ships with,
        // so that it needs no GPU, no driver and no shader translation. Every
        // 3D feature it is asked about, it says no to.
        Software,
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
        SaveGameTemplate, // savegame field schema (typed fields + defaults), consumed by HE::api::save
        Theme,            // UI colour roles + sizes + shadows, light and dark (docs/he-apps-plan.md D1)
        BoneMask,         // which joints an animation layer may touch, by joint NAME (HE::BoneMask)
        BlendSpace        // N clips in a 1D/2D parameter space, mixed by parameter (HE::BlendSpace)
    };

    // Does this kind of asset travel over a collaboration session?
    //
    // Authored data — graphs, materials, UI, type definitions — yes: it is small,
    // and two people editing it at once is the entire point. Imported binary
    // media — meshes, textures, audio, fonts — no: it is large, it is not edited
    // during a session, and keeping it in step is source control's job.
    //
    // It lives HERE, beside the enum, because more than one layer asks the
    // question and a second copy would eventually answer differently. There is
    // NO default label on purpose: a new AssetType then has to be classified
    // deliberately, and forgetting shows up as a warning on this switch instead
    // of as an asset that silently refuses to replicate. Not hypothetical —
    // StructType, EnumType and SaveGameTemplate were added after the editor's
    // copy of this switch was written, fell through to false, and never
    // replicated at all, warning and everything.
    inline constexpr bool isCollabSyncableAssetType(AssetType t)
    {
        switch (t)
        {
            case AssetType::Material:
            case AssetType::MaterialFunction:
            case AssetType::Widget:
            case AssetType::HorizonCodeClass:
            case AssetType::ParticleSystem:
            case AssetType::AnimatorStateMachine:
            case AssetType::InputAction:
            case AssetType::InputMappingContext:
            case AssetType::Script:
            case AssetType::Scene:
            case AssetType::Prefab:
            case AssetType::StructType:
            case AssetType::EnumType:
            case AssetType::SaveGameTemplate:
            // Authored, small, and the one asset a whole team looks at while it
            // is being changed — a theme edit is exactly what two people want to
            // see land live.
            case AssetType::Theme:
            // A list of joint names and weights: a few hundred bytes, authored in
            // the editor, and the sort of thing an animator and a rigger change
            // while looking at the same character.
            case AssetType::BoneMask:
            // A handful of clip references with coordinates: a few hundred bytes,
            // authored in the editor by dragging points around, and exactly the
            // sort of thing two people tune while watching the same character run.
            case AssetType::BlendSpace:
                return true;

            case AssetType::StaticMesh:
            case AssetType::SkeletalMesh:
            case AssetType::Texture:
            case AssetType::Audio:
            case AssetType::Font:
            case AssetType::Shader:
            case AssetType::AnimationClip:
            case AssetType::PropertyAnimClip:
            case AssetType::Unknown:
                return false;
        }
        // Only reachable through a cast from an out-of-range value. Refusing is
        // the safe answer: an unrecognised kind does not go on the wire.
        return false;
    }

    // The enumerator's own spelling, for the boundaries that cannot carry the
    // enum: the scripting API's content.typeName and the C-ABI table a native
    // C++ game module reads it through (HorizonGameServices.h). The names are
    // the enumerator names deliberately — they are what the editor, the docs and
    // the asset headers already call these things, so a script comparing against
    // "StaticMesh" is comparing against the one spelling in the project.
    //
    // Here beside the enum for isCollabSyncableAssetType's reason, and with NO
    // default label for the same one: a new AssetType has to be named
    // deliberately, and forgetting shows up as a warning on this switch instead
    // of as an asset that reports itself as unknown.
    inline constexpr const char* assetTypeName(AssetType t)
    {
        switch (t)
        {
            case AssetType::Unknown:              return "";
            case AssetType::StaticMesh:           return "StaticMesh";
            case AssetType::SkeletalMesh:         return "SkeletalMesh";
            case AssetType::Texture:              return "Texture";
            case AssetType::Material:             return "Material";
            case AssetType::Scene:                return "Scene";
            case AssetType::Script:               return "Script";
            case AssetType::Audio:                return "Audio";
            case AssetType::Font:                 return "Font";
            case AssetType::Shader:               return "Shader";
            case AssetType::Prefab:               return "Prefab";
            case AssetType::AnimationClip:        return "AnimationClip";
            case AssetType::PropertyAnimClip:     return "PropertyAnimClip";
            case AssetType::MaterialFunction:     return "MaterialFunction";
            case AssetType::Widget:               return "Widget";
            case AssetType::HorizonCodeClass:     return "HorizonCodeClass";
            case AssetType::InputAction:          return "InputAction";
            case AssetType::InputMappingContext:  return "InputMappingContext";
            case AssetType::ParticleSystem:       return "ParticleSystem";
            case AssetType::AnimatorStateMachine: return "AnimatorStateMachine";
            case AssetType::StructType:           return "StructType";
            case AssetType::EnumType:             return "EnumType";
            case AssetType::SaveGameTemplate:     return "SaveGameTemplate";
            case AssetType::Theme:                return "Theme";
            // The two that were added after this switch was written and fell
            // through to "" — the exact failure the comment above predicts, with
            // the warning and everything. A BoneMask reported itself as unknown
            // to content.typeName, to the C ABI and to anything listing assets by
            // type name.
            case AssetType::BoneMask:             return "BoneMask";
            case AssetType::BlendSpace:           return "BlendSpace";
        }
        // Only reachable through a cast from an out-of-range value — the same
        // "unknown" the enum's own first entry means.
        return "";
    }

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

    // The raw uint8 is what lands in .hescene (and in the CBOR mirror of it), so
    // these values are APPEND-ONLY: renumbering one would silently reinterpret
    // every collider in every saved scene. Add at the end, never in the middle.
    enum class ColliderShape : uint8_t
    {
        Box     = 0,
        Sphere  = 1,
        Capsule = 2,

        // The three below take their geometry from the entity itself rather than
        // from the collider's own numbers, which is why an imported mesh no
        // longer has to pretend it is a crate.
        Mesh        = 3,  // triangle soup of the entity's mesh (LOD0). STATIC bodies
                          // only — Jolt's MeshShape reports MustBeStatic().
        ConvexHull  = 4,  // convex hull of that same mesh; fine on a dynamic body.
                          // Jolt caps a hull at 256 points and FAILS rather than
                          // simplifying, so a dense mesh may build no shape at all.
        HeightField = 5,  // the entity's TerrainComponent height field. STATIC only,
                          // and meaningless on an entity without a terrain.
    };

    // What kind of joint holds two rigid bodies together. Like ColliderShape
    // above, the raw uint8 lands in .hescene, so these values are APPEND-ONLY.
    //
    // Which of JointComponent's fields a type actually reads differs per type —
    // the table lives on the component, next to the fields it is about.
    //
    // The two Jolt types deliberately absent are SwingTwist and SixDOF. Those
    // are the ragdoll pair: they only pay off with JPH::Ragdoll and a skeleton
    // mapping behind them, which is a topic of its own beside the skeletal
    // animation work rather than a sixth line here.
    enum class JointType : uint8_t
    {
        Fixed    = 0,  // weld: no relative movement at all
        Point    = 1,  // ball socket: rotates freely about one shared point
        Hinge    = 2,  // door, lid, wheel — one axis, optionally limited
        Slider   = 3,  // drawer, lift, piston — one direction, optionally limited
        Distance = 4,  // rope, grapple, spring suspension — two points kept apart
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
