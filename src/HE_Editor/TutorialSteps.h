#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// ── Guided tour: curriculum + completion rules ────────────────────────────────
// The step list the interactive tutorial walks a first-time user through, and the
// pure predicates that decide when a step is done.
//
// This translation unit deliberately knows nothing about ImGui, AppContext or any
// other editor type. TutorialPanel.cpp samples the live editor into a `Signals`
// snapshot once per frame and hands it here; everything else — which step is next,
// whether it is satisfied, how progress is written to the config — is decided by
// plain functions over plain data. That is what makes the curriculum testable
// without a window (tests/test_tutorial.cpp) and what keeps a step's text and its
// completion rule side by side in one table instead of scattered across panels.
//
// EVERY step is observed. There is no "press Next to skip" — the tour advances
// only when the editor has actually seen the user do the thing, and the prose-only
// cards (the handful with nothing to do in the editor) advance on an explicit
// acknowledgement that unlocks after the card has been read to the end. Two
// consequences that shape everything below:
//
//  * Checks describe a TRANSITION, not a state. "A light exists" is true the
//    moment a furnished sandbox scene loads, so a state check would tick the step
//    off before the user touched anything — which reads as a broken tutorial.
//    Every check therefore compares the snapshot taken when the step opened
//    (`base`) against the live one (`now`) and wants to see something CHANGE.
//  * Anything a check needs is a field on `Signals`. If a check cannot be
//    expressed over this struct, the step does not get that check.

namespace HE::tut
{
	// ── Components a step can ask the user to add ────────────────────────────
	// Spelled like the .hescene component keys so the step tables read the same as
	// the scene files. Nothing persists these values, so the order is free to move;
	// `Count` doubles as the "unknown name" result of compFromString.
	enum class Comp : uint8_t
	{
		Mesh, Material, Light, RigidBody, Collider, ParticleSystem, Script,
		Terrain, Foliage, NavMesh, Camera, AudioSource, Animator, UICanvas,
		Count
	};
	const char* compName(Comp c);              // "mesh", "rigidbody", …
	Comp        compFromString(std::string_view s); // Count when unknown

	// ── Asset kinds a step can ask the user to create ────────────────────────
	// A subset of HE::AssetType — only what the tour asks for. Kept as its own
	// enum for the same reason as Comp: this file must not depend on engine types.
	enum class Asset : uint8_t
	{
		Material, MaterialFunction, ParticleSystem, Widget, AnimatorStateMachine,
		InputAction, InputMappingContext, HorizonCodeClass, Scene, Texture,
		StaticMesh, SkeletalMesh, Script, Audio, Font, Prefab, AnimationClip,
		Count
	};
	const char* assetName(Asset a);              // "inputaction", "material", …
	Asset       assetFromString(std::string_view s); // Count when unknown

	// ── How a step decides it is finished ────────────────────────────────────
	// Every check is evaluated against the signal snapshot taken when the step
	// became active (`base`) and the current one (`now`), so a check requires a
	// *transition* rather than a state that may already hold when the step opens.
	enum class Check : uint8_t
	{
		// Prose-only card: nothing to do in the editor. Done when the user has
		// read to the end and pressed the acknowledge button (the panel only
		// enables it once the body has been scrolled through).
		ReadAck,
		PanelsVisited,    // every window named in `arg` ('|'-separated) was focused
		CameraFlown,      // the Scene camera both turned and moved
		CameraZoomed,     // the Scene camera's orbit distance changed (wheel/orbit)
		UndoUsed,         // an undo was performed
		EntityAdded,      // the world gained an entity
		ComponentAdded,   // one more entity carries `arg`'s component than before
		SelectionChanged, // a different entity is selected than when the step opened
		SceneSaved,       // the scene went from unsaved/dirty to saved
		AssetAdded,       // the Content tree gained a file
		AssetOfTypeAdded, // the Content tree gained an asset of `arg`'s type
		MaterialAssigned, // one more Material component points at a material asset
		TimeOfDayChanged, // the Sky entity's time of day was moved
		HcNodeAdded,      // a node was added to the Level Script / Game Instance graph
		HcVariableAdded,  // a graph variable was added to either of those graphs
		TabOpen,          // an editor tab whose label or path contains `arg` appeared
		TabOfTypeOpened,  // an editor tab holding an asset of `arg`'s type appeared
		PlayCycled,       // a play session started AND stopped
		LandscapeMode,    // the viewport mode selector was switched to "Landscape"
		ContentRootShown, // the Content Browser was switched to `arg`'s root
		ImportOpened,     // Assets ▸ Import Asset was opened
		PreferencesOpen,  // Edit ▸ Preferences was opened
		ProfilerOpen,     // View ▸ Performance Profiler was opened
		EnvironmentOpen,  // View ▸ Environment was opened
		ExportOpen,       // Build ▸ Export Project was opened
	};

