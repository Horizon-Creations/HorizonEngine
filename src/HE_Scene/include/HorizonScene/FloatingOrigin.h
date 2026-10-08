#pragma once
#include <glm/vec3.hpp>
#include <cstddef>

class HorizonWorld;
class PhysicsWorld;

namespace HE
{

// ── Floating origin (Thema 153) ──────────────────────────────────────────────
// Positions are float. 32 km from 0,0,0 a float step is about 2 mm and objects
// visibly shake by a pixel and more; at 250 km a walking step is rounded away
// (docs/world-streaming-baseline-2026-10-06.md, 3.5). A floating origin keeps
// the camera near 0,0,0 instead: once it is too far out, the whole world moves
// back by a whole step and HorizonWorld::origin remembers, in double, where the
// local 0,0,0 now sits. The absolute position is local + origin.
//
// What moves along (shiftWorldOrigin): the root's children (everything under
// them follows through the hierarchy), the derived world matrices, Jolt bodies
// and characters, CPU particles, trails, precipitation and its ground grid, the
// rig camera's lagged pivot, IK look-at targets, nav agent targets and paths.
// The navmesh is baked in absolute coordinates and queried with the origin added
// (NavigationSystem). Savegames and replication write absolute positions.
//
// What does not, and is why this is a project switch that is off by default:
// numbers a script keeps for itself (a remembered waypoint), keyframes that set
// the position of a top-level entity (a cutscene), and the GPU particles'
// simulation state, which lives in the renderer. Each of those jumps by the
// shift when it happens.

// The shift that brings `cameraPos` back near 0,0,0 once it is more than
// `radius` metres out on any axis: per axis a whole multiple of `radius`, so the
// camera lands within half a radius of the origin and the shift itself is exact
// in float. Zero while the camera is inside, and when radius <= 0 (switched off).
glm::vec3 floatingOriginShift(const glm::vec3& cameraPos, float radius);

// Moves the world by -shift and adds shift to HorizonWorld::origin, so nothing
// changes where it is in absolute terms. `physics` may be null. Must not run
// between two extracts of one frame (RenderExtractor::FrameScope): call it in
// the frame's update, before the systems tick. Returns how many root children
// were moved.
size_t shiftWorldOrigin(HorizonWorld& world, PhysicsWorld* physics, const glm::vec3& shift);

// The two above for one frame. Returns the shift that was applied (zero when none).
glm::vec3 updateFloatingOrigin(HorizonWorld& world, PhysicsWorld* physics,
                               const glm::vec3& cameraPos, float radius);

} // namespace HE
