#include "TutorialSteps.h"
#include <array>
#include <cmath>
#include <iterator>   // std::size

namespace HE::tut
{

// ─── Component names ─────────────────────────────────────────────────────────
namespace
{
	// Index-parallel to Comp; the spelling matches the .hescene component keys.
	constexpr std::array<const char*, static_cast<size_t>(Comp::Count)> kCompNames = {
		"mesh", "material", "light", "rigidbody", "collider", "particlesystem",
		"script", "terrain", "foliage", "navmesh", "camera", "audiosource",
		"animator", "uicanvas",
	};

	// Index-parallel to Asset.
	constexpr std::array<const char*, static_cast<size_t>(Asset::Count)> kAssetNames = {
		"material", "materialfunction", "particlesystem", "widget",
		"animatorstatemachine", "inputaction", "inputmappingcontext",
		"horizoncodeclass", "scene", "texture", "staticmesh", "skeletalmesh",
		"script", "audio", "font", "prefab", "animationclip",
		"theme", "struct", "enum", "savegametemplate", "bonemask", "blendspace",
		"propertyanimclip", "sequence",
	};
}

const char* compName(Comp c)
{
	return c < Comp::Count ? kCompNames[static_cast<size_t>(c)] : "";
}

Comp compFromString(std::string_view s)
{
	for (size_t i = 0; i < kCompNames.size(); ++i)
		if (s == kCompNames[i]) return static_cast<Comp>(i);
	return Comp::Count;
}

const char* assetName(Asset a)
{
	return a < Asset::Count ? kAssetNames[static_cast<size_t>(a)] : "";
}

Asset assetFromString(std::string_view s)
{
	for (size_t i = 0; i < kAssetNames.size(); ++i)
		if (s == kAssetNames[i]) return static_cast<Asset>(i);
	return Asset::Count;
}

bool wantsWindowOpened(Check c)
{
	switch (c)
	{
	case Check::PreferencesOpen:
	case Check::ProfilerOpen:
	case Check::EnvironmentOpen:
	case Check::ExportOpen:
		return true;
	default:
		return false;
	}
}

bool windowOpenIn(Check c, const Signals& s)
{
	switch (c)
	{
	case Check::PreferencesOpen:  return s.preferencesOpen;
	case Check::ProfilerOpen:     return s.profilerOpen;
	case Check::EnvironmentOpen:  return s.environmentOpen;
	case Check::ExportOpen:       return s.exportOpen;
	default:                      return false;
	}
}

// ─── '|'-separated lists (focusWindow, PanelsVisited's arg) ──────────────────
int listEntryCount(std::string_view list)
{
	if (list.empty()) return 0;
	int n = 1;
	for (char c : list) if (c == '|') ++n;
	return n;
}

std::string_view listEntry(std::string_view list, int index)
{
	if (index < 0) return {};
	size_t start = 0;
	for (int i = 0; ; ++i)
	{
		const size_t bar = list.find('|', start);
		const size_t end = (bar == std::string_view::npos) ? list.size() : bar;
		if (i == index) return list.substr(start, end - start);
		if (bar == std::string_view::npos) return {};
		start = bar + 1;
	}
}

bool panelVisited(std::string_view name, std::string_view visited)
{
	if (name.empty()) return false;
	// `visited` is '\n'-delimited AND '\n'-terminated, so searching for the name
	// wrapped in newlines matches whole entries only ("Scene" must not be
	// satisfied by "Scene Settings"). The haystack gets a leading newline so the
	// first entry can match too.
	std::string needle;
	needle.reserve(name.size() + 2);
	needle += '\n';
	needle.append(name);
	needle += '\n';
	std::string hay;
	hay.reserve(visited.size() + 1);
	hay += '\n';
	hay.append(visited);
	return hay.find(needle) != std::string::npos;
}

bool listEntryVisited(std::string_view list, int index, std::string_view visited)
{
	return panelVisited(listEntry(list, index), visited);
}

// ─── The curriculum ──────────────────────────────────────────────────────────
// One chapter per subsystem, in the order someone actually builds a level: look
// around, place something, give it a surface, light it, make it move, make it
// playable, ship it. Every step names the exact affordance it is talking about —
// a tour that says "somewhere in the editor" is worse than no tour.
//
// Every step is observed (see the header): the card advances when the editor sees
// the action, not when the user clicks past it. The few cards with nothing to do
// in the editor use Check::ReadAck, which unlocks its button once the body has
// been read to the end.
//
// `focusWindow` must name windows EditorUI actually opens — the highlight is a
// FindWindowByName lookup and a typo silently highlights nothing. The set is
// asserted in tests/test_tutorial.cpp; add a name there when you add one here.

namespace
{

// ── 1. Orientation ──
constexpr Step kOrientation[] = {
	{ "welcome",
	  "Welcome to Horizon Engine",
	  "This tour walks once through every part of the editor — scenes, assets, "
	  "materials, terrain, physics, animation, UI, scripting, playing and shipping. "
	  "It follows the order a level is actually built in: find your way around, "
	  "place something, give it a surface, light it, make it move, make it playable, "
	  "ship it. No earlier engine experience is assumed.\n"
	  "Each card first explains one idea — what the thing is and why you would want "
	  "it — and then asks for exactly one action, in the line under the text. You do "
	  "it in the real editor, not in a special tutorial mode, and the card watches "
	  "for it: once the editor has seen you do it, the card confirms and moves on by "
	  "itself. Nothing is skipped past, so when the tour says a chapter is done you "
	  "really have used that part of the editor.\n"
	  "A few cards, like this one, have nothing to do. Their button unlocks once you "
	  "have scrolled to the end of the text.\n"
	  "A pulsing outline marks the panel a card is talking about. The card moves out "
	  "of that panel's way on its own; if it still sits where you want to look, drag "
	  "it somewhere else by its title bar.\n"
	  "Back returns to the previous card when you want to read something again. "
	  "Close the window whenever you like and reopen it from Help - Interactive "
	  "Tutorial; your place is remembered. The sandbox is an ordinary project, so "
	  "whatever you build along the way stays yours.",
	  "", "", Check::ReadAck, "" },

	{ "layout",
	  "The editor at a glance",
	  "Every panel has one job, and nearly every task in the rest of the tour starts "
	  "in one of the four outlined ones. Knowing which panel to look at is most of "
	  "learning the editor.\n"
	  "Scene (centre) renders the world you are editing. It is where you move the "
	  "camera and drag things into place.\n"
	  "World Outliner (right) lists every entity in the open scene as a tree. When "
	  "something is small, hidden or hard to click in the viewport, select it here.\n"
	  "Details (below it) edits whatever is selected: its position, its components "
	  "and their settings. Nearly every change that is not a drag happens here.\n"
	  "Content Browser (bottom) is your project's asset library: meshes, materials, "
	  "textures, scripts, other scenes. A scene points at these files rather than "
	  "copying them, so changing an asset changes it everywhere it is used.\n"
	  "Quick Settings (left) holds the engine switches you pinned, and the tab bar "
	  "above the viewport is where asset editors open next to the scene.\n"
	  "Panels are docked, not fixed: drag a tab to rearrange them, and Window - Reset "
	  "Layout puts everything back. Hover almost any control for a tooltip; pressing "
	  "F1 while it shows opens the manual at that entry.\n"
	  "Each outline dims once you have clicked into its panel, and the line under "
	  "this text counts how many you have visited so far.",
	  "Click into each of the four highlighted panels once.",
	  "Scene|World Outliner|Details|Content Browser",
	  Check::PanelsVisited, "Scene|World Outliner|Details|Content Browser" },
};

// ── 2. Viewport navigation ──
constexpr Step kViewport[] = {
	{ "fly",
	  "Flying through the scene",
	  "Before you can work on a scene you have to be able to reach every part of it, "
	  "so the next two cards are about the camera. The one you steer here belongs "
	  "to the editor: it is not part of the level and never shows up in the game, "
	  "so moving it cannot break anything. Look around freely.\n"
	  "Hold the right mouse button inside the Scene view and steer with the mouse. "
	  "While it is held, W/A/S/D move, Q/E drop and rise, and Shift moves four times "
	  "faster. Let go and those keys go back to their usual jobs — W, E and R switch "
	  "the gizmo, which the Entities chapter shows.\n"
	  "The mouse is captured while you look around, so the cursor will not run off "
	  "the viewport.",
	  "Right-drag in the Scene view, then fly with W/A/S/D.",
	  "Scene", Check::CameraFlown, "" },

	{ "orbit",
	  "Orbit, pan, zoom, focus",
	  "Flying covers distance. Once you have arrived you usually want to look at one "
	  "thing from all sides, and that is what orbiting is for: the camera circles a "
	  "pivot point in front of it instead of turning on the spot.\n"
	  "Alt + left mouse orbits around the pivot, the middle mouse button pans "
	  "sideways and up and down, and the wheel moves the camera forward and back "
	  "along its view, towards the pivot or away from it.\n"
	  "Select something and press F with the mouse over the Scene view to frame it: "
	  "the camera moves until the selection fills the view, and the selection "
	  "becomes the new pivot. It is the fastest way back when you have flown off "
	  "into the sky, and the usual first move before orbiting around an object.\n"
	  "If flying feels too slow or too fast for the size of your world, Camera Speed "
	  "is on the Viewport page of Edit - Preferences and next to the camera icon in "
	  "the Scene toolbar.",
	  "Roll the mouse wheel in the Scene view to zoom in or out.",
	  "Scene", Check::CameraZoomed, "" },
};

// ── 3. Entities ──
constexpr Step kEntities[] = {
	{ "outliner",
	  "The World Outliner",
	  "A scene is a tree of entities. Everything you can place is one: meshes, "
	  "lights, cameras, the Sky, spawn points, invisible logic holders.\n"
	  "Parenting matters — children inherit their parent's transform, so moving a "
	  "parent moves the whole group.",
	  "Click an entity in the World Outliner to select it.",
	  "World Outliner", Check::SelectionChanged, "" },

	{ "create-entity",
	  "Create an entity",
	  "Right-click empty space in the World Outliner for Create Entity, or "
	  "right-click an existing entity for Create Child Entity.\n"
	  "New entities start with nothing but a name and a transform — what they *are* "
	  "is decided by the components you add next.",
	  "Right-click in the World Outliner and choose Create Entity.",
	  "World Outliner", Check::EntityAdded, "" },

	{ "gizmo",
	  "Move, rotate, scale",
	  "With an entity selected, the gizmo appears in the viewport. W switches to "
	  "move, E to rotate, R to scale — the same three buttons sit in the Scene "
	  "toolbar.\n"
	  "Every drag is undoable, and the numbers underneath are editable in the "
	  "Details panel if you need exact values.",
	  "Drag the gizmo, then press Ctrl/Cmd+Z to undo it.",
	  "Scene", Check::UndoUsed, "" },
};

// ── 4. Components ──
constexpr Step kComponents[] = {
	{ "add-mesh",
	  "Components make an entity something",
	  "The Details panel shows the components on the selected entity and has an Add "
	  "Component button at the bottom.\n"
	  "A Mesh component gives the entity geometry; the engine's primitives (Cube, "
	  "Sphere, Plane, …) live under the Engine root of the Content Browser and can "
	  "be picked straight from the component's asset field.",
	  "Select your entity and add a Mesh component in Details.",
	  "Details", Check::ComponentAdded, "mesh" },

	{ "add-material",
	  "Give it a surface",
	  "A Material component decides how the mesh is shaded. Without one the engine "
	  "falls back to a neutral default.\n"
	  "One material asset can be shared by hundreds of entities — per-entity tweaks "
	  "go into the parameter overrides on this component, not into a copy of the "
	  "material.",
	  "Add a Material component to the same entity.",
	  "Details", Check::ComponentAdded, "material" },

	{ "save-scene",
	  "Save the scene",
	  "Scenes are files (.hescene) under your project's Content folder. The title "
	  "and the tab show an asterisk while there are unsaved edits.\n"
	  "Ctrl/Cmd+S saves whichever tab is in front — here the scene, in a material "
	  "tab that material. Ctrl/Cmd+Shift+S saves everything that is unsaved, "
	  "Ctrl/Cmd+Alt+S saves the scene under a new name. The scene marked "
	  "as the startup scene in your export profile is the one a packaged build "
	  "opens first.",
	  "Press Ctrl/Cmd+S to save the scene.",
	  "", Check::SceneSaved, "" },
};

// ── 5. Content Browser ──
constexpr Step kContent[] = {
	{ "roots",
	  "Content, Engine and Source",
	  "The Content Browser shows up to three roots: Content (your project), Engine "
	  "(the primitives and materials shipped with the editor, read-only unless you "
	  "override them) and — in C++ projects — Source.\n"
	  "Every engine asset is a .hasset file: one container format with a type tag, "
	  "which is why the browser groups by icon rather than by extension.",
	  "Switch the Content Browser to the Engine root and look at the built-in meshes.",
	  "Content Browser", Check::ContentRootShown, "engine" },

	{ "import",
	  "Import your own assets",
	  "Assets - Import Asset (or dragging files in) brings in glTF/GLB, FBX, OBJ "
	  "and COLLADA models, PNG/JPG/TGA/HDR textures, WAV/OGG audio and fonts. "
	  "Importing converts them to .hasset once; the editor never re-reads the "
	  "original at runtime.\n"
	  "A glTF with a skin is imported as a skeletal mesh plus its animation clips, "
	  "everything else - a rigged FBX included - as a static mesh.",
	  "Open Assets - Import Asset. Cancelling the file dialog is fine.",
	  "Content Browser", Check::ImportOpened, "" },

	{ "create-asset",
	  "Create an asset",
	  "Right-click in the Content Browser and open Create Asset. Scenes and UI "
	  "widgets sit at the top; the rest is grouped by what it belongs to: Gameplay "
	  "(your logic, plus the animation assets), Input, Rendering (materials and "
	  "particle systems) and Data (structs, enums, savegame templates, themes).\n"
	  "Only what the editor can actually author is on that list. Meshes, textures, "
	  "audio and fonts arrive through Import, which is why the menu ends with a "
	  "note saying so. The Gameplay group follows the project's scripting "
	  "language — a Lua project offers Lua scripts, a HorizonCode project offers "
	  "node classes.",
	  "Right-click in the Content Browser and create any asset.",
	  "Content Browser", Check::AssetAdded, "" },
};

// ── 6. The asset family ──
// Every kind of .hasset gets named here, and the ones the Create Asset menu can
// make are actually made — reading a list of asset types teaches nothing about
// where they live or what opens them. A kind that belongs to a later chapter is
// made THERE, next to the thing it feeds: materials and material functions in
// Materials, particle systems in Particles, state machines, bone masks, blend
// spaces, property clips and sequences in Animation, widgets and themes in UI,
// input assets in Gameplay logic. What is left — scenes, prefabs, the data types
// and the import-only media — is made or explained here.
//
// The Create menu has offered no Texture or Static Mesh since the empty stubs it
// wrote were dropped (they could never hold anything). So those two cards point
// at the importer and at the Engine root's built-in meshes instead; their ids
// stay, because a saved position on a vanished id ends the tour.
constexpr Step kAssetTypes[] = {
	{ "asset-anatomy",
	  "What an asset is",
	  "Almost everything in a project is a .hasset: one container format with a "
	  "type tag and a UUID, which is why the browser groups by icon rather than by "
	  "extension. Scenes (.hescene) are the one exception, and only because it "
	  "helps to spot them in a file manager.\n"
	  "The UUID is the identity. Renaming or moving an asset does not break the "
	  "things pointing at it; the reference is to the id, and the path is only how "
	  "you find it.\n"
	  "Two ways in: Create Asset makes an empty one of a kind the editor can "
	  "author, Import Asset converts a file from outside. This chapter makes the "
	  "kinds that stand on their own and explains the imported ones; the kinds "
	  "that feed a material, an animation or a widget are made in those chapters, "
	  "right where they are used.",
	  "", "Content Browser", Check::ReadAck, "" },

	{ "asset-scene",
	  "Scenes",
	  "A scene is an asset like any other: a tree of entities with their "
	  "components, saved as .hescene. A project has as many as you like — a menu "
	  "level, a test box, the real thing.\n"
	  "One is marked as the startup scene in each export profile; that is the one a "
	  "packaged build opens. Scenes can also be loaded additively on top of another, "
	  "which is how a streamed world or a persistent HUD level is built.",
	  "Create a Scene asset in the Content Browser.",
	  "Content Browser", Check::AssetOfTypeAdded, "scene" },

	// Not on the Create menu: a prefab is made FROM something, so it is made where
	// that something lives.
	{ "asset-prefab",
	  "Prefabs",
	  "A Prefab is an entity with its children and components, saved as an asset "
	  "so you can place it again — a lamp post, a pickup, an enemy with its "
	  "weapon. You do not make one from the Create menu but from an entity that "
	  "already looks right.\n"
	  "Save as Prefab writes it to Content/Prefabs and links the entity you saved "
	  "it from to that asset, so it becomes the first instance.",
	  "Right-click an entity in the World Outliner and choose Save as Prefab.",
	  "World Outliner", Check::AssetOfTypeAdded, "prefab" },

	{ "asset-texture",
	  "Textures",
	  "Texture assets hold the pixels plus how they are meant to be read: colour "
	  "textures are sRGB, normal and roughness maps are linear, and getting that "
	  "wrong is the usual cause of a material that looks washed out.\n"
	  "The editor does not paint pixels, so there is no empty texture to create: "
	  "Import brings in PNG/JPG/TGA/HDR, and double-clicking the result opens the "
	  "texture viewer. A material's Texture node points at it, and HDR images are "
	  "what an image-based sky lighting setup wants.\n"
	  "The sandbox ships without textures, so this card is reading only.",
	  "", "Content Browser", Check::ReadAck, "" },

	{ "asset-mesh",
	  "Static and skeletal meshes",
	  "A Static Mesh is geometry with no skeleton: props, terrain rocks, "
	  "architecture. Its editor tab previews it and shows the LOD chain the "
	  "renderer picks from by screen size, and identical meshes are drawn as GPU "
	  "instances rather than one draw call each.\n"
	  "A Skeletal Mesh is the same thing plus a rig, and carries its Animation "
	  "Clips. Both usually arrive through Import from a glTF/GLB — whether the file "
	  "has a skin decides which one you get. FBX, OBJ and COLLADA always give a "
	  "Static Mesh.\n"
	  "You do not need a file of your own to look at one: the Engine root ships "
	  "the built-in primitives (cube, sphere, plane, cylinder and friends) as "
	  "ordinary Static Mesh assets.",
	  "Switch the Content Browser to the Engine root, open Meshes and double-click "
	  "Cube.",
	  "Content Browser", Check::TabOfTypeOpened, "staticmesh" },

	{ "asset-audio",
	  "Audio",
	  "Sound arrives the same way: Import turns a WAV or OGG file into an Audio "
	  "asset, and double-clicking it opens a tab where you can audition it. A raw "
	  ".wav or .ogg dropped into the project opens there too, so you can listen "
	  "before you decide to import it.\n"
	  "Nothing plays on its own. An Audio Source component on an entity plays an "
	  "Audio asset; switch on Spatial and it is heard from where the entity stands, "
	  "quieter with distance. The Audio Listener component is the ears, normally "
	  "on the camera or the player.",
	  "Add an Audio Source component to an entity.",
	  "Details", Check::ComponentAdded, "audiosource" },

	{ "asset-imported",
	  "The other imported kinds",
	  "Three more families exist that the editor does not author, only convert or "
	  "receive:\n"
	  "Font — TTF, used by UI widget text and the in-game UI.\n"
	  "Animation Clip — comes along with a rigged glTF; states in an Animator "
	  "State Machine and the points of a Blend Space reference these.\n"
	  "Shader — hand-written shader code, for the cases the material graph cannot "
	  "express.\n"
	  "Like textures and meshes, they show up in the Content Browser by their icon "
	  "once imported, and the browser's type filter lists every kind in the "
	  "project, so you can always ask it for \"all fonts\".",
	  "", "Content Browser", Check::ReadAck, "" },

	// ── The Data group of the Create menu ──
	// Language-neutral on purpose: HorizonCode graphs, Lua/Python constants and the
	// C++ codegen all read the same type definitions, so these steps work in a
	// project of any scripting language.
	{ "asset-struct",
	  "Structs",
	  "A Struct asset defines a type of your own: named, typed fields with default "
	  "values — a weapon's damage, range and ammo, an item's name and price. Once "
	  "saved it shows up in the editor's type dropdowns, so a graph variable or a "
	  "savegame field can be of that type.\n"
	  "It is data, not code: the same definition is what the C++ export generates "
	  "a real struct from.",
	  "Create Asset - Data - Struct, then double-click it to open its editor.",
	  "Content Browser", Check::TabOfTypeOpened, "struct" },

	{ "asset-enum",
	  "Enums",
	  "An Enum asset is a named list of choices — Idle, Patrol, Chase — stored as "
	  "numbers but always shown by name. Anywhere a value of that type is edited, "
	  "the editor offers a dropdown instead of a number field, so a typo cannot "
	  "produce a fourth state nobody handles.",
	  "Create Asset - Data - Enum, then double-click it to open it.",
	  "Content Browser", Check::TabOfTypeOpened, "enum" },

	{ "asset-savegame",
	  "Savegame templates",
	  "A SaveGame Template is the shape of a save file: which fields a save "
	  "holds, their types (structs and enums included) and their defaults. "
	  "Gameplay writes and reads saves through it, whatever the project's "
	  "scripting language.\n"
	  "Keeping the schema an asset rather than ad-hoc code is what lets the editor "
	  "show you what a save contains.",
	  "Create Asset - Data - SaveGame Template and double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "savegametemplate" },
};

// ── 7. Materials ──
constexpr Step kMaterials[] = {
	{ "material-graph",
	  "Materials are node graphs",
	  "Double-click a material asset and it opens as a tab next to the Scene, not "
	  "as a modal window — you can keep the viewport visible while you work.\n"
	  "A material is a graph: constants, textures, maths and material functions "
	  "feeding the shading output. The engine compiles it for whichever backend you "
	  "are running (Metal, OpenGL, D3D, Vulkan), so one graph looks the same "
	  "everywhere.",
	  "Create a Material in the Content Browser and double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "material" },

	{ "material-assign",
	  "Assign it and see it",
	  "Drop the material into the Material component's asset field (or drag it onto "
	  "the entity) and the viewport updates immediately.\n"
	  "The preview sphere in the material tab renders with the same shader path as "
	  "the scene, so what you see there is what you get.",
	  "Drag your material onto a Material component's asset slot in Details.",
	  "Details", Check::MaterialAssigned, "" },

	{ "material-function",
	  "Material functions",
	  "A Material Function is a reusable sub-graph — a bit of shading maths you "
	  "want in five materials without copying it five times. It opens in the same "
	  "node canvas and appears as a single node inside the materials that use it.\n"
	  "Nothing is interpreted at runtime: on export a function is inlined into "
	  "every material that calls it, so the shipped shader has no indirection.",
	  "Create a Material Function and double-click it to open it.",
	  "Content Browser", Check::TabOfTypeOpened, "materialfunction" },
};

// ── 7. Environment ──
constexpr Step kEnvironment[] = {
	{ "sky",
	  "Sky and Weather are entities",
	  "There is no hidden world settings blob: the sky is an entity called Sky "
	  "carrying an Environment component, and weather is an entity called Weather. "
	  "Delete them and the scene renders against a flat background.\n"
	  "Window - Environment adds or removes either one; the Sun and Moon are children "
	  "of the Sky entity and travel with it.",
	  "Open Window - Environment.",
	  "Environment", Check::EnvironmentOpen, "" },

	{ "sky-tuning",
	  "Time of day, clouds, stars",
	  "Select the Sky entity and its Details panel has the whole atmosphere: day/"
	  "night cycle and time of day, sun and moon, volumetric clouds, cirrus and "
	  "contrails, fog, stars, nebula and aurora.\n"
	  "The Weather entity drives cloud coverage, wind, and rain/snow particles on "
	  "top of it. All of it is scene data, so it saves with the level.",
	  "Select the Sky entity and drag its Time of Day slider.",
	  "World Outliner|Details", Check::TimeOfDayChanged, "" },

	{ "lights",
	  "Local lights",
	  "Add a Light component to an entity for a directional, point or spot light. "
	  "Point and spot lights get shadow maps from a shared atlas, and the clustered "
	  "lighting path means you are not limited to a handful of them.\n"
	  "Ambient light comes from the sky itself — an image-based term the sky pass "
	  "feeds into shading.",
	  "Add a Light component to an entity.",
	  "Details", Check::ComponentAdded, "light" },
};

// ── 8. Landscape & foliage ──
constexpr Step kLandscape[] = {
	{ "landscape-mode",
	  "Landscape mode",
	  "The mode selector in the Scene toolbar switches from View to Landscape. The "
	  "left panel then becomes the terrain toolset instead of Quick Settings.\n"
	  "Create Landscape builds a heightfield with the size, resolution and noise you "
	  "set; it is split into chunks with automatic LODs, so a large terrain still "
	  "culls and renders cheaply.",
	  "Switch the Scene toolbar's mode selector to Landscape.",
	  "Scene", Check::LandscapeMode, "" },

	{ "sculpt",
	  "Sculpt and paint",
	  "The Landscape panel on the left replaces Quick Settings while the mode is "
	  "active: Create Landscape first, then the brush raises, lowers, smooths and "
	  "flattens the heightfield under the cursor.\n"
	  "Paint mode paints layer weights instead, which a Landscape Layer Blend node "
	  "in the terrain material turns into different surfaces. Brush size, strength "
	  "and falloff are on the same panel; Reset Sculpting returns to the generated "
	  "shape.",
	  "Press Create Landscape in the Landscape panel, then drag the brush over it.",
	  "Quick Settings", Check::ComponentAdded, "terrain" },

	{ "foliage",
	  "Foliage",
	  "A Foliage component scatters a mesh across a surface — grass, rocks, trees — "
	  "as GPU instances rather than as thousands of entities, with density, scale "
	  "jitter and cull distance as parameters.\n"
	  "Switch the Scene toolbar back to View mode when you are done with the terrain.",
	  "Add a Foliage component to an entity.",
	  "Details", Check::ComponentAdded, "foliage" },
};

// ── 9. Physics ──
constexpr Step kPhysics[] = {
	{ "rigidbody",
	  "Rigid bodies",
	  "A Rigid Body makes an entity take part in the simulation. Static geometry "
	  "wants a collider without a rigid body; anything that should fall, roll or be "
	  "pushed wants both.\n"
	  "Physics only runs in play mode — in the editor nothing falls, which is what "
	  "you want while you are placing things.",
	  "Add a Rigid Body component to an entity above the ground.",
	  "Details", Check::ComponentAdded, "rigidbody" },

	{ "collisions",
	  "Colliders and collision callbacks",
	  "A Collider gives the body its shape: box, sphere, capsule or mesh. Without "
	  "one a rigid body has mass but nothing to hit.\n"
	  "Collision and trigger events are delivered to your gameplay logic — a script, "
	  "a HorizonCode graph or a native C++ class — so a trap, a pickup or a door is "
	  "a handful of nodes rather than a polling loop. Raycasts against the physics "
	  "world come from the same place.",
	  "Add a Collider component to the same entity.",
	  "Details", Check::ComponentAdded, "collider" },
};

// ── 10. Particles ──
constexpr Step kParticles[] = {
	{ "particle-asset",
	  "Particle systems",
	  "A Particle System is a full asset with its own graph editor: emission, "
	  "lifetime, velocity, colour and size over life, and the material the "
	  "particles are drawn with.\n"
	  "An entity references it through a Particle System component. On export the "
	  "graph is baked into real shader code, so a packaged build does not interpret "
	  "anything at runtime.",
	  "Create a Particle System asset and double-click it to open it.",
	  "Content Browser", Check::TabOfTypeOpened, "particlesystem" },

	{ "particle-assign",
	  "Put it in the scene",
	  "Add a Particle System component to an entity and point it at your asset. "
	  "Emission is previewed live in the viewport.\n"
	  "The rain and snow of the Weather entity use the same system, simulated on the "
	  "GPU where the backend supports it.",
	  "Add a Particle System component to an entity.",
	  "Details", Check::ComponentAdded, "particlesystem" },
};

// ── 11. Animation ──
constexpr Step kAnimation[] = {
	{ "skeletal",
	  "Skeletal meshes",
	  "An imported rigged model becomes a Skeletal Mesh asset plus its Animation "
	  "Clips. Its editor tab shows the mesh with a bone overlay so you can check the "
	  "rig came through intact.\n"
	  "Skinning runs on the GPU on every backend, so a crowd costs about what the "
	  "same triangle count costs without a skeleton.\n"
	  "This one is reading only — a rigged model is something you bring, not "
	  "something the editor can hand you.",
	  "", "Content Browser", Check::ReadAck, "" },

	{ "state-machine",
	  "Animator state machines",
	  "An Animator State Machine is its own asset, edited in the same node canvas as "
	  "materials and HorizonCode: states hold clips, transitions hold conditions, "
	  "and parameters are driven from gameplay.\n"
	  "An entity's Animator component only references the asset and keeps the "
	  "runtime state, so one machine can drive a whole crowd.",
	  "Create an Animator State Machine asset and double-click it to open it.",
	  "Content Browser", Check::TabOfTypeOpened, "animatorstatemachine" },

	// The rest of Create Asset - Gameplay's animation block, in the order the menu
	// lists it. Each is a standalone asset with its own tab, and none needs a rig
	// to be created or opened, so each can be observed in the sandbox.
	{ "bone-mask",
	  "Bone masks",
	  "A Bone Mask says which joints an animation layer may touch, and how "
	  "strongly — a reload that plays on the upper body while the legs keep "
	  "running. The Animation Layers component points each layer at one.\n"
	  "A mask stores joint NAMES, not a skeleton, so one mask fits every rig that "
	  "names its joints the same way. The tab asks for a reference mesh only to "
	  "draw the tree you tick joints in.",
	  "Create Asset - Gameplay - Bone Mask, then double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "bonemask" },

	{ "blend-space",
	  "Blend spaces",
	  "A Blend Space places several clips in a one- or two-dimensional parameter "
	  "space — idle, walk and run along speed, or eight directions of strafing — "
	  "and mixes them by where the parameters stand. A state in a state machine "
	  "can play one instead of a single clip.\n"
	  "Its tab is a diagram, not a graph: drag the points, then drag the cursor to "
	  "see the mix.",
	  "Create Asset - Gameplay - Blend Space and double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "blendspace" },

	{ "property-clip",
	  "Property animation",
	  "Not everything that moves has a skeleton. A Property Animation Clip "
	  "animates component properties over time — a moving platform, a door, a "
	  "light that fades — with one row per property and keys on a shared time "
	  "axis.\n"
	  "It opens in the Sequencer tab, and a Property Animator component on the "
	  "entity plays it.",
	  "Create Asset - Gameplay - Property Animation Clip and double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "propertyanimclip" },

	{ "sequence",
	  "Cinematic sequences",
	  "A Sequence is a cutscene: several actors on one clock, with camera cuts, "
	  "skeletal clips, events and sound. It opens in the Cinematic tab, which "
	  "previews the shot through the sequence's own cameras without writing any "
	  "of it into your level.\n"
	  "A Sequence Player component plays it while the game runs and receives the "
	  "events that are addressed to no particular actor.",
	  "Create Asset - Gameplay - Sequence and double-click it.",
	  "Content Browser", Check::TabOfTypeOpened, "sequence" },
};

// ── 12. Navigation ──
constexpr Step kNavigation[] = {
	{ "navmesh",
	  "Navigation meshes",
	  "A Nav Mesh component on an entity defines a bake volume; the Bake button in "
	  "its Details section walks the scene geometry and builds the walkable surface. "
	  "The result is drawn as an overlay in the viewport.\n"
	  "Nav Agent components then path across it — agent radius, height and speed are "
	  "per-agent.",
	  "Add a Nav Mesh component and press Bake.",
	  "Details", Check::ComponentAdded, "navmesh" },
};

// ── 13. UI ──
constexpr Step kUI[] = {
	{ "widget",
	  "In-game UI widgets",
	  "A UI Widget asset opens in a two-part editor: a Designer where you lay out "
	  "panels, images, text, buttons, check boxes, sliders, progress bars, text "
	  "inputs and combo boxes, and a Graph where you wire up their behaviour.\n"
	  "Widgets live outside the world — you show and hide them from gameplay code, "
	  "so a menu does not need an entity.",
	  "Create a UI Widget asset and double-click it to open it.",
	  "Content Browser", Check::TabOfTypeOpened, "widget" },

	{ "widget-logic",
	  "Widget logic",
	  "The Graph side of the widget tab is HorizonCode: event nodes for clicks and "
	  "hovers, get/set nodes for the widget's own elements, typed graph variables "
	  "that persist per widget, and functions with access modifiers.\n"
	  "From a script or another graph you can call a widget's public functions by "
	  "name, which is how a HUD stays decoupled from the gameplay that feeds it.\n"
	  "Switch your open widget tab to Graph and have a look before moving on.",
	  "", "", Check::ReadAck, "" },

	{ "theme",
	  "Themes",
	  "A Theme is the look shared by every widget: colour roles, size steps, text "
	  "sizes and shadows, each with a light and a dark value side by side, so the "
	  "mode you are not looking at cannot be forgotten.\n"
	  "Widgets refer to roles (Accent, Surface) rather than to colours, so "
	  "changing the theme restyles the whole interface at once. It sits under "
	  "Create Asset - Data because it is a set of values, not a layout.",
	  "Create Asset - Data - Theme, then double-click it to open the theme editor.",
	  "Content Browser", Check::TabOfTypeOpened, "theme" },
};

// ── HorizonCode ──
// Built entirely on the Level Script and the Game Instance, on purpose: those two
// graphs exist in EVERY project whatever its scripting language, while a
// HorizonCode Class asset only exists in a HorizonCode project. A chapter that
// asked a Lua project to create one would be a step nobody there could finish.
constexpr Step kHorizonCode[] = {
	{ "hc-intro",
	  "HorizonCode",
	  "HorizonCode is the visual scripting language: a graph of nodes instead of "
	  "text. White exec pins are the order things happen in, coloured data pins are "
	  "the values flowing between them.\n"
	  "The editor interprets the graph so you can play immediately, but a packaged "
	  "build does not: on export every graph is translated into real C++ and "
	  "compiled, the same way the material graphs become shader code. What you "
	  "build here is not a slower way to write gameplay.\n"
	  "It is the same node canvas as materials, particle systems and animator state "
	  "machines — one set of gestures for all of them.",
	  "", "", Check::ReadAck, "" },

	{ "hc-level-script",
	  "The Level Script",
	  "Window - Level Script opens the graph belonging to the scene you have open. It "
	  "has the events the level itself cares about: Level Loaded, Level Unloaded "
	  "and a per-frame update.\n"
	  "It is stored inside the scene, so it travels with the level and is saved by "
	  "the same Ctrl/Cmd+S.",
	  "Open Window - Level Script.",
	  "", Check::TabOpen, "::LevelScript::" },

	{ "hc-node",
	  "Adding nodes",
	  "Right-click the empty canvas for the searchable add menu, or drag off a pin "
	  "and let go — the menu then lists only what can legally connect there.\n"
	  "Drag from an out pin to an in pin to wire two nodes; drag a link away from "
	  "its input to cut it. The canvas pans with the middle mouse button and zooms "
	  "with the wheel, like the viewport.",
	  "Add any node to the Level Script graph.",
	  "", Check::HcNodeAdded, "" },

	{ "hc-variables",
	  "Graph variables",
	  "The Variables list on the left of the canvas holds typed, persistent state "
	  "for this graph — a score, a door's open flag, a reference to the player. Add "
	  "one and it gives you Get and Set nodes for it; dragging the variable onto the "
	  "canvas offers both.\n"
	  "Functions live under it, each with its own sub-graph and an access modifier. "
	  "Public ones can be called by name from another graph or from a script, which "
	  "is how a HUD widget and the gameplay that feeds it stay decoupled.",
	  "Add a variable to the Level Script graph.",
	  "", Check::HcVariableAdded, "" },

	{ "hc-game-instance",
	  "The Game Instance",
	  "Window - Game Instance opens the one graph that outlives every scene: it is "
	  "created before anything loads and runs until the game quits. Save data, "
	  "settings, the current player, a scene-change request — anything that must "
	  "survive a level change belongs here rather than in a level script.\n"
	  "It is a project asset, not a scene one, so it is the same graph whichever "
	  "level is open.",
	  "Open Window - Game Instance.",
	  "", Check::TabOpen, "::GameInstance::" },

	{ "hc-compile",
	  "Compiling and the rest of the language",
	  "The Compile button on the canvas runs exactly the translation the export "
	  "would run and reports the first node that would not compile, highlighted in "
	  "place — so a broken graph is found here rather than at packaging time.\n"
	  "HorizonCode classes (their own asset, in a HorizonCode project) attach to an "
	  "entity the way a script does, and widget graphs drive UI. All three are the "
	  "same language with different events wired in.",
	  "", "", Check::ReadAck, "" },
};

// ── Scripting ──
constexpr Step kScripting[] = {
	{ "language",
	  "Your project's language",
	  "A project is authored in exactly one gameplay language — HorizonCode (visual "
	  "graphs), Lua, Python or C++ — chosen when it was created. The editor only "
	  "offers the matching assets everywhere, so there is no way to end up with half "
	  "a project in each.\n"
	  "UI widgets and the two graphs from the HorizonCode chapter — the Level Script "
	  "and the Game Instance — are shared by every language.",
	  "", "", Check::ReadAck, "" },

	{ "entity-logic",
	  "Logic on an entity",
	  "Per-entity behaviour is a script asset on a Script component (Lua/Python), a "
	  "HorizonCode class, or a native C++ class in Source/ — depending on the "
	  "project's language.\n"
	  "Properties you declare on it show up in the Details panel, so designers can "
	  "tune behaviour without opening the code.\n"
	  "Which of those you get is fixed by the project you created, so this card is "
	  "reading only.",
	  "", "Details", Check::ReadAck, "" },

	{ "input",
	  "Input actions",
	  "Input is data, not hard-coded keys: an Input Action asset names something the "
	  "player can do, and an Input Mapping Context binds keys, buttons and axes to "
	  "those actions.\n"
	  "Gameplay listens to the action, so rebinding never touches your logic.",
	  "Create an Input Action asset in the Content Browser.",
	  "Content Browser", Check::AssetOfTypeAdded, "inputaction" },

	{ "input-mapping",
	  "Mapping contexts",
	  "A Mapping Context is the other half: it binds concrete keys, buttons and "
	  "axes to those actions, and several can be active at once with a priority. "
	  "Walking, driving and being in a menu are three contexts you push and pop "
	  "rather than three sets of if-statements.\n"
	  "A Player Host on an entity is what activates them for a player.",
	  "Create an Input Mapping Context asset.",
	  "Content Browser", Check::AssetOfTypeAdded, "inputmappingcontext" },
};

// ── 15. Play ──
constexpr Step kPlay[] = {
	{ "play",
	  "Play in the editor",
	  "The Play button in the Scene toolbar runs the scene in place: physics ticks, "
	  "scripts run, the game camera takes over and the mouse is captured.\n"
	  "Esc releases the mouse so you can click Stop, and presses again to resume. "
	  "Stop restores the scene exactly as it was — play mode never edits your level.",
	  "Press Play, look around, then press Stop.",
	  "Scene", Check::PlayCycled, "" },

	{ "play-report",
	  "The play report",
	  "Every warning and error logged during a session is collected and shown after "
	  "you stop, with the play-clock time it first appeared and repeats collapsed.\n"
	  "It is the fastest way to notice a script that threw on frame 300 while you "
	  "were looking somewhere else. A clean session shows nothing at all, which is "
	  "why this card does not wait for one.",
	  "", "", Check::ReadAck, "" },
};

// ── 16. Performance ──
constexpr Step kPerformance[] = {
	{ "settings",
	  "Engine settings",
	  "Edit - Preferences opens the Preferences tab. Its Rendering pages hold the "
	  "engine switches: render path (forward or deferred), ambient occlusion "
	  "method, bloom, screen-space reflections, ray-traced global illumination, "
	  "GPU particles, VSync and frame cap.\n"
	  "Anything you pin there also appears in Quick Settings next to the viewport, "
	  "so the switches you A/B most are one click away.",
	  "Open Edit - Preferences and look through the Rendering settings.",
	  // No highlight: what the user has to hit is a menu, not a panel. Pointing at
	  // Quick Settings here would be pointing at the wrong thing.
	  "", Check::PreferencesOpen, "" },

	{ "profiler",
	  "The profiler",
	  "Window - Performance Profiler shows live CPU and GPU frame cost, per-pass GPU "
	  "timings and render counters. F9 starts and stops a benchmark capture, which "
	  "writes a JSON dump next to the log.\n"
	  "Capture before and after a change — the per-pass breakdown tells you which "
	  "pass actually paid for the effect you just turned on.",
	  "Open Window - Performance Profiler.",
	  "Performance Profiler", Check::ProfilerOpen, "" },
};

// ── 17. Packaging ──
constexpr Step kPackaging[] = {
	{ "export",
	  "Export your project",
	  "Build - Export Project packs the whole project into a .hpak archive next to a "
	  "runtime executable: compressed, optionally encrypted, with unchanged assets "
	  "reused from the previous export.\n"
	  "Export profiles are saved in the project — Development and Shipping are "
	  "seeded for you — and each one picks its startup scene, target platform and "
	  "exclude patterns.",
	  "Open Build - Export Project.",
	  "", Check::ExportOpen, "" },

	{ "targets",
	  "Targets and shipping",
	  "A profile targets this machine or Windows/macOS/Linux from a prebuilt runtime "
	  "bundle; macOS can emit a signed .app. Node-graph materials and particle "
	  "graphs are compiled ahead of time for the backends you tick, so the shipped "
	  "build has no first-frame compile hitch.\n"
	  "The result is a folder you can hand to someone — no editor, no engine "
	  "install. Have a look at the profile in the export dialog, then close it.",
	  "", "", Check::ReadAck, "" },
};

// ── 18. Done ──
constexpr Step kFinish[] = {
	{ "finish",
	  "That is the tour",
	  "You have seen every major system the editor ships with, and used each one "
	  "yourself — nothing here was a special tutorial mode, it was the real editor "
	  "the whole way.\n"
	  "Help - Interactive Tutorial reopens this window at any time, and the Tutorial "
	  "project template in the Project Hub recreates the sample project whenever you "
	  "want a clean sandbox.",
	  "", "", Check::ReadAck, "" },
};

constexpr Chapter kChapters[] = {
	{ "orientation", "Getting oriented",   "The editor's panels and what each one is for",
	  kOrientation, static_cast<int>(std::size(kOrientation)) },
	{ "viewport",    "Moving around",      "Flying, orbiting and framing in the Scene view",
	  kViewport,    static_cast<int>(std::size(kViewport)) },
	{ "entities",    "Entities",           "The scene tree, creating things, the gizmo",
	  kEntities,    static_cast<int>(std::size(kEntities)) },
	{ "components",  "Components",         "Turning an empty entity into something",
	  kComponents,  static_cast<int>(std::size(kComponents)) },
	{ "content",     "Assets",             "The Content Browser, importing and creating",
	  kContent,     static_cast<int>(std::size(kContent)) },
	{ "assettypes",  "Asset types",        "Every kind of .hasset and where it comes from",
	  kAssetTypes,  static_cast<int>(std::size(kAssetTypes)) },
	{ "materials",   "Materials",          "Node-graph surfaces that compile everywhere",
	  kMaterials,   static_cast<int>(std::size(kMaterials)) },
	{ "environment", "Sky, weather, light","Atmosphere as scene data, plus local lights",
	  kEnvironment, static_cast<int>(std::size(kEnvironment)) },
	{ "landscape",   "Landscape",          "Heightfield terrain, sculpting, painting, foliage",
	  kLandscape,   static_cast<int>(std::size(kLandscape)) },
	{ "physics",     "Physics",            "Rigid bodies, colliders and collision events",
	  kPhysics,     static_cast<int>(std::size(kPhysics)) },
	{ "particles",   "Particles",          "Particle assets and their graph editor",
	  kParticles,   static_cast<int>(std::size(kParticles)) },
	{ "animation",   "Animation",          "Skeletal meshes, clips and state machines",
	  kAnimation,   static_cast<int>(std::size(kAnimation)) },
	{ "navigation",  "Navigation",         "Baking nav meshes and moving agents",
	  kNavigation,  static_cast<int>(std::size(kNavigation)) },
	{ "ui",          "User interface",     "Widget designer and widget logic",
	  kUI,          static_cast<int>(std::size(kUI)) },
	{ "horizoncode", "HorizonCode",        "Visual scripting: graphs, variables, compile-to-C++",
	  kHorizonCode, static_cast<int>(std::size(kHorizonCode)) },
	{ "scripting",   "Gameplay logic",     "Project language, entity logic and input assets",
	  kScripting,   static_cast<int>(std::size(kScripting)) },
	{ "play",        "Playing",            "Play in editor and the post-play report",
	  kPlay,        static_cast<int>(std::size(kPlay)) },
	{ "performance", "Settings & profiling","Engine switches and where the frame goes",
	  kPerformance, static_cast<int>(std::size(kPerformance)) },
	{ "packaging",   "Shipping",           "Export profiles, targets and packaged builds",
	  kPackaging,   static_cast<int>(std::size(kPackaging)) },
	{ "finish",      "Done",               "Where to go from here",
	  kFinish,      static_cast<int>(std::size(kFinish)) },
};

} // namespace

const Chapter* chapters()   { return kChapters; }
int            chapterCount() { return static_cast<int>(std::size(kChapters)); }

int totalSteps()
{
	int n = 0;
	for (const Chapter& c : kChapters) n += c.stepCount;
	return n;
}

// ─── Completion ──────────────────────────────────────────────────────────────
// Read every case as "what CHANGED since this step opened". A check that merely
// asks "is X true" would be pre-satisfied by whatever the scene happened to
// contain, and the step would tick itself off before the user did anything.
bool satisfied(const Step& step, const Signals& base, const Signals& now)
{
	switch (step.check)
	{
	case Check::ReadAck:          return now.acknowledged;
	case Check::EntityAdded:      return now.entityCount > base.entityCount;
	case Check::AssetAdded:       return now.assetCount  > base.assetCount;
	case Check::UndoUsed:         return now.undoCount   > base.undoCount;
	case Check::ImportOpened:     return now.importOpens > base.importOpens;
	case Check::PlayCycled:       return now.playSessions > base.playSessions;
	case Check::MaterialAssigned: return now.materialsAssigned > base.materialsAssigned;
	case Check::HcNodeAdded:      return now.hcNodes     > base.hcNodes;
	case Check::HcVariableAdded:  return now.hcVariables > base.hcVariables;

	// "Switched to" rather than "is on": a step that opens while the target is
	// already showing must still ask the user to do it, not tick instantly.
	case Check::LandscapeMode:    return now.landscapeMode && !base.landscapeMode;
	case Check::PreferencesOpen:  return now.preferencesOpen && !base.preferencesOpen;
	case Check::ProfilerOpen:     return now.profilerOpen && !base.profilerOpen;
	case Check::EnvironmentOpen:  return now.environmentOpen && !base.environmentOpen;
	case Check::ExportOpen:       return now.exportOpen && !base.exportOpen;

	// A save only counts when there was something to save; opening this step on an
	// already-clean scene would otherwise tick it off without the user touching
	// anything.
	case Check::SceneSaved:       return base.sceneUnsaved && !now.sceneUnsaved;

	// Selecting the entity that was ALREADY selected is not "click an entity".
	case Check::SelectionChanged:
		return now.selectionSet && now.selectedEntity != base.selectedEntity;

	case Check::ContentRootShown:
	{
		// arg names the root by word so the step table does not encode the
		// Content-Browser's internal index.
		const std::string_view a = step.arg;
		const int want = (a == "engine") ? 1 : (a == "source") ? 2 : (a == "content") ? 0 : -1;
		return want >= 0 && now.contentRootKind == want && base.contentRootKind != want;
	}

	case Check::CameraFlown:
	{
		// Both, and deliberately: turning on the spot is the RMB look, walking is
		// WASD, and the step teaches the two together. The thresholds are well
		// above the jitter a click without a drag produces.
		const float dx = now.camX - base.camX;
		const float dy = now.camY - base.camY;
		const float dz = now.camZ - base.camZ;
		const float moved  = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float turned = std::abs(now.camYaw - base.camYaw) +
		                     std::abs(now.camPitch - base.camPitch);
		return moved > 1.0f && turned > 0.15f;
	}

	case Check::CameraZoomed:
		// The wheel (and an orbit dolly) is the only thing that changes the pivot
		// distance, so this cannot be satisfied by flying around.
		return std::abs(now.camPivot - base.camPivot) > 0.5f;

	case Check::TimeOfDayChanged:
		return now.skyPresent && base.skyPresent &&
		       std::abs(now.timeOfDay - base.timeOfDay) > 0.005f;

	case Check::ComponentAdded:
	{
		const Comp c = compFromString(step.arg);
		return c != Comp::Count && now.count(c) > base.count(c);
	}

	case Check::AssetOfTypeAdded:
	{
		const Asset a = assetFromString(step.arg);
		return a != Asset::Count && now.count(a) > base.count(a);
	}

	case Check::TabOfTypeOpened:
	{
		const Asset a = assetFromString(step.arg);
		return a != Asset::Count && now.tabCount(a) > base.tabCount(a);
	}

	case Check::TabOpen:
		return step.arg[0] != '\0' &&
		       base.openTabs.find(step.arg) == std::string::npos &&
		       now.openTabs.find(step.arg) != std::string::npos;

	case Check::PanelsVisited:
	{
		const int n = listEntryCount(step.arg);
		if (n == 0) return false;
		for (int i = 0; i < n; ++i)
			if (!listEntryVisited(step.arg, i, now.visitedPanels)) return false;
		return true;
	}
	}
	return false;
}

// ─── Cursor arithmetic ───────────────────────────────────────────────────────
// A cursor of { chapterCount(), 0 } is the one canonical "finished" position;
// clamp() maps every out-of-range or stale cursor onto a valid one so no caller
// has to range-check before indexing.
Cursor clamp(Cursor c)
{
	const int nChapters = chapterCount();
	if (c.chapter < 0 || c.step < 0)       return Cursor{ 0, 0 };
	if (c.chapter >= nChapters)            return Cursor{ nChapters, 0 };
	if (c.step >= kChapters[c.chapter].stepCount)
	{
		// Past the end of its chapter — roll forward rather than clamping onto the
		// last step, so a saved position from a shortened chapter resumes at the
		// next thing the user has not seen.
		return clamp(Cursor{ c.chapter + 1, 0 });
	}
	return c;
}

bool finished(Cursor c)
{
	c = clamp(c);
	return c.chapter >= chapterCount();
}

Cursor advance(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return c;
	return clamp(Cursor{ c.chapter, c.step + 1 });
}

Cursor retreat(Cursor c)
{
	c = clamp(c);
	if (c.chapter == 0 && c.step == 0) return c;
	if (finished(c))
	{
		const int last = chapterCount() - 1;
		return Cursor{ last, kChapters[last].stepCount - 1 };
	}
	if (c.step > 0) return Cursor{ c.chapter, c.step - 1 };
	const int prev = c.chapter - 1;
	return Cursor{ prev, kChapters[prev].stepCount - 1 };
}

Cursor nextChapter(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return c;
	return clamp(Cursor{ c.chapter + 1, 0 });
}

const Step* stepAt(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return nullptr;
	return &kChapters[c.chapter].steps[c.step];
}

const Chapter* chapterAt(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return nullptr;
	return &kChapters[c.chapter];
}

int flatIndex(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return totalSteps();
	int n = 0;
	for (int i = 0; i < c.chapter; ++i) n += kChapters[i].stepCount;
	return n + c.step;
}

Cursor fromFlat(int index)
{
	if (index < 0) return Cursor{ 0, 0 };
	for (int i = 0; i < chapterCount(); ++i)
	{
		if (index < kChapters[i].stepCount) return Cursor{ i, index };
		index -= kChapters[i].stepCount;
	}
	return Cursor{ chapterCount(), 0 };
}

Cursor findStep(std::string_view id)
{
	for (int ci = 0; ci < chapterCount(); ++ci)
		for (int si = 0; si < kChapters[ci].stepCount; ++si)
			if (id == kChapters[ci].steps[si].id) return Cursor{ ci, si };
	return Cursor{ chapterCount(), 0 };
}

std::string serialize(Cursor c)
{
	c = clamp(c);
	if (finished(c)) return "done";
	return kChapters[c.chapter].steps[c.step].id;
}

Cursor deserialize(std::string_view s)
{
	if (s.empty())  return Cursor{ 0, 0 };
	if (s == "done") return Cursor{ chapterCount(), 0 };
	return findStep(s);
}

} // namespace HE::tut
