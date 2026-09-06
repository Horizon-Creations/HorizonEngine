#include "HcNodeDocs.h"

#include <HorizonScene/EngineApi.h>

#include <string>

namespace HE::Ed::NodeDocs
{
namespace
{
	struct Row { const char* id; const char* doc; };

	// ── The table ────────────────────────────────────────────────────────────
	// In registry order, so a category reads as a group and a gap is visible.
	// Prose without hard line breaks: both places this is shown wrap it
	// themselves (the tooltip at a fixed column, the manual at the reading
	// width), and a baked-in newline fights whichever width it lands in.
	constexpr Row kDocs[] = {

	// ── Debug ────────────────────────────────────────────────────────────────
	{ "log",
	  "Writes a line to the editor's Console panel and to HorizonEngine.log. The "
	  "cheapest way to find out whether an exec chain ran at all." },
	{ "debug.line",
	  "Draws a line in the world for Seconds (0 = this frame only). Editor and "
	  "development builds only; a packaged game draws nothing." },
	{ "debug.sphere",
	  "Draws a wireframe sphere in the world for Seconds. Useful for showing what "
	  "an overlap or sphere cast actually covered." },
	{ "debug.box",
	  "Draws a wireframe box between two world corners for Seconds." },
	{ "debug.clear",
	  "Removes every debug shape drawn so far, including ones whose time has not "
	  "run out." },

	// ── Entity ───────────────────────────────────────────────────────────────
	{ "entity.getName",
	  "The entity's name as the World Outliner shows it. Empty for an entity that "
	  "does not exist." },
	{ "entity.spawn",
	  "Creates an empty entity at run time and returns it. It has a transform and "
	  "nothing else — add what it needs with the other calls, or use Spawn Class, "
	  "which brings the mesh, the collider, the body and the logic an author gave "
	  "the class." },
	{ "entity.spawnClass",
	  "Creates an instance of a HorizonCode class at this position and returns its "
	  "entity. Unlike Spawn Entity it arrives furnished: the components the class "
	  "carries, its physics body and its graph are all in place before Construct "
	  "and Begin Play run, so its first frame already knows where it stands. "
	  "Rotation stays as the class authored it. Returns 0 and logs why if the "
	  "class is unknown or is not an Entity class." },
	{ "entity.spawnClassRotated",
	  "Spawn Class with the rotation stated as well, in Euler degrees. The two are "
	  "separate calls because \"leave the rotation the class authored\" and \"face "
	  "zero degrees\" are different requests, and a defaulted 0,0,0 could not tell "
	  "them apart." },
	{ "entity.destroyObject",
	  "Destroys a spawned object by its reference: the class instance, the entity "
	  "under it and that entity's physics bodies. The counterpart to Spawn Class — "
	  "reach the reference with Get Object On Entity. Destroy Entity is the other "
	  "half of the pair and takes an entity instead." },
	{ "entity.destroy",
	  "Removes the entity and everything parented under it. Any id still held "
	  "elsewhere becomes invalid; check with Exists before using one you kept." },
	{ "entity.self",
	  "The entity this graph is running on. 0 in a graph that has none — the Game "
	  "Instance, or a widget that was never attached to one." },
	{ "entity.selfObject",
	  "This graph's own instance as an object reference, for handing to Bind "
	  "Event, Call External or anything else that takes a target." },
	{ "entity.instance",
	  "The HorizonCode object running on this entity, if one is. An empty "
	  "reference when the entity carries no class." },
	{ "entity.owned",
	  "The entity an object reference sits on — the inverse of Instance." },
	{ "entity.distance",
	  "Straight-line distance between two entities in metres. Cheaper than "
	  "subtracting two positions and taking the length, and reads better. "
	  "Negative when there is no world to measure in, which no running game has." },
	{ "entity.findByName",
	  "Finds the first entity with this name in the active scene. A linear search "
	  "over every named entity: fine once in a while, wasteful every frame — "
	  "store the id in a variable instead." },
	{ "entity.exists",
	  "Is this entity id still valid? The guard to run before touching an entity "
	  "that something else may have destroyed." },
	{ "entity.setVisible",
	  "Shows or hides whatever the entity draws — mesh, skeletal mesh, light. "
	  "Physics and scripts are untouched; it stops being drawn, not being there." },
	{ "entity.getVisible",
	  "Whether the entity is currently drawn. True for an entity that has nothing "
	  "to draw at all." },
	{ "entity.saveState",
	  "Stores this entity's flagged attributes (see its Save State component) in "
	  "the active save, keyed by its UUID. Play mode only." },
	{ "entity.hasSavedState",
	  "Does the active save carry state for this entity?" },
	{ "entity.applySavedState",
	  "Applies every attribute the active save carries for this entity back onto "
	  "it. Partial: what the save lacks stays as it is. Play mode only." },

	// ── Transform ────────────────────────────────────────────────────────────
	{ "transform.getPosition",
	  "LOCAL position in metres: for a child entity this is its offset from the "
	  "parent, not where it ends up. Use Get World Position for that. Without a "
	  "parent the two are the same, which is why the difference only shows up on "
	  "the first attached object." },
	{ "transform.setPosition",
	  "Moves the entity to a LOCAL position — relative to its parent, if it has "
	  "one. It teleports: nothing sweeps for collisions on the way, so a physics "
	  "body can end up inside a wall. For a character use Move instead." },
	{ "transform.getWorldPosition",
	  "Where the entity actually stands, with every parent taken into account. "
	  "Composed on the spot from the parent chain, so it is true even for an "
	  "entity that moved or was spawned this frame." },
	{ "transform.setWorldPosition",
	  "Puts the entity at a world position whatever its parents are doing, by "
	  "converting through them. Teleports like Set Position." },
	{ "transform.getRotation",
	  "Rotation as euler angles in degrees (X pitch, Y yaw, Z roll)." },
	{ "transform.setRotation",
	  "Turns the entity to these euler angles in degrees. Y is the one that turns "
	  "a standing character to face elsewhere." },
	{ "transform.getScale",
	  "Scale per axis. 1 is the mesh at its authored size." },
	{ "transform.setScale",
	  "Resizes the entity. A physics collider follows the scale only "
	  "approximately — for anything that has to collide accurately, scale the "
	  "collider's own extents." },

	// ── Physics ──────────────────────────────────────────────────────────────
	{ "physics.raycast",
	  "Fires a ray from Origin along Direction and reports the first thing it "
	  "hits. Direction does not have to be normalised. Hit is false when nothing "
	  "is in the way, and the other outputs are then meaningless rather than "
	  "stale — always branch on Hit first." },
	{ "physics.sphereCast",
	  "Like Raycast, but sweeps a sphere of Radius along the ray, so it catches "
	  "what a thin ray slips past — a gap between two crates, the edge of a step. "
	  "The extra cost is worth it for anything a character has to stand on." },
	{ "physics.overlapSphere",
	  "Every entity whose collider overlaps this sphere, as an array. The "
	  "explosion-radius query; walk the result with a For Each." },
	{ "physics.setVelocity",
	  "Sets a body's velocity in metres per second outright, discarding whatever "
	  "it had. For a nudge use Add Impulse, which adds to the motion instead of "
	  "replacing it." },
	{ "physics.getVelocity",
	  "The body's current velocity in metres per second. Zero for an entity "
	  "without a physics body." },
	{ "physics.isGrounded",
	  "Is this character controller standing on something right now? False for an "
	  "entity that has no character controller." },
	{ "physics.addForce",
	  "Adds a continuous force (newtons) for this tick — call it every frame while "
	  "it should push. Returns false when the entity has no dynamic body, which is "
	  "the usual reason nothing moves." },
	{ "physics.addImpulse",
	  "Adds an instant change in momentum: a jump, a hit, an explosion. Call it "
	  "once, not every frame." },
	{ "physics.addTorque",
	  "Adds rotational force about the world axes, spinning the body." },
	{ "physics.setGravity",
	  "Sets gravity for the WHOLE physics world, not for one entity. The default "
	  "is (0, -9.81, 0)." },
	{ "physics.getGravity",
	  "The physics world's gravity vector." },
	{ "physics.setPosition",
	  "Teleports the entity's physics body and its transform to this position in "
	  "one step. Nothing sweeps on the way, and the velocity is kept — Set Position "
	  "And Stop is the respawn variant." },
	{ "physics.setPositionAndReset",
	  "Teleports like Set Position and zeroes the velocity on arrival. What a "
	  "respawn wants, so a player put back at a checkpoint does not keep the fall "
	  "that killed them." },
	{ "physics.hasPhysics",
	  "Whether the entity has a rigid body or a character controller at all. False "
	  "without a physics world, and the honest answer to why a force did nothing." },

	// ── Material ─────────────────────────────────────────────────────────────
	{ "material.getParam",
	  "Reads one named parameter of the entity's material as a colour. The name is "
	  "the one the material graph's Param node declares; all four components are "
	  "stored, whatever the shader reads." },
	{ "material.setParam",
	  "Overrides one named material parameter for THIS entity only — the material "
	  "asset and every other entity using it are untouched. Ok is false when the "
	  "entity has no material or the parameter is not declared." },

	// ── Animator ─────────────────────────────────────────────────────────────
	{ "animator.setParam",
	  "Writes a named parameter of the entity's animator state machine — the "
	  "values its transitions test. This is how gameplay drives animation: set "
	  "Speed, let the graph decide between walk and run." },
	{ "animator.getParam",
	  "Reads a named animator parameter back. 0 for an unknown name or an entity "
	  "without an animator." },
	{ "animator.getState",
	  "The name of the state playing right now, as the animator asset spells it. "
	  "Empty when there is no state machine." },

	// ── Particles ────────────────────────────────────────────────────────────
	{ "particle.burst",
	  "Emits a number of particles at once from an entity's Particle System, "
	  "ignoring its emit rate entirely. This is what a hit, a muzzle flash or a "
	  "footstep is: so many at a moment, which a rate cannot express — an emit "
	  "rate high enough to look instant depends on the frame rate. It answers how "
	  "many were actually made, which can be fewer than asked for when the "
	  "emitter's Max Particles is already reached." },
	{ "particle.play",
	  "Starts the emitter, or starts it again from the beginning. A one-shot that "
	  "has already run needs this to run a second time: it resets the burst it has "
	  "emitted, so firing the same effect twice fires it twice. Particles still in "
	  "the air are left alone, which is what lets a fast repeat overlap its own "
	  "tail." },
	{ "particle.stop",
	  "Stops the emitter producing anything more, while the particles already out "
	  "live out their lifetime and fade normally. Deliberately not the same as "
	  "hiding the entity, which leaves it simulating unseen, nor the same as a "
	  "pause, which would freeze a cloud of smoke in mid-air." },
	{ "particle.isPlaying",
	  "Whether the effect is still running — either still emitting, or with "
	  "particles still alive. It goes false the moment a one-shot has completely "
	  "finished, which is how a graph waits for an effect instead of guessing a "
	  "duration." },

	// ── Movement ─────────────────────────────────────────────────────────────
	{ "movement.speed",
	  "How fast the character is moving across the ground, in metres per second, "
	  "ignoring any vertical motion. The value an animator's walk/run blend wants." },
	{ "movement.verticalSpeed",
	  "The character's vertical speed in metres per second: positive rising, "
	  "negative falling. What tells a jump from a fall." },
	{ "movement.isGrounded",
	  "Is the character standing on something? Read from the character controller, "
	  "so it is the same answer physics used this tick." },
	{ "movement.velocity",
	  "The character's full velocity vector in metres per second." },
	{ "movement.forwardAmount",
	  "How much of the character's motion points the way it is facing, from -1 "
	  "(backwards) through 0 (still or sideways) to 1 (straight ahead). One half "
	  "of a locomotion blend space." },
	{ "movement.rightAmount",
	  "The sideways half of the same pair: -1 strafing left, 1 strafing right." },

	// ── Locomotion ───────────────────────────────────────────────────────────
	{ "locomotion.move",
	  "Tells the character to move in a WORLD-space direction this frame; the "
	  "length is the throttle, 0 to 1. It has to be called every frame it should "
	  "move — the intent is cleared at the end of each one, so a missed frame "
	  "reads as \"stopped\" rather than as \"keep going\"." },
	{ "locomotion.look",
	  "Turns the character by these degrees this frame. Yaw turns it; pitch is "
	  "consumed by a camera rig if one is coupled to it." },
	{ "locomotion.setMaxSpeed",
	  "The character's top speed in metres per second at full input. Changing it "
	  "is how sprinting and wading are done." },
	{ "locomotion.setOrientToMovement",
	  "Whether the character turns to face the way it walks. Switch it off while "
	  "something else owns the facing — a camera rig with coupled rotation, an "
	  "aim mode." },
	{ "locomotion.jump",
	  "Throws the character upward at the Jump Speed its Character Controller was "
	  "authored with. Unlike Move this is a single event, not an intent held per "
	  "frame: call it once when the button goes down. It returns whether the "
	  "character actually left the ground, and refuses in mid-air — which is what "
	  "stops a held button from flying. So when a jump never fires, look at Is "
	  "Grounded before you look at the speed. It is not quite that strict: a jump "
	  "asked for within a tenth of a second of walking off a ledge is still "
	  "granted, because a player who presses a frame late means to jump." },
	{ "locomotion.jumpWith",
	  "A jump at the upward speed given here in metres per second, instead of the "
	  "authored one. For the jumps one number cannot cover: a charge that grows "
	  "while the button is held, a low hop out of a crouch, a launch pad that "
	  "throws whoever steps on it. The ground rule is the same, and a speed of "
	  "zero or less is refused rather than treated as a jump." },

	// ── Navigation ───────────────────────────────────────────────────────────
	{ "nav.moveTo",
	  "Sends a Nav Agent walking to a world position: the route across the baked "
	  "nav mesh is searched right here, and then followed over the next frames "
	  "without anything else being called. The one node an enemy needs in order to "
	  "come after the player. It answers whether the walk started, and the answer "
	  "is worth branching on, because a false means the agent will not move at "
	  "all: no nav mesh has been baked, the destination is off the walkable "
	  "surface, or the two ends sit on parts of the level that do not connect. A "
	  "refused call also leaves the agent walking wherever it already was. For a "
	  "target that keeps moving, simply call it again — each call replans from "
	  "where the agent stands." },
	{ "nav.stop",
	  "Halts the agent where it stands and throws its route away. The way to break "
	  "off a chase or hold a patrol still during a conversation. It leaves the "
	  "destination as it was, so Move To starts a fresh walk afterwards." },
	{ "nav.isMoving",
	  "Is this agent walking a route right now? It falls to false by itself the "
	  "moment the agent arrives, which is how a patrol knows it is time to hand "
	  "out the next waypoint." },
	{ "nav.hasPath",
	  "Whether the agent is holding a route it can still follow. Is Moving is the "
	  "order that was given; this is whether there turned out to be a way. Telling "
	  "the two apart is how \"walking\" is distinguished from \"was sent somewhere "
	  "it cannot reach\"." },
	{ "nav.remainingDistance",
	  "How far the agent still has to walk, measured in metres along the route's "
	  "corners rather than straight through the walls between. Good for slowing an "
	  "NPC down near its goal, or for deciding that a chaser is close enough to "
	  "strike. It is -1, never 0, whenever there is no route to measure — an agent "
	  "standing still, or one that has just arrived — so compare against 0 only "
	  "after Has Path says there is something to compare." },
	{ "nav.setSpeed",
	  "The speed in metres per second at which the agent walks its route from now "
	  "on. The current route is kept and only the pace changes, so this is how one "
	  "guard strolls a patrol and then charges. It is the Nav Agent's own speed, "
	  "not the character's Max Speed, which belongs to input-driven movement; a "
	  "negative value is taken as standing still." },

	// ── UI ───────────────────────────────────────────────────────────────────
	{ "ui.getText",
	  "The text of a UI Text element, by entity. Empty for an element that is not "
	  "one." },
	{ "ui.setText",
	  "Replaces a UI Text element's text. The usual way a score, a timer or a "
	  "subtitle reaches the screen." },
	{ "ui.getColor",
	  "The element's colour, alpha included." },
	{ "ui.setColor",
	  "Sets the element's colour. On a Text element this is the text itself; on an "
	  "Image element it is the tint over the texture." },
	{ "ui.getVisible",
	  "Is this UI element currently shown?" },
	{ "ui.setVisible",
	  "Shows or hides one UI element. Hiding a container hides everything inside "
	  "it, and a hidden element stops receiving clicks." },
	{ "ui.getPosition",
	  "The element's position inside its canvas, in UI units from its anchor." },
	{ "ui.setPosition",
	  "Moves a UI element inside its canvas. Measured from its anchor, so what "
	  "this means depends on which corner or edge the element is anchored to." },
	{ "ui.getSize",
	  "The element's width and height in UI units." },
	{ "ui.setSize",
	  "Resizes a UI element. A stretched anchor overrides this on the axes it "
	  "stretches." },
	{ "ui.setMaterialParam",
	  "Overrides a material parameter on a UI element that uses a material — a "
	  "progress bar's fill, a shader-driven panel. Ok is false when the element "
	  "has no material or the parameter is not declared." },
	{ "ui.pointerOverUI",
	  "Is the pointer over any UI right now? The guard to put in front of a click "
	  "that acts on the world, so a press on a button does not also shoot." },

	// ── Widget ───────────────────────────────────────────────────────────────
	// Creating, showing, hiding and destroying are the Create/Show/Hide/Destroy
	// Widget nodes, not engine calls; these three take the Widget those nodes hand
	// out.
	{ "widget.setZOrder",
	  "Which widget draws over which: higher is nearer the front. For keeping a "
	  "pause menu above a HUD. Takes the Widget from a Create Widget node." },
	{ "widget.isVisible",
	  "Is this widget on screen? A new widget starts visible, so this is false "
	  "only after Hide Widget, or for a Widget that was destroyed." },
	{ "widget.callFunction",
	  "Calls a public function on the widget's own graph by name — the way the "
	  "outside talks to a screen. Ok is false when the widget or the function is "
	  "not there." },

	// ── Cursor / App ─────────────────────────────────────────────────────────
	{ "cursor.setVisible",
	  "Shows or hides the mouse cursor. A first-person game hides it while "
	  "playing and shows it again for menus; see also the Input Mode calls, which "
	  "decide who receives the clicks." },
	{ "app.quit",
	  "Ends the game. In the editor it stops play mode instead, so a quit button "
	  "can be tested without closing the editor." },

	// ── Math ─────────────────────────────────────────────────────────────────
	// The trigonometry and rounding rows are registered through a helper rather
	// than written out one by one in EngineApi.cpp — which is exactly why the
	// coverage test walks the LIVE registry: reading the source file misses them.
	{ "math.sin",
	  "The sine of an angle given in RADIANS. Feed degrees through To Radians "
	  "first — this is the one that catches everyone." },
	{ "math.cos",
	  "The cosine of an angle in radians. Paired with Sine it turns an angle into "
	  "a direction: cos for X, sin for Z." },
	{ "math.tan",
	  "The tangent of an angle in radians. Runs off to infinity near 90 degrees, "
	  "so keep the input away from it." },
	{ "math.sqrt",
	  "The square root. A negative input has none and yields NaN, which then "
	  "spreads silently through everything downstream — clamp at 0 first if the "
	  "value can go negative." },
	{ "math.abs",
	  "Drops the sign: -3 and 3 both come out as 3. How a distance is made out of "
	  "a difference." },
	{ "math.floor",
	  "Rounds DOWN to a whole number: 2.9 becomes 2, and -2.1 becomes -3." },
	{ "math.ceil",
	  "Rounds UP to a whole number: 2.1 becomes 3." },
	{ "math.round",
	  "Rounds to the nearest whole number, halves going up." },
	{ "math.sign",
	  "-1 for a negative number, 1 for a positive one, 0 for zero. Which way, "
	  "without how far." },
	{ "math.pow",
	  "Raises the base to the exponent." },
	{ "math.mod",
	  "The remainder after division. Wrapping an angle back into 0..360, or doing "
	  "something every Nth item. A divisor of 0 yields 0 rather than a NaN." },
	{ "math.min",
	  "The smaller of the two values." },
	{ "math.max",
	  "The larger of the two values. Max with 0 is the usual way to stop a value "
	  "going negative." },
	{ "math.radians",
	  "Converts degrees to radians. The engine's rotations are in degrees and the "
	  "trigonometry is in radians, so this sits between them." },
	{ "math.degrees",
	  "Converts radians back to degrees — the direction to use before writing an "
	  "angle into a transform." },
	{ "math.atan2",
	  "The angle of the vector (X, Y) in radians, over the full circle. This is "
	  "how a direction becomes a heading, and it gets the quadrant right where "
	  "plain arctangent cannot." },
	{ "math.clamp",
	  "Keeps a value inside Lo..Hi. Below Lo it becomes Lo, above Hi it becomes "
	  "Hi." },
	{ "math.lerp",
	  "Blends from A to B by T: 0 gives A, 1 gives B, 0.5 the middle. T outside "
	  "0..1 is not clamped — it keeps going past either end." },
	{ "math.length",
	  "The length of a 2D vector." },
	{ "math.distance",
	  "The distance between two 2D points." },
	{ "math.length3",
	  "The length of a 3D vector — how far it reaches, regardless of direction." },
	{ "math.distance3",
	  "The distance between two 3D points in metres." },
	{ "math.normalize3",
	  "The same direction at length 1. A zero vector has no direction and stays "
	  "zero rather than becoming a NaN." },
	{ "math.dot3",
	  "The dot product: how much two directions agree. 1 means the same way, 0 "
	  "perpendicular, -1 opposite. With normalised vectors it is the cosine of the "
	  "angle between them, which is how a field-of-view check is written." },
	{ "math.cross",
	  "A vector perpendicular to both inputs. Forward crossed with up gives right, "
	  "which is how a local axis is built from a facing." },

	// ── Random ───────────────────────────────────────────────────────────────
	{ "random.seed",
	  "Fixes the random sequence, so a run can be repeated exactly. Seed with a "
	  "constant while debugging and everything random happens the same way twice." },
	{ "random.value",
	  "A random number from 0 up to (but not including) 1. An exec node, not a "
	  "pure one, precisely because it changes every call: a pure node would be "
	  "re-evaluated per reader and hand two of them different numbers." },
	{ "random.range",
	  "A random number between Min and Max." },
	{ "random.rangeInt",
	  "A random whole number from Min to Max, both ends included." },
	{ "random.chance",
	  "True with probability P (0 never, 1 always). The coin flip, spelled out." },

	// ── Time ─────────────────────────────────────────────────────────────────
	{ "time.deltaTime",
	  "Seconds since the previous frame, scaled by the time scale. Multiply "
	  "anything per-second by it and the motion stays the same on a fast machine "
	  "and a slow one." },
	{ "time.unscaledDeltaTime",
	  "The same, but ignoring Set Time Scale — so a UI animation keeps running at "
	  "normal speed while the game is in slow motion or paused." },
	{ "time.elapsed",
	  "Seconds since the game started, scaled." },
	{ "time.frameCount",
	  "How many frames have been drawn. For \"every Nth frame\" work; frames are "
	  "not a unit of time, so do not pace anything with it." },
	{ "time.unscaledElapsed",
	  "Real seconds since the game started — it keeps counting through slow "
	  "motion, a pause and a hit stop, which is what a pause menu needs to time "
	  "anything at all." },
	{ "time.setTimeScale",
	  "Slows down or speeds up game time: 0.5 is half speed, 0 is slow motion "
	  "down to a standstill, 1 is normal. This is the scale you asked for; a "
	  "Pause Game or a Hit Stop overrides it without erasing it. Real-time Delay "
	  "nodes and Unscaled Delta Time are deliberately unaffected." },
	{ "time.timeScale",
	  "The time scale that was asked for — what Set Time Scale last wrote, even "
	  "while a pause or a hit stop is holding the clock at zero. Effective Time "
	  "Scale is the one actually in effect." },
	{ "time.pause",
	  "Pauses the game: Delta Time goes to zero and the player's input stops "
	  "being delivered, which is what makes a pause menu a pause menu. The time "
	  "scale is left alone, so resuming returns to the slow motion it came from." },
	{ "time.resume",
	  "Lifts the pause this script asked for. It cannot lift the one the window "
	  "put in place when the game lost focus — those are separate reasons, and "
	  "the game runs again only once none of them is left." },
	{ "time.isPaused",
	  "True while the game is paused. Note it stays FALSE during a hit stop: a "
	  "freeze stops the world, it does not stop the player being heard." },
	{ "time.hitStop",
	  "Freezes the game for a moment of real time — 0.08 to 0.12 seconds is the "
	  "usual weight for a landed hit. It returns to exactly the speed it came "
	  "from, slow motion included, and input is never swallowed. Triggering it "
	  "again takes the longer of the two windows rather than adding them up." },
	{ "time.isFrozen",
	  "True while a Hit Stop is still running. For the effect that spawned it, "
	  "not for gameplay logic." },
	{ "time.effectiveScale",
	  "What actually multiplies Delta Time this frame: the time scale, or zero "
	  "while a pause or a hit stop is on. The number to put on a debug HUD." },

	// ── Player ───────────────────────────────────────────────────────────────
	{ "player.possess",
	  "Hands a character to a player controller: from here on the controller's "
	  "input drives it, and the camera rig follows it. Possessing a second "
	  "character releases the first." },
	{ "player.unpossess",
	  "Releases whatever the controller was driving. The character stays in the "
	  "world, just without anyone at the controls." },
	{ "player.possessed",
	  "The character this controller is currently driving, or an empty reference." },
	{ "player.controllerOf",
	  "Which controller is driving this character — the inverse of Possessed." },
	{ "player.controller",
	  "The local player's controller. The starting point in a single-player game, "
	  "where there is exactly one." },
	{ "player.character",
	  "The character the local player is driving right now. Shorthand for "
	  "Possessed of Controller." },

	// ── Input ────────────────────────────────────────────────────────────────
	{ "input.keyDown",
	  "Is this key held down right now? The name is SDL's spelling (\"W\", "
	  "\"Space\", \"Escape\"). True for as long as it is held, so use it for "
	  "movement and put one-shot actions behind an Input Action instead." },
	{ "input.mouseButton",
	  "Is this mouse button held? 0 left, 1 middle, 2 right." },
	{ "input.mousePosition",
	  "The pointer's position in window pixels, measured from the top-left." },
	{ "input.mouseDelta",
	  "How far the pointer moved since the previous frame, in pixels. This is what "
	  "mouse look reads — not the position, which stops changing once the cursor "
	  "is captured." },
	{ "input.scrollDelta",
	  "How far the wheel turned this frame; positive is away from the user." },
	{ "input.gamepadConnected",
	  "Is a gamepad plugged in? Worth checking before showing button prompts." },
	{ "input.gamepadButton",
	  "Is this gamepad button held? SDL's names, so \"a\", \"b\", \"dpup\" — not "
	  "\"south\" and not \"cross\"." },
	{ "input.gamepadAxis",
	  "A gamepad axis from -1 to 1, deadzone already applied. Names are SDL's: "
	  "\"leftx\", \"lefty\", \"righttrigger\"." },
	{ "input.setModeGameOnly",
	  "The game receives input and the UI does not: the cursor is captured and "
	  "hidden, mouse look works, buttons cannot be clicked. First-person play." },
	{ "input.setModeGameAndUI",
	  "Both receive input — the cursor is visible, the UI is clickable, and the "
	  "game still reads keys. Inventory screens and build modes." },
	{ "input.setModeUIOnly",
	  "Only the UI receives input; the game sees nothing. What a pause menu wants, "
	  "so a click behind it cannot fire a weapon." },
	{ "input.mode",
	  "Which input mode is active: \"GameOnly\", \"GameAndUI\" or \"UIOnly\"." },

	// ── Camera ───────────────────────────────────────────────────────────────
	{ "camera.getPosition",
	  "The active camera's world position." },
	{ "camera.setPosition",
	  "Moves the active camera. Ignored the moment a camera rig is driving it — "
	  "the rig recomputes the position from its target every frame." },
	{ "camera.getRotation",
	  "The active camera's rotation as euler degrees." },
	{ "camera.setRotation",
	  "Turns the active camera. Same caveat as Set Position: a rig overrides it." },
	{ "camera.getFov",
	  "The vertical field of view in degrees." },
	{ "camera.setFov",
	  "Sets the vertical field of view in degrees. Widening it while sprinting is "
	  "the classic speed cue; animate it rather than snapping." },
	{ "camera.setRigMode",
	  "Switches the camera rig between first person (0) and third person (1). "
	  "First person is the same rig with an arm length of zero." },
	{ "camera.getRigMode",
	  "0 first person, 1 third person. 0 when there is no rig at all." },
	{ "camera.setRigTarget",
	  "Points the rig at an entity to follow. Setting it to nothing puts the rig "
	  "back on whichever character the player possesses." },
	{ "camera.setArmLength",
	  "How far behind the target the third-person camera sits, in metres. 0 puts "
	  "it in the target's head." },
	{ "camera.getArmLength",
	  "The rig's current arm length in metres." },
	{ "camera.setTargetYawMode",
	  "Free (0) lets the character face where it walks while the camera looks "
	  "elsewhere. Follow (1) turns the character with the camera, which is what "
	  "makes strafing and walking backwards work. Follow Smoothed (2) does the "
	  "same, but swings the body after the camera at the rig's turn rate." },
	{ "camera.getTargetYawMode",
	  "0 Free (the character faces where it walks), 1 Follow (it turns with the "
	  "camera), 2 Follow Smoothed (it swings after the camera)." },
	{ "camera.getRigYaw",
	  "Which way the rig is looking, in degrees." },
	{ "camera.getRigPitch",
	  "How far up or down the rig is looking, in degrees." },
	{ "camera.addYawPitch",
	  "Turns the rig by these degrees, clamped to the pitch limits the rig "
	  "carries. This is what a look input drives — adding to the angles rather "
	  "than setting them is what keeps two input sources from fighting." },
	{ "camera.setLagEnabled",
	  "Switches camera lag on or off. With it on the camera eases after its "
	  "target instead of being welded to it; off is the default, so an existing "
	  "scene looks exactly as it did." },
	{ "camera.getLagEnabled",
	  "Whether the rig is easing after its target rather than following it "
	  "rigidly." },
	{ "camera.setLagSpeeds",
	  "How quickly the camera catches up, per second: the first number moves the "
	  "camera to its target, the second swings the boom round to the look "
	  "direction. Larger is tighter; 0 never catches up at all." },
	{ "camera.snapRig",
	  "Puts the camera exactly where it belongs on the next frame instead of "
	  "easing into it. This is what a teleport, a respawn or a cut needs — "
	  "without it the camera sails across the level to catch up." },
	{ "camera.playShake",
	  "Shakes the camera: a position amplitude in metres, a rotation amplitude "
	  "in degrees, a frequency in shakes per second and a duration. A duration "
	  "of 0 or less runs until it is stopped, which is what the handle it "
	  "returns is for; a one-shot can drop it." },
	{ "camera.stopShake",
	  "Ends one running shake by the handle Play Camera Shake gave out. An "
	  "unknown or already-finished handle does nothing." },
	{ "camera.stopAllShakes",
	  "Ends every shake on the camera at once — what a cutscene or a death "
	  "screen wants, rather than remembering each handle." },
	{ "camera.kickFov",
	  "Pushes the field of view by these degrees and lets it fall back: attack, "
	  "hold and decay in seconds. Negative degrees zoom in. It never changes the "
	  "camera's own FOV setting, so Get Camera FOV keeps answering what the "
	  "author set." },
	{ "camera.blendTo",
	  "Hands the view to another camera over a number of seconds. Curve 0 is "
	  "linear, 1 smoothstep, 2 ease-out. 0 seconds is a straight cut, and so is "
	  "switching the main camera by hand — a blend only ever starts here." },
	{ "camera.isBlending",
	  "Whether the picture is currently easing in from another camera. True "
	  "until the blend has fully arrived." },

	// ── Audio ────────────────────────────────────────────────────────────────
	{ "audio.play",
	  "Plays a sound flat — the same in both ears, wherever the listener is. "
	  "Music, UI clicks, narration. The handle it returns is what Stop and Is "
	  "Playing take; a one-shot can simply drop it." },
	{ "audio.playAt",
	  "Plays a sound at a world position, quieter with distance. Full volume "
	  "inside Min Dist, silent past Max Dist. Needs an Audio Listener in the "
	  "scene, or there are no ears to hear it from." },
	{ "audio.stop",
	  "Stops one playing sound by handle. A handle that has already finished is "
	  "harmless." },
	{ "audio.stopAll",
	  "Stops every sound at once — what a scene change or a hard cut wants." },
	{ "audio.isPlaying",
	  "Is this handle still sounding? False once it has finished or been stopped." },
	{ "audio.setBusVolume",
	  "Sets the volume of a mixer bus by name, so a whole group — music, sfx, "
	  "voice — moves together. This is what a settings screen writes to." },
	{ "audio.setSoundPosition",
	  "Moves a playing spatial sound to a new world position. For a sound that "
	  "has to follow something that moves while it plays." },

	// ── File ─────────────────────────────────────────────────────────────────
	{ "fs.writeText",
	  "Writes a text file, replacing whatever was there. Paths are relative to the "
	  "game's own data directory; it cannot write outside it. For savegames use "
	  "the Save calls, which handle the format and the atomic write." },
	{ "fs.readText",
	  "Reads a whole text file. Empty when the file does not exist — ask Exists "
	  "first if the difference matters." },
	{ "fs.exists",
	  "Is there a file at this path?" },
	{ "fs.remove",
	  "Deletes a file. Ok is false if it was not there." },
	{ "fs.makeDir",
	  "Creates a directory, parents included. Ok is true if it already existed." },

	// ── Save ─────────────────────────────────────────────────────────────────
	{ "save.create",
	  "Starts a NEW save from the project's SaveGame Template, with the fields "
	  "seeded from the template defaults, and makes it active. In memory only "
	  "until Write Save." },
	{ "save.load",
	  "Loads Saves/<id>.json, re-validates it against the template it names and "
	  "makes it active. Fails if the save or its template is missing." },
	{ "save.write",
	  "Persists the active save to disk, atomically." },
	{ "save.close",
	  "Drops the active save without writing it." },
	{ "save.activeId",
	  "Id of the active save; empty when there is none." },
	{ "save.list",
	  "Ids of every save on disk." },
	{ "save.exists",
	  "Is there a save with this id on disk?" },
	{ "save.delete",
	  "Deletes a save file. Does not touch the active save." },
	{ "save.fields",
	  "Field names the active save's template declares — so a graph never has to "
	  "hardcode them." },
	{ "save.setNumber",
	  "Writes a Float, Int or Enum field of the active save. The field is picked "
	  "from the template on the node itself." },
	{ "save.getNumber",
	  "Reads a Float, Int or Enum field. An unknown field, or no active save, "
	  "yields the Default input rather than zero." },
	{ "save.setString",
	  "Writes a String field of the active save." },
	{ "save.getString",
	  "Reads a String field; falls back to the Default input." },
	{ "save.setBool",
	  "Writes a Bool field of the active save." },
	{ "save.getBool",
	  "Reads a Bool field; falls back to the Default input." },
	{ "save.setStruct",
	  "Writes a Struct field. The value's definition must match the one the "
	  "template declares." },
	{ "save.getStruct",
	  "Reads a Struct field of the active save." },

	// ── Scene ────────────────────────────────────────────────────────────────
	{ "scene.load",
	  "Replaces the current scene with another one. It happens at the end of the "
	  "frame, not inside this call — the exec chain after it still runs, in the "
	  "old scene." },
	{ "scene.loadAdditive",
	  "Loads another scene ALONGSIDE this one at a world offset and returns its "
	  "zone id, which every other zone call takes. Hidden loads it without showing "
	  "it, so the streaming in can be paid for before the player arrives." },
	{ "scene.unloadZone",
	  "Removes an additively loaded zone and everything in it." },
	{ "scene.activate",
	  "Makes the pending level the active one. The second half of a load that was "
	  "started earlier." },
	{ "scene.hasPendingLevel",
	  "Is a level load waiting to be activated?" },
	{ "scene.showZone",
	  "Shows a zone that was loaded hidden. The entities were there all along; "
	  "this is the moment they start being drawn and ticked." },
	{ "scene.hideZone",
	  "Hides a zone without unloading it — cheap to undo, unlike unload." },
	{ "scene.zonePosition",
	  "The world offset a zone was loaded at." },
	{ "scene.setZonePosition",
	  "Moves a whole loaded zone to a new world offset. How an endless world "
	  "recycles a chunk instead of loading a new one." },
	{ "scene.zoneScene",
	  "Which scene asset a zone was loaded from." },
	{ "scene.loadedZones",
	  "The ids of every additively loaded zone, as an array." },
	{ "scene.available",
	  "Every scene asset in the project, as an array of paths — so a level select "
	  "does not have to hardcode the list." },

	// ── String ───────────────────────────────────────────────────────────────
	{ "string.length",
	  "How many characters the text has." },
	{ "string.substring",
	  "Count characters starting at Start (counting from 0). A range past the end "
	  "is clamped rather than an error." },
	{ "string.contains",
	  "Does the text contain this piece? Case-sensitive." },
	{ "string.find",
	  "Where the piece first occurs, counting from 0, or -1 when it does not." },
	{ "string.replace",
	  "Replaces every occurrence of From with To." },
	{ "string.toUpper",
	  "The text in upper case." },
	{ "string.toLower",
	  "The text in lower case." },
	{ "string.trim",
	  "Removes whitespace from both ends — what a text input needs before it is "
	  "compared to anything." },
	{ "string.startsWith",
	  "Does the text begin with this piece?" },
	{ "string.endsWith",
	  "Does the text end with this piece?" },
	{ "string.toNumber",
	  "Parses the text as a number. Text that is not one yields 0, so this cannot "
	  "tell \"0\" from nonsense — validate first if that matters." },
	};

