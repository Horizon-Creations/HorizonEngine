#include "../EditorGuides.h"

// ── From prototype to a game somebody else starts ────────────────────────────
// Interface, sound, saving, streaming and the export.
//
// The export page carries a warning that is only true for a while: builds made
// before the asset type index shipped start into an empty world. It is here
// because someone with an older build in a folder has no other way to find out.

namespace HE::Ed::Guides
{

std::vector<Docs::Page> shippingPages()
{
	std::vector<Docs::Page> out;

	// ── HUD and menus ────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A health bar that follows the game, and a menu that takes the input "
			     "while it is open."),
		}));

		secs.push_back(section("build", "Step 1", "Build the widget", {
			steps({
				{ "Content Browser -> UI Widget",
				  "Opens the widget editor." },
				{ "Lay out the elements",
				  "Anchors decide what happens when the window is resized — anchor a "
				  "health bar to a corner and it stays there at every resolution." },
				{ "Name the elements you will address",
				  "The name is how a graph finds them later." },
			}),
			para("A widget has its own graph, so simple behaviour — a button that "
			     "closes the menu — lives inside the widget rather than in the game."),
		}));

		secs.push_back(section("show", "Step 2", "Show it and drive it", {
			lead("Create Widget makes one; its title says which."),
			para("Set UI Text writes into a label, Set UI Visible shows and hides "
			     "parts, Set UI Color flashes something red on damage, Set UI Size "
			     "drives a bar. Set Widget Z-Order decides what sits on top when two "
			     "are open."),
			tip("Create it once, not every frame", {
				para("Begin Play is where a HUD is made. Creating one in Tick makes a "
				     "new widget every frame, and they stack up invisibly until the "
				     "frame rate falls over."),
			}),
		}));

		secs.push_back(section("input", "Step 3", "Who gets the input", {
			lead("A menu that does not stop the game is a menu the player fights."),
			table({ "Node", "For" }, {
				{ "Set Input Mode: Game Only", "Normal play. The HUD is visible and receives nothing" },
				{ "Set Input Mode: Game and UI", "A HUD with buttons. A click on a widget is consumed by it" },
				{ "Set Input Mode: UI Only", "Pause menus. The game receives nothing" },
			}),
			para("Set Cursor Visible goes with them: a mouse pointer belongs to a menu, "
			     "not to a shooter."),
		}));

		out.push_back(page("guides-ui", "A HUD and menus",
		                   "Interface that follows the game, and stops it fighting the "
		                   "player when a menu is open.",
		                   std::move(secs)));
	}

	// ── Sound ────────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("Sound that comes from where it happens, and a volume slider that "
			     "works."),
		}));

		secs.push_back(section("place", "Step 1", "Sound in the world", {
			steps({
				{ "Add Component -> Audio Source",
				  "On the thing making the noise — a torch, a machine." },
				{ "Set Asset ID",
				  "The sound to play." },
				{ "Tick Spatial",
				  "This is what makes it come from the entity instead of from "
				  "everywhere. Range and Rolloff Factor decide how fast it fades." },
				{ "Tick Loop and Play on Start for ambience",
				  "A torch crackles from the moment the scene loads." },
			}),
			note("Something has to be listening", {
				para("Add Component -> Audio Listener belongs on the camera. Without "
				     "one there is no position to be spatial relative to."),
			}),
		}));

		secs.push_back(section("script", "Step 2", "Sound from a graph", {
			table({ "Node", "For" }, {
				{ "Play Sound", "A one-shot with no position — UI clicks, music" },
				{ "Play Sound At", "A one-shot somewhere in the world — an impact" },
				{ "Stop Sound / Stop All Sounds", "Cutting it off, e.g. on a scene change" },
				{ "Is Sound Playing", "Not restarting something already running" },
			}),
			para("These work from Lua and Python as well as HorizonCode."),
		}));

		secs.push_back(section("buses", "Step 3", "Volume the player can set", {
			lead("Every source belongs to a Bus, and a bus is what an options menu "
			     "turns down."),
			para("Put effects, music and voice on separate buses, then Set Bus Volume "
			     "from a slider. Without buses the only volume is per source, and an "
			     "options screen would have to know about all of them."),
		}));

		out.push_back(page("guides-audio", "Sound",
		                   "Sound that comes from where it happens, on buses a menu can "
		                   "turn down.",
		                   std::move(secs)));
	}

	// ── Saving ───────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A checkpoint the player returns to, and a save they can come back to "
			     "tomorrow."),
		}));

		secs.push_back(section("template", "Step 1", "Say what a save contains", {
			steps({
				{ "Content Browser -> SaveGame Template",
				  "The shape of a save: which fields, of which types." },
				{ "Add the fields",
				  "Health, level, coins. Named, because that is how they are read back." },
			}),
			para("Create Save makes one, Write Save puts it on disk, Load Save brings "
			     "it back. Save Set Number and Save Get Number and their string, bool "
			     "and struct siblings move the values. List Saves and Save Exists are "
			     "what a load menu is built from."),
		}));

		secs.push_back(section("entities", "Step 2", "Saving where things are", {
			lead("Positions are not fields. They come from the entities themselves."),
			steps({
				{ "Add Component -> Save State",
				  "On anything whose place should survive — the player, a pushed crate." },
				{ "Tick Transform",
				  "Records where it is. Visibility records whether it was still there." },
				{ "Call Save Entity State at a checkpoint",
				  "Records the current state into the active save." },
				{ "Call Apply Saved State on load",
				  "Puts it back." },
			}),
			para("Apply Saved State moves the physics body too and clears its speed, so "
			     "a player restored mid-fall does not resume falling."),
			warn("A save is not a snapshot of the world", {
				para("What is saved is your template's fields plus the entities you "
					 "gave a Save State component. Everything else — what you spawned, "
				     "what you destroyed, which doors you opened — is not in there "
				     "unless you wrote it into a field yourself."),
			}),
		}));

		out.push_back(page("guides-saving", "Saving and checkpoints",
		                   "A checkpoint that puts the player back where they were, "
		                   "and what a save does not contain.",
		                   std::move(secs)));
	}

	// ── Streaming ────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A world larger than one scene, loaded in pieces as the player moves "
			     "through it."),
		}));

		secs.push_back(section("zones", "Step 1", "Load a piece", {
			para("Load Scene Additive brings a second scene in beside the current one "
			     "as a zone, at a position you choose. Unload Zone takes it away again, "
			     "and Show Zone and Hide Zone keep it loaded while it is out of sight — "
			     "cheaper than reloading it when the player turns round."),
			para("Load Scene replaces everything instead, which is what a level change "
			     "is. Has Pending Scene and Activate Loaded Scene let you prepare the "
			     "next level in the background and switch when it is ready, so the "
			     "screen does not freeze."),
			note("Zones bring their collision", {
				para("A streamed zone's floors are solid the moment it arrives, so the "
				     "player does not fall through what just loaded."),
			}),
		}));

		secs.push_back(section("limits", "Honestly", "What to watch", {
			warn("Memory only goes up", {
				para("Unloading a zone destroys its entities, but nothing releases the "
				     "assets they used. Walking a loop through the same zones raises "
				     "memory every lap and never lowers it. Fine for a demo, not for a "
				     "long session — worth knowing before a world is designed around "
				     "constant streaming."),
			}),
			para("Set Zone Position moves a loaded zone and rebuilds its collision at "
			     "the new place. A dynamic object inside it loses its speed in the "
			     "process."),
		}));

		out.push_back(page("guides-streaming", "A world bigger than one scene",
		                   "Zones loaded and unloaded as the player moves, and the one "
		                   "limit to design around.",
		                   std::move(secs)));
	}

	// ── Export ───────────────────────────────────────────────────────────────
	{
		std::vector<Docs::Section> secs;

		secs.push_back(section("what-you-get", "Guide", "What you end up with", {
			lead("A folder you can hand to somebody else, which starts without the "
			     "editor."),
		}));

		secs.push_back(section("export", "Step 1", "Export", {
			steps({
				{ "Open the export dialog",
				  "Pick the scene the game starts in." },
				{ "Choose the graphics settings that ship with it",
				  "Resolution, window mode, backend and the effects. They travel beside "
				  "the game and are what the player gets on first launch." },
				{ "Export",
				  "Assets are packed into an archive next to the executable." },
			}),
			warn("Re-export anything you built before this build", {
				para("Older archives carry no asset type index. A game packaged without "
				     "one cannot find its Player Controller class at startup — so "
				     "nothing spawns the character and the game opens on an empty "
				     "world. Exporting again is the whole fix; nothing in the project "
				     "needs changing."),
			}),
		}));

		secs.push_back(section("differs", "Step 2", "Where the build differs from Play", {
			lead("Play mode is not a perfect preview. These are the differences worth "
			     "knowing before you blame the build."),
			bullets({
				"The editor keeps a project folder beside it; the packaged game has "
				"only its archive. Anything found by scanning folders behaves "
				"differently.",
				"The editor tick order is not the game's, so anything timing-sensitive "
				"can feel slightly different.",
				"Undo, the console panel and every editor gizmo are gone. A game that "
				"leaned on one of those for diagnosis has none in the build.",
				"A scene switch behaves differently: the editor refuses some of it "
				"during Play, the game does it for real.",
			}),
			tip("Test the export early and often", {
				para("The first export of a project is where the differences surface, "
				     "and they are cheaper to fix in week one than in the last week. "
				     "Export once as soon as a character walks."),
			}),
		}));

		secs.push_back(section("player", "Step 3", "What the player cannot change yet", {
			lead("Worth planning around."),
			para("The graphics settings that ship are the ones you chose. There is no "
			     "options API yet, so a resolution or volume menu cannot write them "
			     "back at runtime, and key bindings cannot be remapped by the player — "
			     "the rebinding interface is the editor's. Nor is there a console, an "
			     "FPS display or a command line in the build: if a tester says it "
			     "stutters, the log file is what you have."),
		}));

		out.push_back(page("guides-export", "Exporting your game",
		                   "A folder somebody else can run, and the ways it differs "
		                   "from pressing Play.",
		                   std::move(secs)));
	}

	return out;
}

} // namespace HE::Ed::Guides
