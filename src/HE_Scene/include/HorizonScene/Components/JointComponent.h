#pragma once
#include <Math/Math.h>
#include <Types/Enums.h>
#include <Types/UUID.h>

using JointType = HE::JointType;

// ─── A joint between this entity's rigid body and another one ────────────────
// A door on its frame, a chain of links, a rope between a grapple and a wall.
// PhysicsWorld turns it into a Jolt TwoBodyConstraint at scene start
// (initialize) or at spawn time (addEntity/addJoint), and destroys it again the
// moment either body goes away.
//
// ONE COMPONENT, NOT FIVE. A HingeJointComponent beside a SliderJointComponent
// would separate the fields cleanly and cost five serializers, five Details
// panels, five sets of tooltips, and would turn changing the type in the editor
// into "delete it and author a new one". ColliderComponent is the precedent in
// this tree and this is the same table it carries:
//
//   Fixed      nothing — the two bodies are welded in the relative pose they
//              are already in, so a broken-up object needs no anchors authored
//   Point      anchorA — ONE shared pivot; both bodies are attached to that
//              world point and rotate freely about it
//   Hinge      anchorA (the same shared pivot) + axis + minLimit/maxLimit
//   Slider     axis + minLimit/maxLimit; the anchors are the current relative
//              pose, so a drawer starts where it was authored
//   Distance   anchorA AND anchorB — the one type that reads both, because a
//              rope really does run from a point on A to a point on B
//
// WHY NOT TWO ANCHORS EVERYWHERE: giving Point or Hinge two separate world
// points tells Jolt to make them the same point, so the first step snaps the
// two bodies together. One shared pivot is Jolt's own recipe (see the note in
// PointConstraint.h) and it is what an author means by "hinge it here".
//
// ONE JOINT PER ENTITY. A chain is a chain of ENTITIES, each naming the one
// before it. A vector here would need a list UI in the Details panel to be
// authorable at all; it can be added later without changing the file format
// (an array with one element).
struct JointComponent
{
    JointType type = JointType::Fixed;

    // The OTHER body, by EntityIdComponent UUID — the way RopeComponent names
    // its attachments and CameraRigComponent its target. A raw entt handle
    // survives neither a save nor a prefab instantiation nor a collaboration
    // session. Empty (or naming an entity with no rigid body) means no joint is
    // built, and PhysicsWorld logs rather than staying silent.
    HE::UUID target;

    // Attachment points in each entity's OWN LOCAL space, converted to world
    // when the joint is built. Local because a prefab carries its joint with it:
    // a world anchor authored at the origin would be wrong the moment the prefab
    // is dropped anywhere else.
    //
    // Read per the table above — anchorB is Distance's alone.
    glm::vec3 anchorA{ 0.0f };
    glm::vec3 anchorB{ 0.0f };

    // The hinge's axis of rotation / the slider's direction of travel, as a
    // direction in A's LOCAL space. Normalised when the joint is built; a
    // zero-length axis is refused with a log rather than handed to Jolt.
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };

    // How far the joint may travel: DEGREES for a hinge, METRES for a slider,
    // and unread by the other three types.
    //
    // Degrees, not radians, even though Jolt wants radians: every other authored
    // angle on this surface is in degrees (an entity's rotation, a shape cast's
    // rotation) and this is a pose, not a rate — the one radian value in the
    // physics API is the angular VELOCITY, and it says so where it lives.
    //
    // minLimit >= maxLimit means UNLIMITED, which is the default: a hinge that
    // spins all the way round, a slider with no stops. Jolt requires the range
    // to straddle zero (the pose the joint was built in is 0), so a range that
    // does not is widened to include it, with a warning.
    float minLimit = 0.0f;
    float maxLimit = 0.0f;
};
