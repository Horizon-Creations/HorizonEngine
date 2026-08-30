#include "../EditorGuides.h"

// ── The world and what happens in it ─────────────────────────────────────────
// Baking navigation, making things appear while the game runs, reacting when
// the player walks somewhere, and the collision shapes everything above rests
// on.
//
// The NavMesh page stands on its own even though the chasing-NPC guide also
// bakes one: baking is the step people come back to after moving a wall, and
// looking it up should not mean reading a page about enemies.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> worldPages()
{
	std::vector<Docs::Page> out;

	// ── Baking a NavMesh ─────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A walkable surface over your level that agents can find paths "
			     "across."),
			para("One NavMesh covers the whole scene. It does not belong to the floor, "
			     "so it can live on an entity of its own — an empty one is fine."),
		}));

		secs.push_back(section("bake", "Step 1", "Bake it", {
			steps({
				{ "Create an entity and Add Component -> Nav Mesh",
				  "Six settings appear in the Details panel." },
				{ "Set Walk Height and Walk Radius to your character",
				  "Walk Height is the headroom an agent needs. Walk Radius pulls the "
				  "walkable surface back from walls so an agent does not clip corners." },
				{ "Set Walk Climb",
				  "The tallest step an agent walks up without a ramp." },
				{ "Press Bake",
				  "It gathers the level's static geometry, then builds." },
				{ "Read the two lines above the button",
				  "\"Geometry: N verts N tris\" has to show more than zero, and "
				  "\"NavMesh:\" has to read \"baked\"." },
				{ "Save the scene",
				  "Not optional — see below." },
			}),
			tip("Show NavMesh is the only honest check", {
				para("The checkbox draws the walkable surface in the viewport. Numbers "
				     "tell you something was built; the overlay tells you whether it "
				     "covers the places you meant. It is editor-only and never appears "
				     "in the game."),
			}),
		}));

		secs.push_back(section("static", "Step 2", "What the bake can see", {
			lead("Visible static meshes and terrain. Nothing else."),
			warn("\"Geometry: 0 verts\" means it found nothing to bake", {
				para("Moving bodies, characters and triggers are excluded on purpose. "
				     "A floor authored as a Dynamic or Kinematic Rigid Body is "
				     "therefore invisible to the bake and produces an empty NavMesh, "
				     "with no error anywhere. Zero verts is the symptom, Body Type is "
				     "the cause."),
			}),
			warn("Baking without saving ships a game with no NavMesh", {
				para("The scene file stores the gathered GEOMETRY, not the finished "
				     "NavMesh — the engine bakes again when the scene loads. So a bake "
				     "only survives if you save afterwards. A packaged build made from "
				     "an unsaved scene has no NavMesh at all, and every agent in it "
				     "just stands there."),
			}),
			note("Nothing rebakes on its own", {
				para("A wall you moved, a floor you widened or a ramp you deleted is "
				     "not in the NavMesh until you press Bake again. And save again."),
			}),
		}));

		secs.push_back(section("tune", "Step 3", "When paths look wrong", {
			table({ "Symptom", "Usually" }, {
				{ "Agents cut corners into walls", "Walk Radius too small for the character" },
				{ "Narrow gaps are not walkable", "Walk Radius too large" },
				{ "Agents refuse a step they should climb", "Walk Climb below the step height" },
				{ "Ramps are not walkable", "Max Slope below the ramp's angle" },
				{ "Surface is blocky and misses ledges", "Cell Size too coarse — smaller is finer and slower" },
			}),
		}));

		out.push_back(page("guides-navmesh", "Baking a NavMesh",
		                   "A walkable surface over the level, and how to tell whether "
		                   "it really covers it.",
		                   std::move(secs)));
	}

	// ── Spawning ─────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("Projectiles, pickups and enemies that appear while the game runs, "
			     "already carrying everything they need."),
		}));

		secs.push_back(section("class", "Step 1", "Furnish the thing first", {
			lead("What you spawn is a CLASS you prepared in the editor, not an entity "
			     "you assemble at runtime."),
			steps({
				{ "Content Browser -> HorizonCode Class",
				  "This is the projectile, the pickup, the enemy." },
				{ "Give it its components",
				  "Mesh, Collider, Rigid Body, whatever it needs. The class remembers "
				  "them." },
				{ "Give it its logic",
				  "Its own graph — a projectile that damages what it hits, a pickup "
				  "that removes itself when taken." },
			}),
			para("A spawn then arrives complete: mesh, collider, physics body and "
			     "running logic, all before its Begin Play executes. It can be pushed "
			     "in the same breath it was created in."),
		}));

		secs.push_back(section("spawn", "Step 2", "Spawn it", {
			table({ "Node", "When" }, {
				{ "Create Object", "In a HorizonCode graph. Its title shows which class it makes." },
				{ "Spawn Class", "The same from any language, taking a position." },
				{ "Spawn Class Rotated", "When the thing needs to point somewhere — a projectile does." },
			}),
			para("A projectile is usually Spawn Class Rotated at the muzzle, followed "
			     "by Add Impulse along its forward direction. Because the body exists "
			     "before Begin Play, the impulse lands on the first frame rather than "
			     "the second."),
			warn("Spawn Entity is not the same node", {
				para("Spawn Entity makes a BARE entity: a name and a transform, no "
				     "mesh, no collider, no logic. It is for an empty marker. If you "
				     "spawned something and nothing appeared, check which of the two "
				     "you used."),
			}),
		}));

		secs.push_back(section("cleanup", "Step 3", "Getting rid of it again", {
			lead("Nothing cleans up after a spawn."),
			para("Destroy Entity removes it and gives its physics body back. A "
			     "projectile that neither hits anything nor destroys itself stays in "
			     "the world forever, and a few hundred of them cost frames."),
			tip("Give anything you spawn a way to die", {
				para("A lifetime counted down in the class's own Tick is enough, and it "
				     "is one node. Do it when you create the class, not after the "
				     "profiler tells you to."),
			}),
		}));

		out.push_back(page("guides-spawning", "Spawning things at runtime",
		                   "Projectiles and pickups that arrive complete, and get "
		                   "cleaned up again.",
		                   std::move(secs)));
	}

	// ── Triggers ─────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A region that tells you when something walks into it — a pickup, a "
			     "door, a checkpoint, the end of the level."),
		}));

		secs.push_back(section("build", "Step 1", "Build the volume", {
			steps({
				{ "Create an entity where the region should be",
				  "It needs no mesh; it is a shape, not something to look at." },
				{ "Add Component -> Collider",
				  "Size it to the region. Tick Is Trigger." },
				{ "Add Component -> Rigid Body",
				  "Body Type Static. Yes, a trigger needs one too." },
				{ "Add Component -> Script",
				  "The class that reacts. Its Class field takes a HorizonCode class or "
				  "a .lua/.py asset." },
			}),
			warn("A trigger without a Rigid Body is silent", {
				para("The collider alone produces nothing. No error, no warning, no "
				     "events — you walk through the region and the graph never runs. "
				     "This is the single most common reason a trigger 'does not work'."),
			}),
		}));

		secs.push_back(section("react", "Step 2", "React to it", {
			lead("The class on the trigger gets an event when something enters and "
			     "another when it leaves."),
			para("The event carries the other entity, which is what you branch on: a "
			     "door that only opens for the player compares it against Get Player "
			     "Character; a damage volume simply hurts whatever walked in."),
			para("A trigger passes things through. A collision event on a SOLID object "
			     "is the other half — that is what a projectile uses to know it hit "
			     "something."),
			note("There are no tags or teams yet", {
				para("There is no built-in way to ask what KIND of thing entered. "
				     "Today you compare against a known entity, or put a variable on "
				     "the class and read it back through a cast. Worth knowing before "
				     "you design a system around 'all enemies'."),
			}),
		}));

		out.push_back(page("guides-triggers", "Triggers and pickups",
		                   "A region that reacts when something walks into it.",
		                   std::move(secs)));
	}

	// ── Physics props ────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("Crates that fall, get pushed and can be shot at."),
		}));

		secs.push_back(section("body", "Step 1", "Pick the body type", {
			table({ "Body Type", "Means" }, {
				{ "Static", "Never moves. Level geometry, floors, walls — and the only kind the NavMesh bake can see." },
				{ "Dynamic", "Falls, collides, can be pushed. A crate." },
				{ "Kinematic", "Moves only when something moves it, and shoves dynamic bodies aside. Lifts, doors, the collider on a character." },
			}),
		}));

		secs.push_back(section("shape", "Step 2", "Pick the collider shape", {
			lead("The shape is what the physics sees, and it does not have to be the "
			     "mesh."),
			table({ "Shape", "Use for" }, {
				{ "Box", "Crates, walls, most level geometry" },
				{ "Sphere", "Balls, pickups, blast volumes" },
				{ "Capsule", "Characters" },
				{ "Convex Hull", "An irregular prop that still needs to move" },
				{ "Mesh", "Detailed static geometry — the exact triangles" },
				{ "Height Field", "Terrain. The landscape uses it by itself" },
			}),
			warn("A Mesh collider cannot move", {
				para("It is a triangle soup, and physics cannot solve one against "
				     "another. Put it on a Static body. On a Dynamic body the engine "
				     "falls back to a hull rather than leaving it bodiless, but the "
				     "shape you get is not the shape you drew."),
			}),
			tip("Cheap shapes first", {
				para("A Box against a Box is far cheaper than a Mesh against anything. "
				     "Reach for Mesh when the silhouette genuinely matters, not by "
				     "default."),
			}),
		}));

		secs.push_back(section("push", "Step 3", "Push it from a graph", {
			table({ "Node", "When" }, {
				{ "Add Force", "A continuous push — wind, a conveyor. Applied every frame it is called." },
				{ "Add Impulse", "A single kick — an explosion, a projectile hit, a jump pad." },
				{ "Add Torque", "Spin it." },
				{ "Set Velocity", "Set the speed outright, ignoring what it was doing." },
				{ "Set Position (Physics)", "Teleport. Set Position And Stop (Physics) also clears the speed, which is what a respawn wants." },
			}),
			para("Overlap Sphere is how an explosion finds what to push: everything "
			     "within a radius in one call, instead of a fan of rays that misses "
			     "whatever sits between them."),
			warn("Moving a physics object by its transform does nothing", {
				para("Set Position writes the transform, and the physics step writes it "
				     "straight back from the body on the same frame. Anything with a "
				     "body has to be moved with the physics nodes above. This is quiet: "
				     "the object simply does not go where you put it."),
			}),
		}));

		out.push_back(page("guides-physics-props", "Physics props",
		                   "Crates that fall and can be pushed, with the right shape "
		                   "and the right way to move them.",
		                   std::move(secs)));
	}

	return out;
}

} // namespace HE::Ed::Guides
