#pragma once
#include <glm/vec3.hpp>
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>

class HorizonWorld;
class PhysicsWorld;

namespace HE
{

class CellStreamer;

// ── The physics half of a cell host (Thema 164, step 3a) ─────────────────────
// What an application does with the CellStreamer's hooks to keep the physics world
// in step with the cells, in one place, so that the game and the tests run the same
// calls and a test cannot pass over a wiring the game does not have. (The cell host
// of step 2c, CellRuntime, will own these calls; until then the application's hook
// lambdas are the only callers.)
//
// What it comes to, from the point of view of a cell:
//   - its bodies go into the physics world a slice at a time, each slice as ONE
//     charge (PhysicsWorld::addEntities);
//   - when it goes, its bodies are removed first, in a pass of their own, and the
//     joints that named them wait for its return (PhysicsWorld::removeEntityTree
//     with requeueJoints);
//   - a cell is not started when the physics world has no room for its bodies
//     (cellBodiesFit as CellStreamer::Hooks::bodiesFit);
//   - while the cell under it is not there, whatever could fall is held, and let go
//     when it is (cellHolds as PhysicsWorld::setRegionHold's test).

// A slice of a cell was built: bodies for the entities it brought, as one charge.
// Returns how many entities got physics.
int cellSliceBodies(PhysicsWorld& physics, HorizonWorld& world, const std::vector<entt::entity>& created);

// A cell is about to be destroyed (its root still stands): its bodies leave the
// physics world, the whole subtree, before any entity is destroyed. Destroying an
// entity takes its subtree with it, so a walk that came after would find the
// children gone. Joints that named a removed body wait for it to be added again.
// Returns how many entities had physics.
int cellUnloadBodies(PhysicsWorld& physics, HorizonWorld& world, entt::entity root);

// Whether `bodies` more bodies keep the physics world within its reserve: nine
// tenths of Jolt's body table. The last tenth stays free for what is spawned at
// run time and for the bodies the base itself grows; the table's size is a hard
// limit and the reserve is what keeps a world from running into it unseen.
bool cellBodiesFit(const PhysicsWorld& physics, uint32_t bodies);

// The test PhysicsWorld::setRegionHold wants for a world that streams cells: is
// `worldPosition` (relative to the world's floating origin, which is how Jolt has
// it) in a square of the manifest whose cell is not built? See CellStreamer::holdsAt.
bool cellHolds(const CellStreamer& streamer, const HorizonWorld& world, const glm::vec3& worldPosition);

} // namespace HE
