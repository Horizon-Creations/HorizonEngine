#include "../EditorGuides.h"

// ── First steps ──────────────────────────────────────────────────────────────
// The three pages someone needs before anything else can be built: a character
// they can walk around with, the input that drives it, and the choice of
// language they will be stuck with for the rest of the project.
//
// The player page is the one everything else leans on. It is longer than it
// looks like it should be, and that is the honest shape of the engine today:
// there is no template project with a player already in it, so the first hour
// is spent assembling one. Where a step exists only because the engine does not
// do it for you yet, the page says so instead of pretending it is a design.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> firstStepsPages()
{
	std::vector<Docs::Page> out;

	// ── Your first playable scene ────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A scene you can press Play in and walk around: ground you stand on, "
			     "a character that moves with the keyboard, and a camera that follows "
			     "it."),
			para("Six steps. The order matters in one place only, and it is called out "
			     "there. Everything else can be done whenever."),
			note("There is no starter project yet", {
				para("None of the project presets ship a player. That is why this page "
				     "exists and why it is this long — you are assembling by hand what "
				     "a template would normally hand you. It is worth doing once "
				     "carefully: the result is a scene you can copy for every later "
				     "project."),
			}),
		}));

		secs.push_back(section("ground", "Step 1", "Ground you do not fall through", {
			lead("A floor needs two separate things, and having only the first is the "
			     "single most common way to spend twenty minutes on nothing."),
			steps({
				{ "Create the floor",
				  "A Landscape, or an entity with Add Component -> Mesh scaled flat. "
				  "Either works." },
				{ "Add Component -> Rigid Body",
				  "Set Body Type to Static. This is what makes it solid." },
				{ "Add Component -> Collider",
				  "Box for a flat mesh. A Landscape does not need one: terrain builds "
				  "its collision from its own height field." },
			}),
			warn("A mesh you can see is not a mesh you can stand on", {
				para("Visibility and collision are unrelated. A floor with no Rigid "
				     "Body renders exactly the same and the character falls straight "
				     "through it, with nothing in the log. If your character drops out "
				     "of the world on the first frame, this is why."),
			}),
		}));

		secs.push_back(section("classes", "Step 2", "The two classes a player is made of",
		{
			lead("A player is two objects, not one: the CONTROLLER is the person "
			     "holding the pad, the CHARACTER is the body in the world. Splitting "
			     "them is what lets one player take over a different body later."),
			steps({
				{ "In the Content Browser, right-click -> Player Controller",
				  "Creates a HorizonCode class that already derives from "
				  "PlayerController. Do not build it from a plain HorizonCode Class "
				  "and set the base yourself; this entry gets it right." },
				{ "Right-click -> Player Character",
				  "The same, deriving from PlayerCharacter." },
			}),
			note("Both entries appear in a HorizonCode project", {
				para("In a Lua, Python or C++ project the Content Browser offers "
				     "Script or C++ Class instead. The two classes still exist as "
				     "concepts — you write them in your language — but the ready-made "
				     "entries are HorizonCode's."),
			}),
			para("A Player Character arrives furnished. The engine gives it a Character "
			     "Controller, a capsule Collider, a kinematic Rigid Body, a Movement "
			     "component and a child camera with a Camera Rig, so you do not add any "
			     "of those by hand."),
		}));

		secs.push_back(section("spawn", "Step 3", "Spawn the character and take it over", {
			lead("The controller creates the character. This is the one ordering rule "
			     "on the page, and getting it wrong is the difference between a game "
			     "and an empty world."),
			steps({
				{ "Open the Player Controller class",
				  "Its graph is where the game starts." },
				{ "Add the Begin Play event",
				  "It runs once, when the controller comes into the world." },
				{ "Add Create Object",
				  "Pick your Player Character class in its title. Wire a Location if "
				  "you want it somewhere other than where the class was authored." },
				{ "Add Possess",
				  "Feed it the object Create Object returned. Now the input reaches "
				  "that character." },
			}),
			warn("A character placed in the scene by hand is not possessed", {
				para("Dragging a Player Character into the level gives you a body "
				     "standing there, and it will not move: nothing has taken it over. "
				     "Spawning from the controller's Begin Play is the path the engine "
				     "is built around. A level-placed character does get found and can "
				     "be possessed, but you still have to say so."),
			}),
			tip("Get Player Controller and Get Player Character work from anywhere", {
				para("Any graph can ask for them, which is how a door, a trigger or a "
				     "menu reaches the player without a wired reference. Cast the "
				     "result to your own class to get at its variables."),
			}),
		}));

		secs.push_back(section("camera", "Step 4", "A camera that follows", {
			lead("The Camera Rig is the follow camera. It comes with the Player "
			     "Character, so this step is usually just choosing how it behaves."),
			steps({
				{ "Select the character's camera child",
				  "Its Details panel has Camera Rig." },
				{ "Set Mode",
				  "First person sits at the eyes; third person swings out on an arm." },
				{ "Set Arm Length for third person",
				  "How far behind the character the camera sits." },
				{ "Leave Collide With World on",
				  "It pulls the camera in when a wall would otherwise be between it "
				  "and the character." },
			}),
			table({ "Field", "What it changes" }, {
				{ "Mode", "First or third person" },
				{ "Arm Length", "Distance behind the character in third person" },
				{ "Pivot Offset", "Where on the body the camera looks from" },
				{ "Sensitivity", "How far the view turns per unit of mouse movement" },
				{ "Pitch Min / Pitch Max", "How far up and down the view can tilt" },
				{ "Hide Target Mesh", "Hides the body in first person so you are not inside it" },
			}),
		}));

		secs.push_back(section("move", "Step 5", "Make it walk", {
			lead("Movement is one node per frame, fed from whatever your input says."),
			steps({
				{ "Open the Player Character class",
				  "Its Tick event is where movement belongs." },
				{ "Add Move",
				  "Its forward and right amounts are the direction, in the camera's "
				  "frame. Feed them from your input axes." },
				{ "Add Look",
				  "The same for turning the view, from the mouse or the right stick." },
				{ "Set Max Speed on the Movement component",
				  "Metres per second." },
			}),
			para("Is Grounded, Get Speed and Get Velocity read back what actually "
			     "happened, which is what an animation or a footstep sound keys off."),
			warn("Move does nothing without a Character Controller", {
				para("It steers a character; it does not push a rigid body. An entity "
				     "with only a Rigid Body ignores it silently. A Player Character "
				     "has one already — this only bites on an entity you assembled "
				     "yourself."),
			}),
		}));

		secs.push_back(section("play", "Step 6", "Press Play", {
			lead("If it works, you walk. If it does not, it is almost always one of "
			     "four things."),
			table({ "What you see", "What it is" }, {
				{ "The character falls forever", "The floor has no Rigid Body (step 1)" },
				{ "Nothing exists at all", "Nothing spawned the character (step 3)" },
				{ "It stands there, camera works", "No input is bound yet — see the input guide" },
				{ "It stands there, camera does not", "Nothing possessed it (step 3)" },
			}),
			tip("The console is faster than guessing", {
				para("Most silent failures do write a line. Open the console panel "
				     "before assuming nothing happened."),
			}),
		}));

		out.push_back(page("guides-first-player", "Your first playable scene",
		                   "Ground, a character, a camera and movement — a scene you "
		                   "can press Play in and walk around.",
		                   std::move(secs)));
	}

	// ── Input ────────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("WASD and a gamepad stick that both drive the same movement, without "
			     "the character knowing which one you used."),
			para("Input is two assets. An Input Action is a name for something the "
			     "player can do — \"Move\", \"Jump\". An Input Mapping Context says "
			     "which keys and buttons produce it. Your graph listens for the "
			     "action, so adding gamepad support later changes no logic."),
		}));

		secs.push_back(section("assets", "Step 1", "Create the two assets", {
			steps({
				{ "Content Browser -> Input Action",
				  "One per thing the player does. Its file name is the name your graph "
				  "listens for, so name it for the verb: Move, Look, Jump." },
				{ "Choose what kind it is",
				  "A button fires once; an axis carries a number; a 2D axis carries "
				  "two, which is what a movement stick is." },
				{ "Content Browser -> Input Mapping Context",
				  "One is enough for a whole game. It lists which keys and buttons "
				  "feed which action." },
				{ "Bind the keys",
				  "Search for a key or press it and let the editor detect it. Mouse "
				  "buttons bind here too." },
			}),
			note("Every mapping context in the project is loaded", {
				para("You do not activate one. The engine takes the union of all of "
				     "them at start, so a second context adds bindings rather than "
				     "replacing them."),
			}),
		}));

		secs.push_back(section("graph", "Step 2", "Listen for the action", {
			lead("An action arrives in the controller's graph as an event named after "
			     "the asset."),
			para("A button action fires on press. An axis action carries its value, "
			     "which for movement you pass straight into Move. Nothing in the "
			     "character needs to know whether it came from a key or a stick."),
			warn("The action name is the FILE name", {
				para("Renaming the asset renames the event. A graph listening for the "
				     "old name stops being called, and nothing reports it — the event "
				     "simply never fires again."),
			}),
		}));

		secs.push_back(section("modes", "Step 3", "Deciding who gets the input", {
			lead("When a menu is open, the game should stop reacting to clicks. That "
			     "is what the input modes are for."),
			table({ "Node", "What it does" }, {
				{ "Set Input Mode: Game Only", "UI is visible but receives nothing" },
				{ "Set Input Mode: Game and UI", "Both react; a click on a widget is consumed by it" },
				{ "Set Input Mode: UI Only", "The game receives nothing — pause menus" },
			}),
			para("Is Pointer Over UI answers the same question for a single case, when "
			     "you want to keep Game and UI but skip one particular click."),
		}));

		secs.push_back(section("raw", "Also", "Reading a key directly", {
			lead("Key Down, Mouse Button, Gamepad Button and Gamepad Axis read the "
			     "hardware without any asset in between."),
			para("Useful for a debug key or a prototype. Not what a shipping game "
			     "should be built on: the player cannot rebind it, and it ignores the "
			     "input modes above."),
		}));

		out.push_back(page("guides-input", "Input: actions and bindings",
		                   "Keyboard and gamepad driving the same movement, through "
		                   "actions your graph listens for.",
		                   std::move(secs)));
	}

	// ── Which language ───────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("choice", "Guide", "Which language to write in", {
			lead("A project picks one when it is created. The choice decides what the "
			     "Content Browser offers you and how much of the engine you can reach."),
			table({ "", "Reaches", "Good for" }, {
				{ "HorizonCode", "Everything, plus Delay and events",
				  "Gameplay. The only one with a wait node, and the only one the "
				  "ready-made Player Controller and Player Character entries exist for." },
				{ "Lua", "The scriptable groups",
				  "Quick logic and iteration, with hot reload in the editor." },
				{ "Python", "The scriptable groups",
				  "The same, when you would rather write Python." },
				{ "C++", "Everything, compiled",
				  "Systems work and anything performance-critical." },
			}),
			warn("The text languages cannot wait", {
				para("There is no Delay in Lua or Python. Anything on a timer has to "
				     "count down in an update function by hand, or live in a "
				     "HorizonCode class. This is the difference people hit first."),
			}),
			note("Not every group is reachable from a text script", {
				para("The engine API is grouped, and only some groups are exposed to "
				     "Lua and Python. Notably transform is not: a text script reads a "
				     "LOCAL position, and there is no world-space setter. If a script "
				     "needs world positions, either keep the entity unparented or put "
				     "that logic in a HorizonCode class, where Get World Position "
				     "exists."),
			}),
			tip("Mixing is allowed", {
				para("The Script component takes a HorizonCode class as readily as a "
				     ".lua or .py file, so one entity can be scripted one way and the "
				     "next another."),
			}),
		}));

		out.push_back(page("guides-language", "Which scripting language",
		                   "What each language reaches, and the two limits that decide "
		                   "the choice.",
		                   std::move(secs)));
	}

	return out;
}

} // namespace HE::Ed::Guides
