#include "../EditorGuides.h"

// ── The character ────────────────────────────────────────────────────────────
// Everything between "it moves" and "it feels right". Jumping, the camera, and
// the honest state of animation.
//
// The animation page is deliberately short and says more about what is missing
// than about what is there. That is not laziness: an author who spends an hour
// looking for blend spaces that do not exist has been failed by the manual, not
// by the engine.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> characterPages()
{
	std::vector<Docs::Page> out;

	// ── Jumping ──────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A character that jumps when the player presses the button, and only "
			     "when jumping makes sense."),
		}));

		secs.push_back(section("height", "Step 1", "Set the height", {
			lead("Jump height is not a field. It falls out of two that are."),
			steps({
				{ "Select the character",
				  "Its Details panel has Character Controller." },
				{ "Set Jump Speed (m/s)",
				  "How fast it leaves the ground. This is the dial you turn." },
				{ "Look at Gravity (m/s\xc2\xb2) above it",
				  "The two together decide how high it goes." },
			}),
			para("The height reached is the jump speed squared, divided by twice the "
			     "gravity. The defaults — 5 m/s against 9.81 — carry about 1.27 m, "
			     "which clears a low crate. Doubling the jump speed does not double "
			     "the height, it quadruples it."),
			tip("Raise gravity rather than lowering jump speed", {
				para("A floaty jump is usually too little gravity, not too much speed. "
				     "Gravity is per character, so a heavy enemy and a light player can "
				     "disagree about it in the same scene."),
			}),
		}));

		secs.push_back(section("node", "Step 2", "Wire the node", {
			lead("One node, on whatever event your jump action fires."),
			table({ "Node", "What it does" }, {
				{ "Jump", "Uses the character's own Jump Speed" },
				{ "Jump With Speed", "Takes the speed as an input — a charged jump, a pad that launches you" },
			}),
			para("Both answer whether the jump actually happened. Branch on it if a "
			     "sound or an animation should only play when it did."),
			note("The Entity pin can stay empty", {
				para("It reads Self, and that is what it means: the character this graph "
				     "belongs to. Every node that acts on an entity works this way, so a "
				     "character doing something to itself wires nothing — Move, Look, Is "
				     "Grounded, Get Speed, Set Position. Wire an entity in only when you "
				     "mean a different one."),
			}),
			warn("A refused jump is not an error", {
				para("Jump returns false in mid-air and does nothing else. If you play "
				     "the sound without checking, the player hears a jump grunt every "
				     "time they mash the button against the sky."),
			}),
		}));

		secs.push_back(section("coyote", "Detail", "The grace period", {
			lead("A jump is still granted for 120 ms after walking off a ledge."),
			para("This is deliberate and it is why running off a platform and pressing "
			     "jump slightly late still works. Without it, players report the jump "
			     "as unreliable without being able to say why — they are pressing a "
			     "few frames after the ground has gone."),
			para("The grace is spent by jumping, so holding the button cannot turn it "
			     "into a second jump, and a teleport ends it: arriving somewhere new "
			     "is not the same as stepping off a ledge."),
		}));

		secs.push_back(section("debug", "Troubleshooting", "A jump that does not fire", {
			lead("Diagnose it at Is Grounded, not at the velocity."),
			para("Under the separator in Character Controller, three greyed rows show "
			     "what the physics is actually seeing: Is Grounded, Air Time (s) and "
			     "Velocity. Air Time is the clock the grace period runs on."),
			table({ "Symptom", "Cause" }, {
				{ "Is Grounded is false while standing still", "The ground is not solid — it needs a Rigid Body" },
				{ "Jump returns true, nothing rises", "Something writes the velocity back down every tick" },
				{ "Only the first jump works", "The character never lands — check Is Grounded after landing" },
			}),
		}));

		out.push_back(page("guides-jump", "Making the player jump",
		                   "A jump that fires when it should, at a height you chose.",
		                   std::move(secs)));
	}

	// ── The camera ───────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A camera that follows the character, turns with the mouse, and does "
			     "not end up inside a wall."),
			para("The Camera Rig is a component on a camera entity. A Player Character "
			     "already has one on a child entity, so most of the time this is "
			     "tuning rather than building."),
		}));

		secs.push_back(section("build", "Step 1", "Put a rig on a camera", {
			lead("Only needed if you are building a camera yourself."),
			steps({
				{ "Add Component -> Camera Rig",
				  "It brings a Camera along, because a rig with nothing to aim does "
				  "nothing." },
				{ "Set Target",
				  "The entity to follow." },
				{ "Set Mode",
				  "First person or third person." },
			}),
		}));

		secs.push_back(section("tune", "Step 2", "Tune it", {
			table({ "Field", "What it changes" }, {
				{ "Arm Length", "How far behind the target the camera sits" },
				{ "Pivot Offset", "Where on the target the arm starts — shoulder height, not the feet" },
				{ "Arm Offset", "Shifts the camera sideways for an over-the-shoulder view" },
				{ "Sensitivity", "Mouse movement to view rotation" },
				{ "Stick Sensitivity", "The same for a gamepad, which turns at a RATE rather than by an amount" },
				{ "Pitch Min / Pitch Max", "How far the view tilts before it stops" },
				{ "Camera Radius", "How thick the camera is when it tests for walls" },
				{ "Collide With World", "Pulls the camera in instead of letting a wall come between it and the target" },
				{ "Hide Target Mesh", "Hides the body in first person" },
				{ "Target Rotation", "Whether the character turns to face where the camera looks" },
			}),
			tip("Turn on Hide Target Mesh for first person", {
				para("Otherwise the camera sits inside the character's own head and the "
				     "view is filled with the inside of a mesh."),
			}),
		}));

		secs.push_back(section("relative", "Step 3", "Walking where the camera looks", {
			lead("The setting that makes the controls feel right, and it is not on "
			     "the camera — it is on the character."),
			para("Movement -> Move Direction Is, set to Camera. Then pushing forward "
			     "walks where the view points instead of along the world's Z axis. A "
			     "Player Character ships set to it; a Movement component you add by "
			     "hand starts in World, which is what a fixed or top-down camera "
			     "wants."),
			warn("Orient To Movement is a different question", {
				para("The two get mistaken for each other constantly, and only one of "
				     "them answers \"why does my character not walk where I am "
				     "looking\"."),
				bullets({
					"Move Direction Is decides where the character GOES.",
					"Orient To Movement decides which way it FACES while going there.",
				}),
				para("With camera-relative input and no Orient To Movement the "
				     "character strafes without ever turning, and slides sideways "
				     "through the world. They belong together."),
			}),
			note("Two owners of one yaw", {
				para("Leave Target Rotation on Free while Orient To Movement is on. "
				     "Follow makes the rig write the character's yaw itself, and two "
				     "things writing one number is a jitter nobody can locate "
				     "afterwards. Follow with Orient To Movement OFF is the other "
				     "valid pair — that is the shooter feel, where you strafe and "
				     "walk backwards while facing where you aim."),
			}),
		}));

		secs.push_back(section("script", "Step 4", "Driving it from a graph", {
			lead("The rig can be read and written while the game runs."),
			para("Look feeds it from the player's input, which is the ordinary case. "
			     "Beyond that: Set Camera Mode switches perspective mid-game, Set "
			     "Camera Distance pulls the arm in for a tight corridor, Set Camera "
			     "Target hands the view to something else for a cutscene, and Turn "
			     "Camera nudges the view without the player asking — a hit reaction, "
			     "or aiming at what just spoke."),
			para("Get Camera Yaw and Get Camera Pitch read where it is looking, which "
			     "is what an aim direction is built from."),
		}));

		out.push_back(page("guides-camera", "The follow camera",
		                   "First or third person, tuned, and kept out of walls.",
		                   std::move(secs)));
	}

	// ── Animation ────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A character that switches between idle, walk and run as it moves."),
			para("The Animator State Machine is an asset: states, transitions between "
			     "them, and parameters your graph writes. The graph does not play "
			     "animations directly — it sets a number, and the state machine "
			     "decides what that means."),
		}));

		secs.push_back(section("build", "Step 1", "Build the state machine", {
			steps({
				{ "Content Browser -> Animator State Machine",
				  "One per character is normal." },
				{ "Add a state per animation",
				  "Idle, Walk, Run." },
				{ "Connect them with transitions",
				  "Each transition carries one condition on a parameter." },
				{ "Put it on the character",
				  "The skeletal mesh side of the entity is what plays it." },
			}),
		}));

		secs.push_back(section("drive", "Step 2", "Drive it from movement", {
			lead("One node per frame is usually the whole animation logic."),
			para("Get Speed reads how fast the character is actually moving. Feed it "
			     "into Set Animator Param, and let the transitions compare it: below "
			     "one number it is idle, above another it runs. Get Forward Amount and "
			     "Get Right Amount do the same for direction, and Is Grounded is what "
			     "a jump or fall state keys off."),
			para("Get Animator State reads back which state is playing, for the cases "
			     "where gameplay needs to know — not attacking while already attacking."),
			tip("Drive from measured speed, not from input", {
				para("Get Speed is what the character DID, input is what was asked for. "
				     "They differ when it walks into a wall, and the version that "
				     "matches what the eye sees is the measured one."),
			}),
		}));

		secs.push_back(section("gaps", "Honestly", "What the animator does not do yet", {
			lead("Worth reading before you plan around it."),
			bullets({
				"No blend spaces. Speed and direction cannot be blended into one "
				"pose — you switch between clips instead, and a diagonal walk is the "
				"forward clip.",
				"No layers or masks. An upper body cannot aim while the legs walk; "
				"one state machine drives the whole skeleton.",
				"No root motion. The character is moved by its controller and the "
				"animation plays over it, so the feet slide unless the clip's speed "
				"and Max Speed are matched by hand.",
				"No animation events. A footstep sound or a hit window cannot be "
				"placed on the timeline; check the state or the time from a Tick "
				"instead.",
				"No additive animation, and no interruptible cross-fade.",
			}),
			para("None of these block a game. They shape how it looks, and knowing "
			     "them now is cheaper than discovering them while polishing."),
		}));

		out.push_back(page("guides-animation", "Animating a character",
		                   "Idle, walk and run driven by how fast the character is "
		                   "really moving — and what the animator cannot do yet.",
		                   std::move(secs)));
	}

	// ── Hit effects ──────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A puff of dust where the shot landed, which appears, plays and "
			     "disappears without anything counting it down."),
		}));

		secs.push_back(section("effect-class", "Step 1", "Build the effect once", {
			steps({
				{ "Content Browser -> right-click -> Gameplay -> Entity Class",
				  "The effect is a thing that can be spawned, so it is a class." },
				{ "Add Component -> Particle System",
				  "Drop a particle system asset into its Asset slot, or make one "
				  "with Content Browser -> Particle System." },
				{ "In the particle asset, turn Looping off",
				  "This is what makes it an effect rather than scenery. A non-looping "
				  "emitter produces Max Particles and then it is done — so Max "
				  "Particles is the size of the puff, and Emit Rate is how quickly "
				  "they leave." },
				{ "Tick Destroy When Finished",
				  "The entity goes away with the last particle. Without it every shot "
				  "leaves a spent emitter in the scene forever." },
			}),
		}));

		secs.push_back(section("fire", "Step 2", "Fire it where the hit was", {
			lead("One node, on whatever already knows where the impact happened."),
			para("Spawn Class takes the class and a position. A Raycast gives you "
			     "that position; so does a collision event. Nothing else is needed — "
			     "the effect plays itself and cleans itself up."),
			tip("It emits at the impact point on the FIRST frame", {
				para("Worth knowing because it used to be the classic bug in engines "
				     "that compose transforms late: the effect appears at the world "
				     "origin for one frame and then jumps. This one asks the "
				     "hierarchy where it is rather than reading a matrix that has not "
				     "been refreshed yet."),
			}),
		}));

		secs.push_back(section("existing", "The other way", "An emitter that is already there", {
			lead("For a torch, a chimney, a wound that smokes while a character "
			     "lives: the emitter sits on the entity and a graph switches it."),
			table({ "Node", "What it does" }, {
				{ "Burst Particles", "So many at once, ignoring the emit rate. A muzzle flash, a footstep, a shower of sparks" },
				{ "Play Effect", "Starts it, or starts a spent one-shot over. Firing the same effect twice really does fire it twice" },
				{ "Stop Effect", "Emits no more; what is already in the air fades out normally" },
				{ "Is Effect Playing", "Still emitting, or still has particles alive. How you wait for an effect instead of guessing a duration" },
			}),
			note("Stop is not the same as hiding", {
				para("Hiding the entity leaves the emitter simulating unseen, and "
				     "clearing Playing in the inspector freezes the cloud where it "
				     "is. Stop Effect is the one that looks right."),
			}),
			warn("Burst can make fewer than you asked for", {
				para("Max Particles is a real cap. Burst answers how many it actually "
				     "made, so a shower that comes out thin is telling you the cap is "
				     "the reason rather than leaving you to guess."),
			}),
		}));

		secs.push_back(section("limits", "Honestly", "What to watch", {
			bullets({
				"Particles draw on Metal and OpenGL. The Direct3D and Vulkan "
				"backends have no particle path at all yet, so an effect that looks "
				"right on this machine may be missing in a Windows build that "
				"selected D3D.",
				"Looping and Max Particles live on the particle ASSET, not on the "
				"entity. Two effects that differ only in burst size are two assets.",
				"A looping emitter never finishes, so Destroy When Finished never "
				"fires on one unless something calls Stop Effect first.",
			}),
		}));

		out.push_back(page("guides-effects", "Hit effects and muzzle flashes",
		                   "A puff of dust that spawns at the impact, plays, and "
		                   "removes itself.",
		                   std::move(secs)));
	}

	return out;
}

} // namespace HE::Ed::Guides
