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
	{ "widget.addChild",
	  "Puts another widget INSIDE this one, while it runs: the asset is grafted "
	  "under the element you name, so a Vertical Box called \"List\" stacks a row "
	  "per call. That is how a list of unknown length is built — a todo list, "
	  "search results, a chat. The Child that comes back is a live object: Set "
	  "External writes its public variables, Call External runs its functions, "
	  "Bind Event listens to what it emits. 0 when the widget, the element or the "
	  "asset is not there." },
	{ "widget.removeChild",
	  "Takes one of those rows out again, by the Child the Add gave you. Its "
	  "elements and its logic both go. Ok is false when that child is not in this "
	  "widget (already removed, or it belongs to another one)." },
	{ "widget.clearChildren",
	  "Empties one element of everything Add Widget Child put in it, and says how "
	  "many went. The short way to rebuild a list from scratch instead of "
	  "tracking every row you added." },
	{ "widget.setListCount",
	  "Tells a ListView how many items there are. It keeps no items of its own: "
	  "it works out which of them fit on screen, puts up only those rows, and asks "
	  "you to fill each one in through On Row Bind. That is why ten thousand items "
	  "cost ten rows instead of ten thousand elements. Name the list by the name "
	  "its element has in the designer. Safe to call from inside On Row Bind — an "
	  "endless list that loads more when its last row appears is written exactly "
	  "that way; the new count takes effect on the next frame." },
	{ "widget.listCount",
	  "How many items the list was last told it has. Not how many rows are on "
	  "screen — that is the list's own business." },
	{ "widget.listRow",
	  "The live row showing that item, or 0 when it is scrolled out of sight. This "
	  "is what an On Row Bind handler writes into: Set External for the row's "
	  "public variables, Call External for its functions. Do not hold on to it — "
	  "the same row is pointed at a different item as soon as the list scrolls." },
	{ "widget.refreshList",
	  "Asks every row on screen to be filled in again, without moving anything. "
	  "What you call after sorting, filtering or editing your data: the list never "
	  "saw it, so it cannot notice that it changed. The selection is by INDEX, so "
	  "after a sort it points at whichever items now sit at those positions — "
	  "remap it yourself if it has to follow the same rows. Calling this from "
	  "inside On Row Bind is allowed: it takes effect on the next frame rather "
	  "than looping back into the bind that asked for it." },
	{ "widget.setListSelected",
	  "Picks or unpicks one item. An index of -1 clears the selection. Does nothing "
	  "when the list's Selection is None; in Single mode picking one drops the "
	  "other. Fires On Selection Changed when the set really changed." },
	{ "widget.listSelected",
	  "The picked item's index, or -1 when nothing is picked. With Multiple "
	  "selection this is the lowest of them." },
	{ "widget.showModal",
	  "Puts this widget up as a DIALOG: the screen behind it dims, nothing under "
	  "it can be clicked, scrolled or reached with the keyboard, and the focus "
	  "starts inside it. It is raised above every other widget, so it cannot end "
	  "up blocking input while drawing behind something. It stays until Close Top "
	  "Layer, Escape or the Back button — that is what makes it modal." },
	{ "widget.openPopup",
	  "Puts the widget up at a point on screen — in render-target pixels — and "
	  "lets a click anywhere else dismiss it. Its root elements are moved there "
	  "and pushed back inside the screen, whatever anchors they were drawn with, "
	  "so a menu near the bottom edge opens upwards instead of off the screen. No "
	  "dimming: a popup is left by looking elsewhere, a dialog has to be answered." },
	{ "widget.openPopupAtPointer",
	  "The same, at the mouse. This is the context menu: answer On Right Clicked "
	  "with it and the menu appears where the click was. The engine remembers "
	  "where the pointer last was, so the graph does not have to." },
	{ "widget.closeTopLayer",
	  "Closes the topmost dialog, popup or open dropdown and says whether there "
	  "was one. Exactly what Escape and the gamepad's Back button do — so a "
	  "\"Cancel\" button is this node and nothing else. The widget that closes gets "
	  "On Dismissed, and the keyboard focus goes back where it was before it "
	  "opened. Hide Widget on a dialog does the same thing — hiding one IS closing "
	  "it, so it lets go of the input, the focus and the dimming and fires On "
	  "Dismissed too. This node is the one to reach for when you do not have the "
	  "dialog's own id, which is the usual case for a Cancel button." },

	{ "widget.scrollListToItem",
	  "Scrolls just far enough for that item to be fully visible, and does nothing "
	  "when it already is — so stepping through a list does not re-centre it on "
	  "every step." },

	// ── Animation ────────────────────────────────────────────────────────────
	{ "widget.animate",
	  "Moves a NUMBER of one element from where it is to where you say, over "
	  "the given seconds, along an easing curve (\"Linear\", \"Out Quad\", "
	  "\"Out Back\"…). Opacity, corner radius, rotation, font size. Zero seconds "
	  "writes it at once. Starting a second one on the same property replaces the "
	  "first from wherever the value has got to, so a value can be retargeted "
	  "mid-flight without a jump. OnAnimationFinished says when it lands, and "
	  "names the property it was." },
	{ "widget.animateColor",
	  "The same for a COLOUR — a fade, a flash, a hover that arrives instead of "
	  "snapping. Every colour an element has can be animated, including a "
	  "button's hovered and pressed ones." },
	{ "widget.animateVec2",
	  "The same for a POINT: Position to slide something in, Size to grow it. "
	  "\"Out Back\" is the curve that makes a dialog land rather than arrive." },
	{ "widget.playAnimation",
	  "Plays one of the animations the widget carries — the ones made in the "
	  "Designer's timeline — picked from the dropdown. Leave Widget unwired and "
	  "it plays this widget's own. Whether it loops is the animation's own "
	  "decision. Direction runs it forwards, backwards, or out and back. "
	  "Restore After Completed puts the properties it moved back the way they "
	  "were when it finishes, which is the whole of \"flash this and undo it\". "
	  "An embedded component's animations count too, so a page can play what its "
	  "component brought with it. OnClipFinished says when it ends; a looping "
	  "one never does, and never restores either." },
	{ "widget.playAnimationLooped",
	  "The same, but you decide about looping instead of the animation. For the "
	  "case where one clip is both the spinner that runs until you stop it and "
	  "the flourish that plays once." },
	{ "widget.childRef",
	  "A reference to the component sitting in one of this widget's slots, by "
	  "the slot's name. A component is a class like any other, so this is how "
	  "you call its functions, bind its events and read its public variables — "
	  "and how a page tells one of three identical cards apart. Leave Widget "
	  "unwired for this widget's own slots." },
	{ "widget.stopAllAnimations",
	  "Stops everything moving in the widget: the authored animations and the "
	  "single-property ones both. What a screen being torn down or swapped "
	  "reaches for, because it does not have to name what it started. Values "
	  "stay where they got to; Restore Original State is the one that puts them "
	  "back." },
	{ "widget.restoreOriginalState",
	  "Stops everything and puts every property an animation ever touched back "
	  "the way it was BEFORE anything animated it — not the way it was a moment "
	  "ago. Stopping is part of it: a clip left running would write over the "
	  "restored values on the next frame. Outputs how many properties were put "
	  "back, which is 0 when nothing had been animated." },
	{ "widget.stopAnimationClip",
	  "Stops that animation, or every one of the widget's when the name is left "
	  "empty. Values stay where they got to, and nothing is reported — cancelled "
	  "is not finished." },
	{ "widget.isPlayingAnimation",
	  "Whether that animation is running right now. What a toggle asks before "
	  "deciding which way to go." },
	{ "widget.stopAnimation",
	  "Stops what is running on that property, or on the whole element when the "
	  "property is left empty. The value stays where it got to — a stop is not a "
	  "rewind — and a stopped animation reports nothing, because cancelled is not "
	  "finished." },

	// ── Theme ────────────────────────────────────────────────────────────────
	{ "theme.set",
	  "Switches the whole application to another Theme asset. Every element bound "
	  "to a colour role takes the new colour at once — that is what a role is for. "
	  "Ok is false when there is no such asset or it cannot be read." },
	{ "theme.setMode",
	  "\"Light\", \"Dark\" or \"System\". Both colours live in the SAME theme, so "
	  "this is a switch and not a second set of widgets: every bound colour "
	  "re-resolves immediately. System is the default and follows the desktop, "
	  "including while the application runs. Anything else is ignored." },
	{ "theme.getMode",
	  "Which of the two colours is on screen right now — Light or Dark, never "
	  "System. This is what to ask when the answer decides something else, like "
	  "which icon to show." },
	{ "theme.getPreference",
	  "What was ASKED for: Light, Dark or System. This is what a Preferences "
	  "screen shows and writes back, because System is a rule and not a colour — "
	  "storing what it resolved to today would stop it following tomorrow." },
	{ "theme.setFontScale",
	  "How big the text is for the person reading it: 1 is what the designer "
	  "drew, 1.5 is half again. Only the TEXT grows — corners, padding and tab "
	  "strips stay where they were authored, because a whole interface zoomed is "
	  "a different setting (the display scale, which the system supplies). "
	  "Clamped to 0.5 … 3." },
	{ "theme.getFontScale",
	  "The text size actually in use, after clamping — what a Preferences screen "
	  "shows next to its slider." },

	// ── Cursor / App ─────────────────────────────────────────────────────────
	{ "cursor.setVisible",
	  "Shows or hides the mouse cursor. A first-person game hides it while "
	  "playing and shows it again for menus; see also the Input Mode calls, which "
	  "decide who receives the clicks." },
	{ "app.quit",
	  "Ends the game. In the editor it stops play mode instead, so a quit button "
	  "can be tested without closing the editor." },
	{ "app.setTitle",
	  "Sets the text in the window's title bar. Only an application owns its "
	  "window outright — in the editor this is ignored rather than renaming the "
	  "editor itself." },
	{ "app.setSize",
	  "Resizes the window, in logical points. Both sides must be positive; a zero "
	  "or negative size is refused rather than passed on to the platform." },
	{ "app.size",
	  "The window's current size in logical points, as X and Y. On a high-DPI "
	  "display this is the points size, not the pixel count behind it." },
	{ "app.minimize",
	  "Puts the window away into the dock or the taskbar. The other half of a "
	  "title bar a window draws itself: without a system frame there is no "
	  "minimise button but the one you drew." },
	{ "app.maximize",
	  "Fills the screen's work area with the window, or puts it back where it "
	  "was when this is off. One call for both, because one button on a title "
	  "bar does both: wire it to Is Window Maximized inverted." },
	{ "app.isMaximized",
	  "Whether the window is maximised right now. Asked of the system rather "
	  "than remembered, so it stays right after somebody double-clicked the "
	  "title bar or used the platform's own shortcut." },
	{ "app.requestRedraw",
	  "Draws one more frame. An application sleeps until something happens, so "
	  "anything that changes the screen without an input event behind it — a "
	  "timer, a finished load — has to say so here." },
	{ "app.showTray",
	  "Puts the application's icon in the system tray (the menu bar on macOS) "
	  "with a tooltip. Called again while it is up, it only changes the tooltip. "
	  "The icon is the one the export generated, so the tray and the window agree "
	  "about what this application looks like." },
	{ "app.hideTray",
	  "Takes the tray icon away again. It also goes when the application exits — "
	  "an icon left behind by a program that is gone is the worst thing a tray "
	  "can do — so this is for hiding it while still running." },
	{ "app.addTrayItem",
	  "Adds an entry to the tray menu, in order. The ID is what On Tray Item "
	  "receives when somebody chooses it, and the label is what they read: "
	  "translating the menu then cannot quietly rewire what its entries do." },
	{ "app.addMenu",
	  "Adds a menu to the application's menu bar, in order. The id is how Add "
	  "Menu Item names it afterwards; the label is what is drawn in the strip. "
	  "Adding the same id twice is one menu, not two. On macOS the same menus go "
	  "into the SYSTEM bar next to the Apple symbol instead, and the strip in the "
	  "window is not drawn at all — a page that leaves room for the bar gets that "
	  "room back there." },
	{ "app.addMenuItem",
	  "Adds an entry to the menu with that id. Choosing it fires On Menu Item "
	  "with the ENTRY's id — the label is only what somebody reads, so "
	  "translating the menu leaves what it does alone. Shortcut is the chord "
	  "that chooses it without opening the menu, written the way people write "
	  "one: \"Ctrl+Shift+S\", \"F5\", \"Alt+Left\". It fires the same On Menu "
	  "Item, because it is the same entry answered a faster way. Ctrl means "
	  "Command on a Mac, so a chord is written once and is right on every "
	  "platform; leave it empty for an entry that has none." },
	{ "app.addMenuSeparator",
	  "Adds a dividing line to a menu. It carries no id and cannot be chosen: it "
	  "is there to group the entries above and below it." },
	{ "app.notify",
	  "Puts a banner in the system's notification centre — what an application "
	  "says when it has finished something nobody is watching any more. It is "
	  "not a dialog: nothing waits for it and nothing comes back. True means the "
	  "system took it, not that somebody read it; whether it appears is the "
	  "system's decision (Do Not Disturb, this app's own switch). On macOS it "
	  "needs the packaged app — a game runtime started straight from a build "
	  "folder has no identity to post as." },
	{ "app.notifyAvailable",
	  "Can this program show notifications at all? False in the editor preview, "
	  "and on a Linux without notify-send. Ask once instead of finding out per "
	  "notification." },
	{ "app.setMenuItemEnabled",
	  "Greys a menu entry out, or brings it back. Addressed by the ENTRY's id — "
	  "the same id On Menu Item carries — so an id used in two menus is one "
	  "command offered twice and both rows follow. A disabled row cannot be "
	  "chosen and its shortcut does nothing either, which is the half that "
	  "matters: a greyed-out Save whose Ctrl+S still saves has said one thing "
	  "and done another." },
	{ "app.setMenuItemChecked",
	  "Puts a mark beside a menu entry, or takes it away — for a row that names "
	  "a state (\"Show Toolbar\") rather than an action. Choosing the row still "
	  "fires On Menu Item and nothing else: whether the mark then moves is the "
	  "application's decision, because a menu that ticked itself would be wrong "
	  "the moment the command it named failed." },
	{ "app.menuItemEnabled",
	  "Whether that menu entry can be chosen right now. False for an id no row "
	  "carries." },
	{ "app.menuItemChecked",
	  "Whether that menu entry carries its mark right now — what a row bound to "
	  "a setting reads to flip it instead of remembering it." },
	{ "app.clearMenuBar",
	  "Removes the whole menu bar. A menu usually changes as a set, so the way "
	  "to change one is to clear it and build the new one." },
	{ "app.setAutostart",
	  "Arranges for this application to start when the user logs in, or takes "
	  "that back. It needs the project's \"Run other programs\" permission: it "
	  "asks the SYSTEM to run a program at every login, which is more than "
	  "running one now, not less." },
	{ "app.autostart",
	  "Whether this application is currently set to start at login. Reads what "
	  "the system was told, not what the application believes — so a checkbox "
	  "bound to it stays right even after somebody changed it elsewhere." },
	{ "app.clearTrayMenu",
	  "Removes every entry from the tray menu, leaving the icon in place. What a "
	  "menu offers usually depends on what the application is doing, and rebuilding "
	  "it is how that stays true." },

	// ── Clipboard ────────────────────────────────────────────────────────────
	{ "clipboard.getText",
	  "The text currently on the system clipboard, or an empty string when there "
	  "is none. The same clipboard Ctrl+C and Ctrl+V use in a text field." },
	{ "clipboard.setText",
	  "Puts text on the system clipboard, replacing whatever was there. Every "
	  "other application on the machine can then paste it." },
	{ "clipboard.hasText",
	  "True when the system clipboard currently holds text. Use it to grey out a "
	  "Paste button instead of pasting nothing." },

	// ── Dialogs ──────────────────────────────────────────────────────────────
	{ "dialog.message",
	  "Shows a native message box and waits until the user dismisses it. Kind is "
	  "0 for information, 1 for a warning, 2 for an error. Blocking on purpose: "
	  "it is for what must be read before anything else happens." },
	// ── JSON ─────────────────────────────────────────────────────────────────
	{ "json.getString",
	  "Reads a text value out of JSON. The path is dotted, with [i] for array "
	  "elements: \"user.name\", \"items[2].id\". Missing, wrong type or unparsable "
	  "text all give you the fallback." },
	{ "json.getNumber",
	  "Reads a number out of JSON at a dotted path. Missing, wrong type or "
	  "unparsable text all give you the fallback rather than an error." },
	{ "json.getBool",
	  "Reads a true/false value out of JSON at a dotted path. Falls back to the "
	  "given value when the path is missing or holds something else." },
	{ "json.has",
	  "True when the dotted path exists in the JSON text. Says nothing about what "
	  "type the value has, only that something is there." },
	{ "json.count",
	  "How many elements the array at this path has, or zero when the path is not "
	  "an array. Use it to walk items[0], items[1] without guessing the end." },
	{ "json.setString",
	  "Writes a text value into JSON and returns the whole document as new text. "
	  "Missing objects along the path are created; an empty input starts a new "
	  "document." },
	{ "json.setNumber",
	  "Writes a number into JSON at a dotted path and returns the whole document "
	  "as new text. Missing objects along the path are created." },
	{ "json.setBool",
	  "Writes a true/false value into JSON at a dotted path and returns the whole "
	  "document as new text. Missing objects along the path are created." },

	// ── Preferences ──────────────────────────────────────────────────────────
	{ "prefs.getString",
	  "Reads a saved setting as text, or the fallback when it was never set. "
	  "Preferences are small scraps like a last folder — not the save system, "
	  "which is shaped by a template and belongs to a game's progress." },
	{ "prefs.getNumber",
	  "Reads a saved setting as a number, or the fallback when it was never set "
	  "or holds something else." },
	{ "prefs.getBool",
	  "Reads a saved setting as true/false, or the fallback when it was never set. "
	  "The natural home for \"don't show this again\"." },
	{ "prefs.setString",
	  "Saves a text setting under a key. Written to disk immediately, so a crash "
	  "cannot cost more than the last change." },
	{ "prefs.setNumber",
	  "Saves a numeric setting under a key. Written to disk immediately." },
	{ "prefs.setBool",
	  "Saves a true/false setting under a key. Written to disk immediately." },
	{ "prefs.has",
	  "True when this key has ever been set. Lets a first run be told apart from "
	  "one where the user deliberately chose the default." },
	{ "prefs.remove",
	  "Forgets one setting, so the next read gets its fallback again. False when "
	  "there was nothing under that key." },
	{ "prefs.clear",
	  "Forgets every setting at once — what a \"reset to defaults\" button does." },

	// ── Date and time ────────────────────────────────────────────────────────
	{ "datetime.now",
	  "The current wall-clock time as seconds since 1970. This is the clock the "
	  "operating system shows, not the game clock: pausing does not stop it." },
	{ "datetime.format",
	  "Turns a time into text using a strftime pattern, in local time. "
	  "\"%Y-%m-%d %H:%M\" gives you 2026-08-27 14:32." },
	{ "datetime.year",   "The year of that time, in local time, as a full number like 2026." },
	{ "datetime.month",  "The month of that time in local time, 1 for January through 12." },
	{ "datetime.day",    "The day of the month of that time, in local time, from 1 to 31." },
	{ "datetime.hour",   "The hour of that time in local time, from 0 to 23." },
	{ "datetime.minute", "The minute of that time in local time, from 0 to 59." },
	{ "datetime.second", "The second of that time in local time, 0 to 60 (leap seconds)." },
	{ "datetime.weekday",
	  "The day of the week of that time in local time, with 0 for Sunday through "
	  "6 for Saturday." },

	{ "dialog.confirm",
	  "Asks a yes/no question in a native dialog and returns true for the first "
	  "button. Both labels are yours, so it can ask \"Save\" against \"Discard\" "
	  "rather than only ever Yes and No. Return takes the first button, Escape "
	  "the second." },
	{ "dialog.openFile",
	  "The system's own file picker. Returns the chosen path, or empty if the "
	  "person cancelled — cancelling is a normal answer, not a failure.\n\n"
	  "What comes back is ALLOWED to the file calls from then on, even in a "
	  "project that permits nothing outside its own folder: somebody choosing a "
	  "file is the permission. This is how an application opens a document.\n\n"
	  "Filter is a description and its extensions in one string, "
	  "\"Text files:txt;md\", or empty for anything." },
	{ "dialog.saveFile",
	  "The system's own \"save as\" picker. Same as Open File Dialog in every "
	  "other way, including that the path it returns becomes writable to the file "
	  "calls. The file usually does not exist yet — that is the point." },
	{ "dialog.pickFolder",
	  "Asks for a directory. Everything INSIDE it becomes reachable to the file "
	  "calls, not just the folder itself, which is what makes \"choose a workspace "
	  "folder\" work with no permission set anywhere." },

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
	{ "fs.isDir",
	  "Is there a DIRECTORY at this path? Exists answers about anything; this one "
	  "answers about the thing you can list." },
	{ "fs.size",
	  "The file's size in bytes, or -1 for anything that is not a readable file — "
	  "a directory included, since a folder's size is not a number one call can "
	  "honestly give." },
	{ "fs.modified",
	  "When the file was last written, in seconds, on the same clock Now uses. So "
	  "\"how old is this file\" is Now minus this, and not a second time format to "
	  "learn. -1 when there is nothing there." },
	{ "fs.list",
	  "The names of everything directly inside a directory, sorted. Names only, "
	  "not full paths — joining stays yours. Empty for a path that is not a "
	  "directory. Leave the path empty for the top of your own folder." },
	{ "fs.rename",
	  "Moves or renames, in one step. Both paths follow the same rules as every "
	  "other file call." },
	{ "fs.copy",
	  "Copies a file. It will NOT overwrite: if something is already at the "
	  "destination it does nothing and Ok is false. This is the one file call that "
	  "could destroy something you did not name, so it refuses instead." },
	{ "fs.watch",
	  "Keeps an eye on a file or a folder. From now on On File Changed fires "
	  "with the path whenever it appears, disappears or changes; for a folder "
	  "that means its immediate contents, one level deep. Returns a handle for "
	  "Stop Watching, or 0 when the path is out of reach. Checked about once a "
	  "second, so it is for reacting, not for timing." },
	{ "fs.unwatch",
	  "Stops a watch, using the handle Watch File gave you. Watches also all end "
	  "when the application does." },

	// ── Printing ─────────────────────────────────────────────────────────────
	{ "print.toPdf",
	  "Writes text as a PDF, at a path that follows the same rules as the file "
	  "nodes. It is set in Courier and laid out as a page of text: lines break "
	  "at your newlines and at the page width, pages break when they are full. "
	  "Needs the project's \"Read and write files\" permission." },
	{ "print.file",
	  "Hands a file to the system's printing. Ok means it was handed over, not "
	  "that it came out of a printer — what the queue does next is between the "
	  "user and their printer. Needs the \"Run other programs\" permission, "
	  "because that is what this is. Not available on Windows yet." },
	{ "print.available",
	  "Is there anything here to print with? Check it before offering a Print "
	  "button. It answers false on Windows, where printing still needs its own "
	  "piece of work." },

	// ── Database ─────────────────────────────────────────────────────────────
	// Open needs the project's "Read and write files" permission; a database is
	// a file. The readers do not.
	{ "db.open",
	  "Opens a SQLite database file, creating it if it is not there yet, and "
	  "gives you a handle for the other Database nodes. The path follows the "
	  "same rules as the file nodes: relative to your project, or somewhere the "
	  "user picked in a dialog. 0 means it did not open." },
	{ "db.close",
	  "Closes a database. They also all close when the application does." },
	{ "db.exec",
	  "Runs SQL that gives no rows back — CREATE, INSERT, UPDATE, DELETE. Put a "
	  "? where each value goes and pass the values as a JSON array in Params "
	  "(for example [\"Ada\", 36]); never paste them into the SQL yourself, or a "
	  "name with a quote in it becomes somebody else's command. Ok is false when "
	  "it failed, and Database Error says why." },
	{ "db.query",
	  "Runs a SELECT and gives the rows back as JSON — an array of objects, one "
	  "per row, which the JSON nodes read. Same ? and Params as Run SQL. An "
	  "empty result and a failed one both come back as [], so check Database "
	  "Error to tell them apart. Very large results are cut off (Database Error "
	  "says so); use LIMIT." },
	{ "db.changes",
	  "How many rows the last Run SQL on this database actually changed. This is "
	  "how you tell \"it worked\" from \"nothing matched\"." },
	{ "db.lastInsertId",
	  "The row id the last INSERT on this database created." },
	{ "db.lastError",
	  "Why the last call on this database failed, in SQLite's own words, or "
	  "empty when it did not." },

	// ── Timers ───────────────────────────────────────────────────────────────
	// None of these need a permission: a timer cannot reach anything, and what
	// it fires is this application's own graph.
	{ "timer.after",
	  "Fires On Timer once, after the seconds you give it, and hands back a "
	  "handle so you can tell it apart from other timers (and cancel it before "
	  "it goes off). Unlike Delay it does not park the graph: everything carries "
	  "on and the event arrives later. 0 means it did not start." },
	{ "timer.every",
	  "The same, but it keeps firing until you cancel it — a clock, an autosave, "
	  "a poll. If the application was busy or away it fires ONCE when it comes "
	  "back, not once for every tick it missed." },
	{ "timer.cancel",
	  "Stops a timer, using the handle it gave you. Ok is false when there was "
	  "no such timer running — it already fired, or it was cancelled before." },
	{ "timer.active",
	  "Is this timer still running? False for a one-shot that has already gone "
	  "off, for a cancelled one, and for a handle that never existed." },
	{ "timer.cancelAll",
	  "Stops every timer this application started. They also all stop when it "
	  "closes." },

	// ── Process ──────────────────────────────────────────────────────────────
	// All three need the project's "Run other programs" permission except Find
	// Program, which runs nothing.
	{ "process.run",
	  "Runs another program and WAITS for it. Arguments go in one at a time — this "
	  "is not a command line, so a path with a space in it needs no quoting and "
	  "cannot inject anything.\n\n"
	  "Four answers, because a caller who only wants to know whether it worked "
	  "reads Ok, and one who has to explain a failure to a person needs the exit "
	  "code and Err. A non-zero exit code is an ANSWER, not a breakage.\n\n"
	  "It blocks the frame, so give it a timeout you are willing to wait; left at "
	  "zero it is thirty seconds. Needs the project's \"Run other programs\" "
	  "permission." },
	{ "process.openUrl",
	  "Hands a web address (or a file) to whatever this desktop opens it with — a "
	  "browser, the file manager, the mail client. The usual way to show a manual "
	  "or a release page. Needs the project's \"Run other programs\" permission." },
	{ "process.which",
	  "Where the system would find this program, or empty if it would not find it "
	  "at all. Deliberately needs NO permission: asking whether something is "
	  "installed runs nothing, and it is how a script tells somebody what it "
	  "would need before they decide to allow it." },

	// ── HTTP ─────────────────────────────────────────────────────────────────
	{ "http.get",
	  "Starts a GET and returns straight away with a ticket number. The answer "
	  "arrives later as On Http Response, carrying that same ticket. Needs the "
	  "project's \"Network access\" permission; 0 means it never started." },
	{ "http.post",
	  "Starts a POST with this body. An empty Content Type means "
	  "application/json. Like Get it returns a ticket and answers later." },
	{ "http.done",
	  "Has this ticket been answered yet? Usually not needed — On Http Response "
	  "fires exactly when it turns true — but useful for a screen that shows "
	  "\"loading\" while it waits." },
	{ "http.ok",
	  "Did the request REACH the server and come back? This is about the "
	  "connection, not about the answer: a 404 is ok=true with status 404." },
	{ "http.status",
	  "The HTTP status code, 200 for the ordinary success. 0 when the request "
	  "never got an answer, and then Response Error says why." },
	{ "http.body",
	  "What came back, as text. Feed it to the JSON nodes when it is JSON." },
	{ "http.error",
	  "Why the request failed to reach anybody (no network, bad host, timed "
	  "out). Empty when Response OK is true." },
	{ "http.forget",
	  "Drops a response you are finished with. Optional: the engine keeps the "
	  "last 32 and forgets the oldest by itself." },
	{ "http.available",
	  "Does this build have a network stack at all? False only on a Linux build "
	  "made without libcurl, where every request would fail — worth saying out "
	  "loud once instead of failing per request." },

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
	{ "string.equals",
	  "Are the two texts the same? Exact and case-sensitive — put To Lower on "
	  "both sides when case should not matter. This is what to branch on after "
	  "On Menu Item or On Tray Item: the Equals node next door compares NUMBERS, "
	  "and two texts arriving there both count as 0, so every id would match "
	  "every other one." },
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