	struct Signals;

	// True for the checks whose target is a window the user has to open, and which
	// therefore cannot fire while that window was ALREADY open when the step began.
	bool wantsWindowOpened(Check c);
	// Whether that window is open in `s`. Handed the step's BASELINE snapshot, the
	// two together are "this step cannot fire because it was already open" — which
	// the panel turns into "close it and open it again" instead of leaving the user
	// staring at a step that looks stuck.
	bool windowOpenIn(Check c, const Signals& s);

	struct Step
	{
		const char* id;          // stable across releases — persisted as progress
		const char* title;
		const char* body;        // paragraphs separated by '\n'
		const char* action;      // the one thing to do ("" = prose card, see ReadAck)
		const char* focusWindow; // ImGui window(s) to highlight, '|'-separated ("" = none)
		Check       check  = Check::ReadAck;
		const char* arg    = "";
	};

	struct Chapter
	{
		const char* id;
		const char* title;
		const char* summary;
		const Step* steps;
		int         stepCount;
	};

	const Chapter* chapters();
	int            chapterCount();
	int            totalSteps();

	// ── Live editor snapshot ─────────────────────────────────────────────────
	// Filled by TutorialPanel each frame. Everything a check can look at lives
	// here; nothing else about the editor is visible to this file.
	struct Signals
	{
		int  entityCount   = 0;
		int  assetCount    = 0;   // files under the project's Content root
		int  playSessions  = 0;   // completed play-in-editor sessions this run
		int  undoCount     = 0;   // undos performed this run
		int  importOpens   = 0;   // times the import-asset dialog was opened
		int  materialsAssigned = 0; // Material components pointing at an asset
		int  contentRootKind = 0; // Content Browser root: 0 Content, 1 Engine, 2 Source
		bool playing       = false;
		bool sceneUnsaved  = true; // dirty edits, or never written to disk
		bool landscapeMode = false;
		bool preferencesOpen = false;
		bool profilerOpen  = false;
		bool environmentOpen = false;
		bool exportOpen    = false;
		bool acknowledged  = false; // the ReadAck button was pressed for this step

		// Selection identity, not just "something is selected": re-selecting the
		// entity that was already selected must not satisfy SelectionChanged.
		bool     selectionSet = false;
		uint32_t selectedEntity = 0;

		// Editor camera pose. CameraFlown/CameraZoomed compare these against the
		// step's baseline — the camera is the only way to observe "the user flew
		// around", since navigation is a gesture and leaves no other trace.
		float camX = 0.0f, camY = 0.0f, camZ = 0.0f;
		float camYaw = 0.0f, camPitch = 0.0f, camPivot = 0.0f;

		// Sky time of day, so "drag the time-of-day slider" is observable.
		float timeOfDay = 0.0f;
		bool  skyPresent = false;

		// HorizonCode: nodes and variables across the two graphs every project has
		// regardless of its scripting language — the Level Script and the Game
		// Instance. Summed on purpose: the step names which graph to work in, and
		// counting them together means a user who experiments in the other one is
		// credited rather than told "no".
		int hcNodes     = 0;
		int hcVariables = 0;

