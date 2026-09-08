#include "../EditorGuides.h"

// ── The character ────────────────────────────────────────────────────────────
// Everything between "it moves" and "it feels right". Jumping, the camera, and
// four pages of animation.
//
// The animation entry page stayed short and hands off: the state machine is the
// thing everyone builds first, and blend spaces, layers and IK are each a recipe
// of their own rather than three more steps on one page.
//
// This file used to say those three did not exist, and the list of what was
// missing was half the animation page. When a gap closes, the sentence that
// named it has to go with it — a manual that still warns about blend spaces
// while the editor has a Blend Space Editor costs the reader more than silence
// would have.

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

		secs.push_back(section("beyond", "Step 3", "Past one clip per state", {
			lead("Three things turn that machine into a character that looks alive. "
			     "Each is its own asset or component, and each has its own page."),
			rich({ run("A state can pose from a "), run("blend space", Docs::Style::Bold),
			       run(" instead of one clip: walk, jog and run mixed by the same "
			           "Speed parameter the transitions already read, so a character "
			           "at 2.7 m/s is not either walking or running but both. "),
			       link("Blended locomotion", "guides-blend-spaces"), run(".") }),
			rich({ run("An "), run("animation layer", Docs::Style::Bold),
			       run(" lays a second pose on top of whatever the machine came up "
			           "with, through a bone mask saying which joints it may touch — "
			           "a reload on the arms while the legs keep running. "),
			       link("An upper body of its own", "guides-animation-layers"),
			       run(".") }),
			rich({ run("And "), run("inverse kinematics", Docs::Style::Bold),
			       run(" bends the finished pose to the world it is standing in: feet "
			           "onto the step that is really there, head turned towards "
			           "whatever the character is watching. "),
			       link("Feet on the ground, eyes on the target",
			            "guides-animation-ik"),
			       run(".") }),
			note("They stack, and always in the same order", {
				para("The animator decides the pose, root motion is taken out of it "
				     "and turned into movement, the layer stack is applied on top of "
				     "what is left, and IK bends the result to the world. Nothing you "
				     "can configure changes that order, which is what makes the four "
				     "of them safe to add one at a time — and it is why an additive "
				     "layer that moves the root joint moves the pose rather than the "
				     "character."),
			}),
		}));

		secs.push_back(section("gaps", "Honestly", "What the animator does not do yet", {
			lead("Worth reading before you plan around it."),
			bullets({
				"A cross-fade cannot be interrupted. While a transition is running, "
				"no other transition is even considered — so a character that starts "
				"a half-second blend into Run and is hit in the middle of it finishes "
				"arriving at Run first.",
				"Additive layers work in LOCAL joint space only. That is right for a "
				"breathing wobble or a flinch, and subtly wrong for an aim offset "
				"over a spine that is already twisted: mesh-space additive, which is "
				"what fixes that, does not exist yet.",
				"A layer poses from a clip or a blend space, never from a state "
				"machine of its own. An upper body with its own idle/aim/reload "
				"logic is built as one layer whose clip gameplay switches, not as a "
				"second machine.",
				"IK has no script nodes. Gameplay can move the entity a head looks "
				"at, but weights, foot switches and targets are Details-panel fields "
				"only.",
				"Foot IK needs a running game. Outside Play there is no physics to "
				"ray-cast against, so the editor deliberately poses the feet the way "
				"the animation left them rather than against a guessed floor.",
			}),
			para("None of these block a game. They shape how it looks, and knowing "
			     "them now is cheaper than discovering them while polishing."),
		}));

		out.push_back(page("guides-animation", "Animating a character",
		                   "Idle, walk and run driven by how fast the character is "
		                   "really moving — and where to go next once that stands.",
		                   std::move(secs)));
	}

	// ── Blend spaces ─────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A character whose legs match its speed at every speed, instead of "
			     "snapping between three clips."),
			para("A blend space is an asset: animation clips placed as points in a "
			     "parameter space, and a mix of whichever points the parameters are "
			     "currently standing between. One axis is speed into walk / jog / "
			     "run; two axes are speed and direction into a whole strafe set."),
			para("It goes where a clip goes. A state of an Animator State Machine can "
			     "pose from one, and so can an animation layer — nothing else about "
			     "either changes."),
		}));

		secs.push_back(section("build", "Step 1", "Make the asset", {
			steps({
				{ "Content Browser -> right-click -> Gameplay -> Blend Space",
				  "Next to Animator State Machine and Bone Mask in the same menu." },
				{ "Double-click it",
				  "It opens in a tab of its own, the way a state machine does." },
				{ "Set Kind",
				  "1D reads one parameter, 2D reads two. 1D is the 2D case that "
				  "ignores Y, so switching later keeps every sample where it is." },
				{ "Type the parameter names into Parameter X (and Parameter Y)",
				  "These are animator parameter names — the same names the state "
				  "machine's transitions compare and a script writes with Set "
				  "Animator Param. Spelling is the whole contract." },
			}),
			warn("A parameter nothing writes reads 0", {
				para("There is no error for an axis pointed at a name that does not "
				     "exist. It reads 0, the sampler clamps 0 to the outermost "
				     "sample, and the character plays whichever clip happens to sit "
				     "closest to zero — usually the idle, convincingly enough that "
				     "the misspelling is the last thing anybody suspects."),
			}),
		}));

		secs.push_back(section("samples", "Step 2", "Place the clips", {
			lead("The diagram is the editor. Everything else on the page is a "
			     "number for what you just dragged."),
			steps({
				{ "Move the preview cursor where the sample belongs",
				  "Click empty space in the diagram, or right-drag, or type Cursor X "
				  "and Cursor Y." },
				{ "Press Add Sample",
				  "It lands where the cursor stands." },
				{ "Drop an Animation Clip into the sample's Clip slot",
				  "The sample is a point plus a clip; without one it weighs nothing." },
				{ "Drag the dot to tune it",
				  "X and Y follow the drag, and the numbers below follow the dot. "
				  "Remove takes a sample back out." },
			}),
			para("Range X Min / Max and the Y pair are the edges of the DIAGRAM, not "
			     "of the mix. The sampler clamps to the outermost sample and never "
			     "extrapolates, so widening the range shows more empty space and "
			     "changes no pose."),
			tip("The cursor is the preview, and it is honest", {
				para("Where the cursor stands, the weights are computed by the same "
				     "function the running game uses. What the diagram shows under "
				     "the cursor is what will play — this is the cheapest way to find "
				     "a hole in a 2D space before a character walks into it."),
			}),
		}));

		secs.push_back(section("use", "Step 3", "Point a state at it", {
			lead("A state node has two slots, and the blend space slot wins."),
			para("Open the Animator State Machine, and drop the blend space onto the "
			     "SECOND slot of a state node — the one under the clip slot. The node "
			     "then reads \"blend space — wins over clip\", which is exactly what "
			     "happens: a state carrying both poses from the space and ignores the "
			     "clip. Right-click that slot to take the blend space off again and "
			     "fall back to the clip."),
			para("Transitions, durations and conditions are untouched by this. A "
			     "state posing from a blend space is still one state, and the machine "
			     "still leaves it only through a transition."),
			note("A layer takes one the same way", {
				rich({ run("On the Animation Layers component, set "),
				       run("Source", Docs::Style::Code),
				       run(" to Blend Space and a Blend Space slot appears in place "
				           "of the Clip slot. A layer has no parameters of its own "
				           "and reads the entity's state-machine parameters, so the "
				           "layer and the state underneath it cannot disagree about "
				           "the speed. See "),
				       link("An upper body of its own",
				            "guides-animation-layers"),
				       run(".") }),
			}),
		}));

		secs.push_back(section("phase", "How it stays in step", "One cycle, not four clocks", {
			lead("Clips of different length are read at the same point in their OWN "
			     "cycle, which is what keeps a blend from crossing the feet."),
			para("Every sample of a blend space runs on one shared phase between 0 "
			     "and 1 rather than on its own seconds. At phase 0.5 a 1.2 second "
			     "walk is read at 0.6 s and a 0.8 second run at 0.4 s, so the left "
			     "foot of one is never mixed with the right foot of the other. This "
			     "is why the playhead of a state or a layer on a blend space is shown "
			     "as \"phase\" and not as seconds."),
			para("Speed Scale on a sample is the correction for a clip whose author "
			     "got the tempo wrong. It does not run that one sample faster on its "
			     "own — all of them share the phase — it shortens the seconds that "
			     "sample contributes to the shared cycle, which speeds the whole mix "
			     "up in proportion to its weight."),
			para("Looping belongs to the SPACE and not to each place it is used, for "
			     "the same reason: a locomotion set either cycles or it does not, and "
			     "its samples cannot disagree about that."),
		}));

		secs.push_back(section("limits", "Honestly", "What to watch", {
			bullets({
				"At most four samples are ever evaluated at once. That is generous "
				"for a set anybody can see; a fifth sample with real weight means "
				"the space is packed tighter than it needs to be.",
				"Nothing extrapolates. Outside the outermost sample the mix clamps "
				"to it, so a Speed that goes past your fastest sample keeps playing "
				"that sample rather than running faster.",
				"A 2D space has no triangulation and no grid — samples sit wherever "
				"you drop them. A region with no sample near it takes whatever the "
				"nearest ones give it, which is why the preview cursor is worth "
				"sweeping across the whole diagram once.",
				"Notifies fire from the sample carrying the most weight, not from "
				"all of them at once. Two footstep notifies in a walk/run blend "
				"produce one footstep, which is what you want and not what a naive "
				"sum would do.",
			}),
		}));

		out.push_back(page("guides-blend-spaces", "Blended locomotion",
		                   "Walk, jog and run mixed by speed instead of switched "
		                   "between — and a strafe set on two axes.",
		                   std::move(secs)));
	}

	// ── Animation layers and bone masks ──────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A character that reloads with its arms while its legs keep "
			     "running, and goes back to running when the reload ends."),
			para("An animation layer is a second pose laid on top of whatever posed "
			     "the character — a clip, a two-clip blend or a state machine, it "
			     "does not care which. A bone mask says which joints the layer is "
			     "allowed to touch. The two together are the upper body."),
			para("Layers live on their own component, so adding them changes nothing "
			     "about the animator that is already there, and an entity without "
			     "the component pays nothing."),
		}));

		secs.push_back(section("mask", "Step 1", "Say which joints belong to the layer", {
			steps({
				{ "Content Browser -> right-click -> Gameplay -> Bone Mask",
				  "One mask is normally reused by every character on the same rig." },
				{ "Double-click it, and drop a skeletal mesh into Reference Skeleton",
				  "The mesh is this editor's own reference and is NOT saved into the "
				  "mask: a mask holds joint NAMES, so it works on every rig that "
				  "spells them the same way." },
				{ "Tick the joints, or press Add Subtree on the spine",
				  "Add Subtree ticks that joint and everything under it at weight 1; "
				  "Clear Subtree is the way to say \"the upper body except the "
				  "fingers\"." },
				{ "Fade the boundary with Weight",
				  "Values below 1 down a couple of spine joints are what makes a "
				  "masked layer blend into the base pose instead of ending at a hard "
				  "edge across the waist." },
			}),
			warn("A mask is an allow-list, and an empty one means nothing", {
				para("A joint that is not ticked has weight 0. So a mask that matches "
				     "no joint on this skeleton does not mean \"everything\" — it "
				     "means the layer does nothing at all, silently. Names the "
				     "reference skeleton does not have are LISTED rather than hidden, "
				     "with a Drop button next to each, because a mask that quietly "
				     "stopped covering the joint it was written for looks exactly "
				     "like a layer that stopped working for no reason."),
			}),
			note("No mask is not the same as an empty mask", {
				para("Leaving the Mask slot empty on a layer means the whole "
				     "skeleton at weight 1. That is the right setting for a full-body "
				     "hit reaction and the wrong one for an upper body."),
			}),
		}));

		secs.push_back(section("layer", "Step 2", "Add the layer", {
			steps({
				{ "Select the character, Add Component -> Animation Layers",
				  "It sits under the animator it will lay poses over." },
				{ "Press Add Layer, and give it a Name",
				  "The name is what scripts address, not the position in the list — "
				  "reordering the stack cannot silently fade a different body part." },
				{ "Drop the animation into Clip, and the mask into Mask",
				  "Or set Source to Blend Space and drop one of those instead." },
				{ "Set Weight",
				  "How much of the layer reaches the pose before the mask is applied. "
				  "This is the number gameplay animates while a weapon comes up." },
			}),
			para("Layers are applied in list order, each onto the result of the one "
			     "before it, so the last one in the list has the last word wherever "
			     "its mask lets it. Every layer has its own Speed, Time, Looping and "
			     "Playing — a slow flinch over a fast run is one number, not a "
			     "compromise."),
			warn("Weight 0 and Playing off are different things", {
				para("Weight 0 blends nothing in and lets the playhead keep running, "
				     "which is why fading a layer back in does not restart it "
				     "mid-stride. Playing off freezes the playhead where it stands "
				     "and still blends it in, which is how a held pose is done. "
				     "Reaching for the wrong one of the two is the usual reason a "
				     "reload restarts from the beginning every time it is faded."),
			}),
			warn("A layer stack with no animator under it does nothing", {
				para("Layers are applied by whichever driver posed the entity that "
				     "frame. On an entity with a skeletal mesh and no Animator, "
				     "Animator Blend or Animator State Machine at all, nothing runs "
				     "the stack. The engine says so in the log once rather than "
				     "leaving you to work it out from a character that will not "
				     "move."),
			}),
		}));

		secs.push_back(section("additive", "Step 3", "Override or additive", {
			lead("Mode is the decision that separates a reload from a breath."),
			table({ "Mode", "What it does", "What it is for" }, {
				{ "Override", "Replaces the pose underneath, weighted, wherever the "
				              "mask allows", "A reload, a wave, a one-handed door "
				              "push — a movement that owns the arms while it runs" },
				{ "Additive", "Adds this clip's DIFFERENCE against a reference pose "
				              "on top of whatever is underneath", "A breathing "
				              "wobble, a limp, an aim offset — a modification that "
				              "has to keep the run running while it leans on it" },
			}),
			para("An additive layer needs to know what its clip is a difference "
			     "FROM. Left empty, Reference Clip means the layer's own clip at "
			     "Reference Time, which defaults to 0 — \"the difference from where "
			     "this animation starts\", the ordinary way an additive clip is "
			     "authored. A separate reference clip is for the case where the base "
			     "pose of an additive set lives in its own file."),
			warn("Additive is local-space only", {
				para("The difference is added joint by joint in each joint's own "
				     "frame. That is correct for a wobble or a flinch. It is subtly "
				     "wrong for an aim offset over a spine that is already twisted, "
				     "because the correction needed there depends on where the spine "
				     "ended up — mesh-space additive is what solves that, and it "
				     "does not exist yet."),
			}),
		}));

		secs.push_back(section("drive", "Step 4", "Switch it from gameplay", {
			lead("Four nodes, and they all address a layer by its name."),
			table({ "Node", "What it does" }, {
				{ "Set Layer Weight", "Fades a layer in or out. The one you animate "
				                      "over a few frames rather than setting to 1" },
				{ "Get Layer Weight", "Reads it back — how a graph waits for a fade "
				                      "to finish instead of counting frames" },
				{ "Play Layer", "Sets the playhead back to 0 and starts it. This is "
				                "how a one-shot layer is fired a second time, and it "
				                "re-arms a notify sitting on frame 0" },
				{ "Get Layer Names", "Every layer on the entity, in list order. For "
				                     "logic that does not want a name typed twice" },
			}),
			para("A one-shot upper-body action is a non-looping layer at weight 1: "
			     "Play Layer to fire it, and it clamps on its last frame when it "
			     "ends. Fading the weight back out is what returns the arms to the "
			     "base pose — the layer stopping does not do that by itself."),
		}));

		secs.push_back(section("limits", "Honestly", "What to watch", {
			bullets({
				"A layer poses from one clip or one blend space, never from a state "
				"machine of its own. An upper body with its own idle / aim / reload "
				"logic is one layer whose clip gameplay switches.",
				"Two layers may share a name. Nothing stops it, and the scripting "
				"API takes the first one — worth knowing before duplicating a layer "
				"and wondering why the copy never fades.",
				"The playhead's unit follows the source: seconds on a clip, a phase "
				"between 0 and 1 on a blend space. The Time field says which one it "
				"is showing, and the two are not comparable.",
				"Root motion is taken out of the pose BEFORE the layers are applied, "
				"which is deliberate: a layer must not write back the root "
				"translation that was just removed. An additive layer that moves the "
				"root joint therefore moves the pose and not the character.",
			}),
		}));

		out.push_back(page("guides-animation-layers", "An upper body of its own",
		                   "A reload on the arms while the legs keep running — "
		                   "layers, bone masks and additive poses.",
		                   std::move(secs)));
	}

	// ── Inverse kinematics ───────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("Feet that stand on the step they are actually on, and a head that "
			     "turns towards what the character is watching."),
			para("Inverse kinematics is a correction laid over the finished pose. It "
			     "runs after every animator, after the layers and after root motion, "
			     "so it holds whatever they came up with and bends it to the world. "
			     "Nothing above it has to know it is there."),
			para("Two independent halves live on one component: foot placement, "
			     "which needs the ground, and look-at, which needs a target."),
		}));

		secs.push_back(section("feet", "Step 1", "Put the feet on the ground", {
			steps({
				{ "Select the character, Add Component -> Inverse Kinematics",
				  "One component, however many legs." },
				{ "Press Add Foot, and type the ankle's name into Foot Joint",
				  "Exactly as the skeleton spells it. Everything else about the leg "
				  "is found from here." },
				{ "Leave Knee Joint and Hip Joint empty",
				  "Empty means \"the joint above the foot, and the one above that\", "
				  "which is right on every humanoid rig. Name them for a digitigrade "
				  "leg — a dog, a bird — where the two above the foot are not the "
				  "ones that should bend." },
				{ "Set Sole Offset",
				  "Ankle to sole, in metres. No skeleton knows this by itself: the "
				  "foot joint sits INSIDE the ankle, so putting the joint on the "
				  "ground buries the foot to that depth." },
				{ "Add the second foot",
				  "Same again with the other ankle." },
			}),
			para("Trace Up and Trace Down are how far above and below the current "
			     "foot the ground is looked for. The upward half is the one that "
			     "matters more than it sounds: it is what finds a step the animation "
			     "has already pushed the foot INTO."),
			warn("Foot IK does nothing outside Play", {
				para("The ground is found by a ray, and there is no physics world in "
				     "the editor's preview. So the feet are deliberately left exactly "
				     "where the animation put them rather than solved against a "
				     "guessed floor — a preview that poses differently from the game "
				     "is worse than no preview. Look-at is not affected and runs in "
				     "the editor too."),
			}),
			warn("And it does nothing where there is no collider", {
				para("A ray needs something to hit. A landscape or a floor mesh with "
				     "no collider on it is not ground as far as IK is concerned, and "
				     "the leg is left alone with no message anywhere. If the feet "
				     "ignore a surface, that surface is the first thing to check."),
			}),
			tip("Align To Normal is what makes a slope look walked on", {
				para("It tilts the foot to match the surface it landed on instead of "
				     "leaving it flat. Max Pitch and Max Roll are the limits: without "
				     "them a foot that finds a wall stands itself on its toes. Max "
				     "Roll is usually the smaller of the two, because a foot rolls "
				     "sideways far less than it pitches."),
			}),
		}));

		secs.push_back(section("pelvis", "Step 2", "Lower the body before the legs stretch", {
			lead("The setting that turns two corrected feet into a character "
			     "standing on a slope."),
			para("With Adjust Pelvis on, the whole body is lowered by the deepest "
			     "foot's drop before the legs are solved. Without it, the low leg has "
			     "to reach the whole way on its own and straightens out — the "
			     "unmistakable look of a character standing on a stair with one leg "
			     "locked."),
			para("Pelvis Joint left empty means the common ancestor of the feet, "
			     "which on a normal rig is the pelvis. Name it when the rig has "
			     "something unusual between the legs and the spine."),
			para("Foot Interp Speed is how fast a foot catches up with ground that "
			     "moved. Higher is more exact and more nervous; lower floats. The "
			     "smoothing exists because at a stair edge the ground under a foot "
			     "really does jump by a whole step in one frame, and a foot that "
			     "follows that exactly reads as a glitch. A character teleported "
			     "across the map is detected as a teleport and snaps instead of "
			     "easing, so this does not cost you a jump cut."),
		}));

		secs.push_back(section("look", "Step 3", "Point the head at something", {
			steps({
				{ "Tick Look At",
				  "The second half of the component, independent of the feet." },
				{ "Type the chain into Look Chain",
				  "Comma-separated, root first: spine, neck, head. One to three "
				  "joints is the usual answer." },
				{ "Pick a target in Look Target",
				  "A dropdown of the entities in the scene, followed as they move. "
				  "Leave it on \"(world point)\" and a Target Point field appears "
				  "instead — the fixed place in the world to watch. \"(world point)\" "
				  "is not \"no target\", it is the other kind of target." },
				{ "Check Head Forward",
				  "Which way the head looks in its own frame. -Z is this engine's "
				  "forward, and it is the default — but an imported rig can carry any "
				  "convention at all and nothing can guess it from the skeleton." },
			}),
			para("Chain Weights says how much of the turn each joint takes, root "
			     "first, and is normalised at use — so the numbers do not have to sum "
			     "to anything and an empty box splits the turn evenly. Spreading the "
			     "turn so the spine carries part of it is what makes a character look "
			     "AT something rather than swivel a head on a still body."),
			para("Max Yaw and Look Max Pitch are the limits, and they are what keeps "
			     "the effect from becoming an owl. A person gives up and turns their "
			     "shoulders long before they can look at their own feet, so the pitch "
			     "limit is normally the tighter of the two."),
			warn("The wrong Head Forward looks like a broken chain, not a wrong axis", {
				para("If the head points ninety degrees away from the target and "
				     "tracks it faithfully at that offset, the chain and the target "
				     "are fine and Head Forward is wrong. Try +Z, then +X, before "
				     "suspecting anything else."),
			}),
		}));

		secs.push_back(section("limits", "Honestly", "What to watch", {
			bullets({
				"No script nodes. Every field is on the component in the Details "
				"panel. Gameplay can move the entity a character looks at, which "
				"covers most of what a look-at is for, but it cannot fade a look "
				"weight or switch a foot off from a graph.",
				"A target further away than the leg is long straightens it rather "
				"than tearing the rig apart. That is the intended answer, and it is "
				"also why a Sole Offset that is much too large shows up as a stiff "
				"leg rather than as an error.",
				"Foot IK reaches two joints up from the ankle and no further. A "
				"correction that would need the spine is a look-at chain's job, not "
				"a leg's.",
				"It runs once per frame per entity, and the engine says so in the "
				"log if something manages to ask twice — the smoothing would "
				"otherwise integrate twice and the feet would arrive at the ground "
				"at double speed.",
			}),
		}));

		out.push_back(page("guides-animation-ik", "Feet on the ground, eyes on the target",
		                   "Legs that follow the ground they are standing on, and a "
		                   "head that turns towards what matters.",
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
