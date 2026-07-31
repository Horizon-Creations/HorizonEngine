#pragma once
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

	// ── How a step decides it is finished ────────────────────────────────────
	// Every check is evaluated against the signal snapshot taken when the step
	// became active (`base`) and the current one (`now`), so a check can require a
	// *transition* rather than a state that may already hold. Manual steps are pure
	// reading and are advanced by the user pressing Next.
	enum class Check : uint8_t
	{
		Manual,           // no automatic detection — the user clicks Next
		EntityAdded,      // the world gained an entity since the step opened
		ComponentPresent, // any entity carries `arg`'s component (see Comp)
		SelectionSet,     // something is selected in the Outliner/viewport
		SceneSaved,       // the scene went from unsaved/dirty to saved
		AssetAdded,       // the Content tree gained a file since the step opened
		TabOpen,          // an editor tab whose label or path contains `arg`
		PlayEntered,      // play-in-editor is currently running
		PlayCycled,       // a play session started AND stopped since the step opened
		LandscapeMode,    // the viewport mode selector is on "Landscape"
		ProfilerOpen,     // View ▸ Performance Profiler is showing
		EnvironmentOpen,  // View ▸ Environment is showing
		ExportOpen,       // Build ▸ Export Project is showing
	};

	struct Step
	{
		const char* id;          // stable across releases — persisted as progress
		const char* title;
		const char* body;        // paragraphs separated by '\n'
		const char* action;      // the one thing to do ("" = nothing to do, read on)
		const char* focusWindow; // ImGui window to highlight ("" = none)
		Check       check  = Check::Manual;
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
		bool selectionSet  = false;
		bool playing       = false;
		bool sceneUnsaved  = true; // dirty edits, or never written to disk
		bool landscapeMode = false;
		bool profilerOpen  = false;
		bool environmentOpen = false;
		bool exportOpen    = false;
		// Bit per Comp, set when ANY entity in the world carries that component.
		uint32_t componentMask = 0;
		// Open editor tabs as one '\n'-delimited blob of labels and asset paths,
		// substring-searched by Check::TabOpen.
		std::string openTabs;

		bool has(Comp c) const
		{
			return c < Comp::Count &&
			       (componentMask & (1u << static_cast<uint32_t>(c))) != 0;
		}
		void set(Comp c)
		{
			if (c < Comp::Count) componentMask |= (1u << static_cast<uint32_t>(c));
		}
	};

	// True when `step` counts as done. `base` is the snapshot from the moment the
	// step opened. Manual steps are never satisfied automatically.
	bool satisfied(const Step& step, const Signals& base, const Signals& now);

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
