#pragma once
#include <Types/Defines.h>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <string>

namespace HE {

// ─── Collision layers ─────────────────────────────────────────────────────────
// A layer is a NAMED COLLISION CHANNEL, project wide, numbered 0…kCount-1. Two
// bodies collide when the matrix says their pair may — the Unity model, and the
// one people expect: "the player's pickup trigger should only see pickups",
// "bullets pass through enemy ragdolls".
//
// WHY THIS LIVES IN HE_Core AND NOT NEXT TO PhysicsWorld: it is a PROJECT
// setting, so it travels ProjectData (.heproj, HE_Tools) → ExportSettings →
// ProjectConfig (project.hcfg) → runtime, exactly like `allowFiles`. HorizonScene
// does not link HE_Tools, so PhysicsWorld may never see ProjectData; HE_Core is
// the one place both ends of that road can look at.
//
// The default construction is EXACTLY TODAY'S BEHAVIOUR: every cell true, so
// every existing project keeps colliding the way it always did. Anything else
// would break every scene on the first open. Static↔Static stays off, but that
// is a property of the simulation (a body that cannot move cannot be pushed),
// not of this matrix — see PhysicsWorld's pair filter.
struct HE_API CollisionLayerConfig
{
    // 16, not 32. Two reasons, and both are load-bearing:
    //  • the project UI draws the matrix as a triangle — 16·17/2 = 136 boxes is
    //    a page, 32·33/2 = 528 is not something anyone operates;
    //  • the Jolt ObjectLayer stays 16 bit (see PhysicsWorld: the layer index is
    //    packed with a moving/non-moving bit), and a 32-layer index would force
    //    JPH_OBJECT_LAYER_BITS=32, a global define that has to be identical in
    //    every translation unit that includes Jolt or it is a silent ODR
    //    violation.
    static constexpr int kCount = 16;

    // The five presets. They are RENAMEABLE but their INDEX is what a scene file
    // stores, so an index never changes meaning underneath a saved scene — a
    // rename is a label change, not a remap.
    enum : uint8_t {
        kDefault   = 0,   // every body that never chose, and every static prop
        kPlayer    = 1,   // nothing lands here on its own; the name is the point
        kTrigger   = 2,   // for volumes that only report — see the note below
        kCharacter = 3,   // default of CharacterControllerComponent::collisionLayer
        kTerrain   = 4,   // the implicit landscape height field, fixed
    };

    // Empty means "no name authored" and reads back as "Layer <n>" from
    // layerName(). Storing the presets as real strings instead would make a
    // renamed preset indistinguishable from an untouched one on load.
    std::string names[kCount];

    // matrix[a][b] — may layer a and layer b touch? Kept SYMMETRIC by every
    // mutator here, because Jolt does not promise which order it asks in: it may
    // call ShouldCollide(a, b) or ShouldCollide(b, a) depending on which body
    // the broadphase reached first, and a half-filled matrix would then let a
    // pair collide on some frames and not on others.
    bool matrix[kCount][kCount];

    CollisionLayerConfig();

    // The display name of a layer: the authored one, or "Layer <n>" when none
    // was given. Out-of-range indices answer "Layer <n>" too rather than
    // throwing — the callers are UI and log lines.
    std::string layerName(int index) const;
    void        setLayerName(int index, const std::string& name);

    // Reads and writes BOTH cells. Out-of-range is a silent no-op / false: a
    // config loaded from a file written by a newer engine with more layers must
    // not take the process down.
    bool collides(int a, int b) const;
    void setCollides(int a, int b, bool value);

    // True when nothing was ever changed — all names empty, every cell set. The
    // exporter uses it to leave the block out of a .heproj/.hcfg entirely, so a
    // project that never touched layers does not grow a 136-entry blob.
    bool isDefault() const;

    // The same JSON shape on both sides of the road (.heproj and .hcfg), so the
    // two can never drift. Only non-empty names and only the FALSE cells are
    // written — a fresh config serialises to an (almost) empty object, and a
    // block written by an older engine loads as "everything else collides".
    void toJson(nlohmann::json& out) const;
    void fromJson(const nlohmann::json& in);
};

} // namespace HE