	// ── The sky properties ───────────────────────────────────────────────────
	// The Environment category is generated from an X-list in EngineApi.h: fifty
	// fields, each producing an env.get… and an env.set… row. A hand-written
	// table would be a hundred rows that say the same thing twice and go stale
	// the moment a field is added — so this is keyed by the FIELD (the part after
	// "env.get" / "env.set"), and the get/set wording is composed around it.
	//
	// The sentences are the ones the Sky entity's own properties carry in the
	// Details panel (EditorHelp.cpp), because they describe the same value.
	struct Field { const char* name; const char* what; };
	constexpr Field kEnvFields[] = {
		{ "TimeOfDay", "the sky's clock: 0 and 1 are midnight, 0.25 sunrise, 0.5 noon" },
		{ "CycleSeconds", "how long a full day takes while the day-night cycle runs, in seconds" },
		{ "SunIntensity", "how strong the sun is, and with it the whole daylit scene" },
		{ "MoonIntensity", "how strong the moonlight is — the difference between a night you can see in and a black screen" },
		{ "MoonPhase", "the moon's phase: 0 new, 0.5 full" },
		{ "MoonCycleDays", "how many days a full new-to-full-to-new cycle takes" },
		{ "CloudCoverage", "how much of the sky the cloud layer fills: 0 clear, 1 overcast" },
		{ "WindDirection", "which way the clouds drift, in degrees" },
		{ "WindSpeed", "how fast the clouds drift" },
		{ "CloudHeight", "the world height of the cloud deck's base, in metres" },
		{ "CloudShadowStrength", "how dark the shadows the cloud layer casts on the ground get" },
		{ "CloudEvolution", "how fast clouds change shape as they drift; 0 freezes the formation" },
		{ "CloudDensity", "how solid the clouds are: low is wispy, high is thick and dark underneath" },
		{ "CloudFluffiness", "how broken up the cloud edges are, from a smooth sheet to billowy" },
		{ "ContrailAmount", "how many vapour trails cross the high sky" },
		{ "CirrusAmount", "how much thin fibrous cirrus sits above the main clouds" },
		{ "CirrusSeed", "which cirrus pattern is drawn — a different sky, not a different amount" },
		{ "GodRays", "the shafts of light through gaps in the cloud; they need broken cover to shine through" },
		{ "ShootingStars", "how often meteors streak across the night sky" },
		{ "LensFlare", "the camera artefact when the sun is in shot" },
		{ "FogDensity", "how thick the atmospheric haze is; even a very small amount gives a landscape distance" },
		{ "FogHeightFalloff", "how much the fog pools near the ground instead of filling the air evenly" },
		{ "RainAmount", "how hard it is raining, 0 to 1" },
		{ "SnowAmount", "how hard it is snowing, 0 to 1" },
		{ "Wetness", "how wet surfaces look after rain" },
		{ "Flash", "the lightning flash, driven per strike by the Weather system" },
		{ "AuroraIntensity", "how strong the aurora ribbons are; 0 switches them off" },
		{ "MilkyWayIntensity", "how bright the galaxy's band is across the night sky" },
		{ "NebulaIntensity", "how visible the deep-space nebula is behind the stars" },
		{ "NebulaSeed", "which nebula shape is generated" },
		{ "NebulaCoverage", "how much of the sky the nebula spans" },
		{ "StarBrightness", "how bright the star field is; stars fade out by themselves as the sky lightens" },
		{ "StarDensity", "how many stars there are" },
		{ "StarSize", "how large stars are drawn" },
		{ "StarSizeVariation", "how much star sizes differ; 0 makes every star the same, which reads as artificial" },
		{ "StarGlow", "the halo around each star: 0 crisp points, higher a softer sky" },
		{ "StarTwinkle", "how much the stars flicker" },
		{ "AuroraHeight", "how tall the aurora ribbons stand above the horizon" },
		{ "AuroraFragmentation", "how broken the ribbons are, from a smooth curtain to ragged streaks" },
		{ "DayNightCycle", "whether time of day advances on its own while the scene runs" },
		{ "AutoAdvance", "whether the moon phase moves with the days on its own" },
		{ "MoonPhaseAuto", "whether the moon phase advances with the day-night cycle" },
		{ "CloudShadows", "whether the cloud layer darkens the ground under it" },
		{ "CloudInterShadows", "whether clouds cast shadows within their own body, so a tall tower darkens what is behind it" },
		{ "LowResClouds", "whether the clouds are raymarched at quarter resolution and upscaled — much cheaper, slightly softer" },
		{ "CloudMode", "the cloud layer: 0 painted on the sky dome (cheap, never comes closer), 1 real 3D volumes the camera can fly into" },
		{ "CloudQuality", "how many steps the cloud raymarch takes — the most expensive sky setting there is" },
		{ "CloudStyle", "0 the original flat drifting layer, 1 cauliflower shapes that tower and dissolve" },
		{ "NebulaQuality", "the nebula's detail level: 0 performance, 1 high, 2 max" },
		{ "SunColor", "the tint of the sunlight; the sky reddens the sun near the horizon on its own" },
		{ "MoonColor", "the tint of the moonlight at night" },
		{ "CloudTint", "the colour multiplied into the clouds" },
		{ "NebulaColor", "the nebula's cool interior veil" },
		{ "NebulaColor2", "the nebula's warm filament regions" },
		{ "NebulaColor3", "the nebula's deep red filament regions" },
		{ "AuroraColor", "the aurora's colour at its lower edge, where it is brightest" },
		{ "AuroraColorTop", "the colour the aurora fades into at the top of the ribbon" },
		{ "StarColor", "the tint over the whole star field; the per-star variation survives it" },
	};