		// How many entities carry each component, so ComponentAdded can want one
		// MORE rather than "at least one" (a furnished scene already has lights).
		std::array<int, static_cast<size_t>(Comp::Count)> compCount{};
		// Same idea for assets: how many of each kind live under Content.
		std::array<int, static_cast<size_t>(Asset::Count)> assetTypeCount{};
		// …and how many editor tabs currently hold an asset of each kind. Matched
		// by type, not by tab label: the tour tells the user to CREATE the asset,
		// and the create flow lets them name it, so "does the label contain
		// 'Material'" would fail on everyone who typed their own name.
		std::array<int, static_cast<size_t>(Asset::Count)> openTabTypeCount{};

		// Open editor tabs as one '\n'-delimited blob of labels and asset paths,
		// substring-searched by Check::TabOpen.
		std::string openTabs;
		// Windows focused since the current step opened, '\n'-delimited and
		// '\n'-terminated, so PanelsVisited can match whole names.
		std::string visitedPanels;

		int  count(Comp c) const
		{
			return c < Comp::Count ? compCount[static_cast<size_t>(c)] : 0;
		}
		void add(Comp c, int n = 1)
		{
			if (c < Comp::Count) compCount[static_cast<size_t>(c)] += n;
		}
		int  count(Asset a) const
		{
			return a < Asset::Count ? assetTypeCount[static_cast<size_t>(a)] : 0;
		}
		void add(Asset a, int n = 1)
		{
			if (a < Asset::Count) assetTypeCount[static_cast<size_t>(a)] += n;
		}
		int  tabCount(Asset a) const
		{
			return a < Asset::Count ? openTabTypeCount[static_cast<size_t>(a)] : 0;
		}
		void addTab(Asset a, int n = 1)
		{
			if (a < Asset::Count) openTabTypeCount[static_cast<size_t>(a)] += n;
		}
	};

	// True when `step` counts as done. `base` is the snapshot from the moment the
	// step opened.
	bool satisfied(const Step& step, const Signals& base, const Signals& now);

	// '|'-separated list helpers, shared by PanelsVisited and by the panel's
	// highlight (focusWindow is the same format).
	int              listEntryCount(std::string_view list);
	std::string_view listEntry(std::string_view list, int index);

	// Is `name` among the '\n'-delimited windows in `visited`? Whole-entry match,
	// so "Scene" is not satisfied by "Scene Settings". The panel uses it to grey
	// out the panels already ticked off and pulse only the ones still to visit.
	bool panelVisited(std::string_view name, std::string_view visited);
	// panelVisited() on the index-th entry of a '|'-separated list.
	bool listEntryVisited(std::string_view list, int index, std::string_view visited);

	// ── Position in the curriculum ───────────────────────────────────────────
	// A cursor one past the last step of the last chapter means "finished"; every
	// helper clamps into that range, so a corrupted or outdated saved position can
	// never index out of bounds.
	struct Cursor
	{
		int chapter = 0;
		int step    = 0;

		bool operator==(const Cursor& o) const { return chapter == o.chapter && step == o.step; }
	};

	Cursor clamp(Cursor c);
	Cursor advance(Cursor c);   // next step, rolling into the next chapter
	Cursor retreat(Cursor c);   // previous step, rolling back a chapter
	Cursor nextChapter(Cursor c);
	bool   finished(Cursor c);
	// The step a cursor points at, or nullptr when finished.
	const Step*    stepAt(Cursor c);
	const Chapter* chapterAt(Cursor c);
	// 0-based index across all chapters (== totalSteps() when finished).
	int    flatIndex(Cursor c);
	Cursor fromFlat(int index);
	// Cursor of a step id, or a finished cursor when the id is unknown — an id that
	// disappeared between releases must not strand the user mid-tour.
	Cursor findStep(std::string_view id);

	// Progress is stored as the step id, not an index: inserting a step in chapter 2
	// would otherwise silently move everyone's saved position.
	std::string serialize(Cursor c);
	Cursor      deserialize(std::string_view s);
}
