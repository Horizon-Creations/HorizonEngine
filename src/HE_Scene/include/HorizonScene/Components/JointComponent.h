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
// The MOTOR (motorTarget/motorMaxForce) is Hinge's and Slider's alone: those are
// the two types with a single degree of freedom to drive along. The other three
// have nothing to turn or push, and a motor authored on one of them is refused
// with a log rather than quietly doing nothing.
//
// collideConnected and breakForce read the same for all five.
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

    // ── Motor: Hinge and Slider only ─────────────────────────────────────────
    // What the joint drives itself towards, and how hard it is allowed to push.
    //
    // motorMaxForce IS THE SWITCH, not motorTarget. A motor with no force behind
    // it is off — which is the default, so nothing an older scene carries starts
    // moving — and a target of zero with force behind it is a BRAKE that holds
    // the door shut. Reading the target as the switch would have made that
    // perfectly ordinary setup unreachable.
    //
    // motorTarget is a RATE: RADIANS per second for a hinge, metres per second
    // for a slider. Radians, unlike the limits above, for the reason the physics
    // API gives at setAngularVelocity — a rate is not a pose, and every rate on
    // this surface is in radians. Newtons for a slider, newton-metres for a
    // hinge; both are a symmetric limit, so a motor can pull as hard as it
    // pushes.
    float motorTarget   = 0.0f;
    float motorMaxForce = 0.0f;

    // How much force the joint carries before it lets go, in newtons. 0 means it
    // never breaks, which is the default.
    //
    // Breaking DESTROYS the joint and this component with it: a broken door is
    // off its hinges, and leaving the component behind would rebuild the joint
    // the next time the entity's body was rebuilt — or the next time the scene
    // was loaded. PhysicsWorld::pollJointBroken reports the pair it was, once.
    float breakForce = 0.0f;

    // May the two jointed bodies touch each other?
    //
    // FALSE BY DEFAULT, which is what a chain wants: consecutive links overlap
    // by construction, and letting them collide makes them fight the joint that
    // holds them. Switch it on for the cases where the shapes genuinely stay
    // apart and the contact matters — a door that must not swing through its
    // own frame.
    //
    // This is not a Jolt flag. The pair is filtered in the contact listener, so
    // it costs nothing per body and applies the moment the joint exists.
    bool collideConnected = false;
};
