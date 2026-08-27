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
	  "nothing else — add what it needs with the other calls, or spawn a class "
	  "with Create Object instead, which comes furnished." },
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
	{ "app.requestRedraw",
	  "Draws one more frame. An application sleeps until something happens, so "
	  "anything that changes the screen without an input event behind it — a "
	  "timer, a finished load — has to say so here." },

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
	{ "time.setTimeScale",
	  "Slows down or speeds up game time: 0.5 is half speed, 0 freezes everything "
	  "that uses Delta Time, 1 is normal. Real-time Delay nodes and Unscaled Delta "
	  "Time are deliberately unaffected." },
	{ "time.timeScale",
	  "The current time scale." },

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
	  "makes strafing and walking backwards work." },
	{ "camera.getTargetYawMode",
	  "0 Free (the character faces where it walks), 1 Follow (it turns with the "
	  "camera)." },
	{ "camera.getRigYaw",
	  "Which way the rig is looking, in degrees." },
	{ "camera.getRigPitch",
	  "How far up or down the rig is looking, in degrees." },
	{ "camera.addYawPitch",
	  "Turns the rig by these degrees, clamped to the pitch limits the rig "
	  "carries. This is what a look input drives — adding to the angles rather "
	  "than setting them is what keeps two input sources from fighting." },

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