	// The sentence both env rows for a field are built from, or null.
	const char* envField(std::string_view name)
	{
		for (const Field& f : kEnvFields)
			if (name == f.name) return f.what;
		return nullptr;
	}

	// Split "env.getCloudCoverage" into ("get", "CloudCoverage").
	bool splitEnv(std::string_view id, bool& isSet, std::string_view& field)
	{
		constexpr std::string_view kGet = "env.get";
		constexpr std::string_view kSet = "env.set";
		if (id.size() > kGet.size() && id.substr(0, kGet.size()) == kGet)
		{ isSet = false; field = id.substr(kGet.size()); return true; }
		if (id.size() > kSet.size() && id.substr(0, kSet.size()) == kSet)
		{ isSet = true;  field = id.substr(kSet.size()); return true; }
		return false;
	}
} // namespace

std::string engineCall(std::string_view id)
{
	for (const Row& r : kDocs)
		if (id == r.id) return r.doc;

	bool isSet = false;
	std::string_view field;
	if (splitEnv(id, isSet, field))
	{
		if (const char* what = envField(field))
		{
			std::string out = isSet ? "Sets " : "Reads ";
			out += what;
			out += ". This is the Sky entity's Environment component — the same "
			       "value its Details panel shows";
			// The one thing a graph author has to know before writing to the sky:
			// the Weather system owns some of these while it is present.
			out += isSet ? ". A Weather component in the scene writes cloud "
			               "coverage, fog, wind and precipitation every tick, so a "
			               "value set here is overwritten while one exists."
			             : ".";
			return out;
		}
	}
	return {};
}

bool hasEngineCall(std::string_view id)
{
	return !engineCall(id).empty();
}

int         explicitCount()      { return static_cast<int>(std::size(kDocs)); }
const char* explicitId(int i)    { return kDocs[i].id; }
int         envFieldCount()      { return static_cast<int>(std::size(kEnvFields)); }
const char* envFieldName(int i)  { return kEnvFields[i].name; }

} // namespace HE::Ed::NodeDocs
