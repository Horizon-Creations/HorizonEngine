#include "doctest.h"

#include "DocsLibrary.h"
#include "EditorHelp.h"
#include "EditorGuides.h"
#include "EditorReference.h"
#include "EditorWidgets.h"

#include <imgui.h>
#include <UIWidget/UIElement.h>   // the widget-type registry the palette lists

#include <algorithm>
#include <cstring>
#include <set>
#include <string>

// ── The editor's tooltips ────────────────────────────────────────────────────
// The help table is the one part of the interface that can rot without anything
// failing: a tooltip pointing at a documentation section that was renamed still
// LOOKS fine, and only stops working for the user who presses F1 on it. So the
// link between the two is asserted here, in both directions that matter — every
// topic the table names has to exist in the shipped bundle, and the lookup rules
// the Details panel relies on have to behave.

using namespace HE::Ed;

namespace
{
	Docs::Library shipped()
	{
		Docs::Library lib;
#ifdef HE_DOCS_BUNDLE_PATH
		lib.load(HE_DOCS_BUNDLE_PATH);
#endif
		return lib;
	}
} // namespace

TEST_CASE("editor help: every entry is a usable tooltip")
{
	std::set<std::string> keys;
	for (int i = 0; i < Help::entryCount(); ++i)
	{
		const Help::Entry& e = Help::entryAt(i);
		CHECK(e.key != nullptr);
		// std::string, not the raw pointer: this doctest's message builder
		// stringifies a const char* as a bool, so every failure would say "1".
		const std::string key = e.key ? e.key : "";
		CHECK(key.size() > 0);
		CHECK_MESSAGE(keys.insert(key).second, "duplicate help key: ", key);

		// A tooltip that only repeats the label is the failure mode this whole
		// table exists to replace, so a body has to be a sentence.
		const std::string body = e.body ? e.body : "";
		CHECK_MESSAGE(body.size() >= 24, "help body too short to say anything: ", key);
		CHECK_MESSAGE(body.find("  ") == std::string::npos,
		              "double space in help body (a wrapped literal lost a space): ", key);

		// The scoped rows deliberately have no title — the label above them is
		// the heading. The hand-placed ones must carry one, because there is no
		// label to inherit.
		const bool scoped   = std::string(e.key).find('/') != std::string::npos;
		const bool hasTitle = e.title != nullptr && e.title[0] != '\0';
		if (!scoped)
			CHECK_MESSAGE(hasTitle, "hand-placed entry without a title: ", key);
	}
	// Guards the case where a bad merge leaves the table almost empty: it would
	// still compile, and every tooltip in the editor would simply be gone.
	CHECK(Help::entryCount() > 120);
}

// ── The gap the static audit cannot see ─────────────────────────────────────
// The palette draws one button per widget type with ImGui::Button(typeName(t)),
// and the coverage script only reads LITERAL labels — so nineteen controls were
// invisible to it and had no explanation at all. Third gap of that kind, after
// the fixed file list and DragInt2, and the same lesson each time: a coverage
// tool that does not know a call shape reports coverage, not a gap.
//
// A scan cannot answer this one, so a runtime test does: every type the registry
// hands out must have an entry, which means a NEW widget type fails here until
// somebody writes the sentence.
TEST_CASE("editor reference: every element type in the palette explains itself")
{
	for (HE::UIWidgetType t : HE::uiWidgetTypeRegistry())
	{
		const std::string key = std::string("UI Palette/") + HE::uiWidgetTypeName(t);
		const HE::Ed::Help::Entry* e = HE::Ed::Help::findKey(key);
		REQUIRE_MESSAGE(e != nullptr, "no palette description for ", key);
		// Long enough to be a sentence. The node-docs test uses the same floor,
		// for the same reason: a three-word stub is a box ticked, not an answer.
		CHECK_MESSAGE(std::strlen(e->body) >= 20, key);
	}
}

TEST_CASE("editor reference: F1 on a control opens the control, not a chapter")
{
	// The point of the generated reference, and the thing that was wrong before
	// it: 320 entries shared 47 topics, so F1 anywhere in the sky settings
	// opened one chapter about the sky. Now every entry has a section of its
	// own, and this is what says so.
	Docs::Library lib = shipped();
	REQUIRE_MESSAGE(lib.loaded(), lib.error());
	HE::Ed::EditorReference::install(lib);

	std::set<std::string> anchors;
	int checked = 0;
	for (int i = 0; i < Help::entryCount(); ++i)
	{
		const Help::Entry& e = Help::entryAt(i);
		const std::string key(e.key);

		// Every entry has a home. A key with no area rule would never appear in
		// the manual at all, and nothing else would say so.
		const Help::Area* area = Help::areaOf(key);
		REQUIRE_MESSAGE(area != nullptr, "help key with no area — add a rule: ", key);

		const std::string topic = Help::referenceTopic(key);
		REQUIRE(!topic.empty());
		int page = -1, section = -1;
		CHECK_MESSAGE(lib.resolve(topic, page, section),
		              "F1 target does not resolve: ", topic);
		CHECK_MESSAGE(section >= 0, "F1 lands on a page, not on the control: ", topic);
		// Two controls sharing an anchor would mean F1 on one opens the other.
		CHECK_MESSAGE(anchors.insert(topic).second, "two controls share an anchor: ", topic);
		++checked;
	}
	INFO("controls with an entry of their own: " << checked);
	CHECK(checked > 300);

	// And the reverse: the reference lists nothing that is not a control.
	int sections = 0;
	for (int i = 0; i < Help::areaCount(); ++i)
	{
		const Docs::Page* p = lib.page(Help::areaAt(i).page);
		if (!p) continue;
		for (const Docs::Section& s : p->sections)
		{
			++sections;
			CHECK(!s.title.empty());
			CHECK(!s.blocks.empty());
		}
	}
	INFO("sections in the generated reference: " << sections);
	CHECK(sections >= checked);
}

TEST_CASE("editor help: the interface's own controls resolve under their panel")
{
	// The static audit (scripts/editor_help_audit.py) walks a file top to bottom
	// and cannot see a scope that a CALLER pushes — the documentation reader
	// opens its scope in draw(), at the end of its file, for helpers defined
	// above it. So those four are checked here instead, where the lookup is the
	// real one.
	struct Case { const char* scope; const char* label; };
	const Case cases[] = {
		{ "Documentation", "Start" },
		{ "Documentation", "Online" },
		{ "Documentation", "Show me" },
		{ "Documentation", "Open the manual online" },
		// And one from every other panel scoped this round, so a scope renamed
		// in the panel and not in the table fails here rather than in silence.
		{ "File",            "Save All" },
		{ "View",            "Console" },
		{ "Help",            "Documentation" },
		{ "World Outliner",  "Save as Prefab" },
		{ "New Entity",      "Cube" },
		{ "Content Browser", "Find References" },
		{ "New Asset",       "Input Action" },
		{ "Console",         "Auto-scroll" },
		{ "Notifications",   "Mark all as seen" },
		{ "Play Report",     "Show warnings" },
		{ "Project Hub",     "Remove from list" },
		{ "Viewport Options", "Snap to grid" },
		{ "Tutorial",        "Start over" },
		{ "Collaboration",   "Ask to edit" },
		{ "Source Root",     "C++ Class" },
		// The settings window's three pages. Its catalogue is scoped
		// "Preferences", but the source-control and tool pages are not part of
		// that catalogue and push their own — which is how the first three of
		// these came to be written as "Preferences/…" and to resolve to nothing
		// at all for as long as they existed.
		{ "Source Control",  "Private" },
		{ "Source Control",  "Push automatically after each commit" },
		{ "Source Control",  "Check the remote for new commits periodically" },
		{ "Source Control",  "Initialize Git repository" },
		{ "Source Control",  "Create & push" },
		{ "Source Control",  "Save token" },
		{ "Source Control",  "Save Identity" },
		// The "##" suffix is part of the key: two buttons say "Recheck" and one
		// field says "Name", and only the suffix tells them apart.
		{ "Source Control",  "Recheck##git" },
		{ "Source Control",  "Set##remote" },
		{ "Source Control",  "Name##gh" },
		{ "Tool Status",     "Fix" },
		{ "Tool Status",     "Recheck all" },
		{ "Preferences",     "Restore Defaults" },
		// The settings catalog retargets its scope at each row's category, so a
		// setting resolves under "Preferences/<category>". One per category, so a
		// category renamed in the panel and not in the table fails here.
		{ "Preferences/Display",             "VSync" },
		{ "Preferences/Display",             "Backend" },
		{ "Preferences/Post-Processing",     "Bloom Threshold" },
		{ "Preferences/Global Illumination", "GI Refl Blur" },
		{ "Preferences/Effects",             "GPU Weather Particles" },
		{ "Preferences/Collaboration",       "Largest Asset to Transfer (MB)" },
		{ "Preferences/Viewport",            "Camera Speed" },
		{ "Preferences/Input",               "Stick Deadzone" },
		{ "Preferences/Appearance",          "UI Font Scale" },
		{ "Preferences/Content Browser",     "Refresh Interval (s)" },
		{ "Build Tools",     "Install Automatically" },
		{ "Build Tools",     "Recheck" },
		{ "Build Tools",     "Don't show this again" },
		// The material editor's five. Its nodes are drawn small enough that a
		// label is three letters, so a scope that stopped resolving here would
		// leave the shortest labels in the editor with nothing behind them.
		{ "Material Node",      "Pow" },
		{ "Material Node",      "Tile" },
		{ "Material Node",      "Set Texture" },
		{ "Material Node",      "Delete Node" },
		{ "Material Graph",     "Delete Comment" },
		{ "Material Parameter", ".." },
		{ "Material Parameter", "Min" },
		{ "Material Settings",  "Clip" },
		{ "Material Preview",   "Show Material" },
		{ "Material Preview",   "Load" },
		// The UI designer's six. Its layout fields change their MEANING with the
		// anchor, so a wrong scope here would hand somebody the sentence for a
		// field they are not looking at.
		{ "UI Hierarchy",  "Canvas##uiwroot" },
		{ "Canvas",        "Width" },
		{ "Canvas",        "Scale" },
		{ "UI Widget",     "Width" },
		{ "UI Widget",     "Left/Right" },
		{ "UI Widget",     "Clip children" },
		{ "UI Graph",      "Event Graph" },
		{ "UI Graph",      "Get" },
		{ "UI Graph Node", "(Any)" },
		{ "UI Graph Node", "Delete Node" },
		{ "UI Variable",   "Access" },
		{ "UI Variable",   "Scale##vdef" },
		// Input. "Bind" is drawn by a helper defined above every one of its
		// callers, so its scope is pushed inside that helper.
		{ "Input Action",  "Bind" },
		{ "Input Action",  "Auto Detect" },
		{ "Input Action",  "Press any input\xE2\x80\xA6" },
		{ "Input Action",  "Fires while the game is paused" },
		// Landscape. The panel has two mutually exclusive halves and each has a
		// "Radius": the creation form and the brush. Two scopes, and the brush's
		// three numbers are looked up by their "##paint"/"##brush" spelling and
		// fall back to the shared entry.
		{ "New Landscape",      "Seed" },
		{ "New Landscape",      "Create Landscape" },
		{ "Landscape",          "Radius##paint" },
		{ "Landscape",          "Radius##brush" },
		{ "Landscape",          "Weightmap##paint" },
		{ "Landscape",          "Reset Sculpting" },
		{ "Environment Window", "Select##sky" },
		{ "Environment Window", "Add Weather" },
		// Export, the build window and the profiler. Three of these labels carry
		// characters that are easy to get wrong in a key: two spaces before the
		// bracket, and a real em dash.
		{ "Export",       "Compile HorizonCode" },
		{ "Export",       "Metal" },
		{ "Export",       "Export" },
		{ "Build Window", "Start Game" },
		{ "Build Window", "Build Settings" },
		{ "Profiler",     "Target" },
		{ "Profiler",     "Stop & Dump  (F9)" },
		{ "Profiler",     "Start Benchmark Capture  (F9)" },
		{ "Profiler",     "Detailed GPU pass timing (serializes GPU — capture only)" },
		{ "Profiler",     "clear" },
		// The animator, audio and mesh tabs. A transition row's six labels are
		// looked up with their "##t" spelling and fall back to the visible name.
		{ "State Machine",             "Loop##st" },
		{ "State Machine",             "Add State" },
		{ "State Machine Transitions", "From##t" },
		{ "State Machine Transitions", "Thresh##t" },
		{ "State Machine Transitions", "Duration##t" },
		{ "State Machine Transitions", "+ Transition##sm" },
		{ "State Machine Parameters",  "+ Param" },
		{ "Sync Graph",                "Show node" },
		{ "Audio Editor",              "Pitch" },
		{ "Audio Editor",              "Import as Audio Asset" },
		{ "Mesh Viewer",               "Ground grid" },
		{ "Mesh Viewer",               "Studio" },
		{ "Mesh Viewer",               "Clip:" },
		// HorizonCode. Two of these scopes are pushed in files SHARED by four
		// editors (HcGraphHost, HcEditorUtil), so their entries have to read
		// correctly in a level script, a widget, a class and a sync graph alike.
		{ "Script Graph",               "Event Graph" },
		{ "Script Graph",               "Show node" },
		{ "Script Variable",            "Access" },
		{ "Script Variable",            "Scale##vdef" },
		{ "Script Node",                "Overridable" },
		{ "Script Node",                "Function" },
		{ "HorizonCode Event",          "Carries a value" },
		{ "HorizonCode Event",          "Declare" },
		{ "HorizonCode Node",           "Value" },
		{ "HorizonCode Node",           "Variable" },
		{ "HorizonCode Default Value",  "Pos##sd" },
		{ "HorizonCode Default Value",  "Pos##el" },
		{ "HorizonCode Default Value",  "Reset" },
		{ "HorizonCode Default Value",  "+ Add Slot" },
		{ "Class Components",           "Add Child" },
		{ "Type Editor",                "+ Add Field" },
		{ "Type Editor",                "Set as Project Default" },
		// Collaboration and source control. The git-remedy buttons are drawn by
		// helpers that BOTH the startup dialog and the Preferences page call, so
		// one scope has to serve both call sites.
		{ "Collaboration Session", "Display name" },
		{ "Collaboration Session", "Port" },
		{ "Collaboration Session", "Join code##lan" },
		{ "Collaboration Session", "Hand over" },
		{ "Collaboration Session", "Enable and join" },
		{ "Session Participants",  "Block" },
		{ "Block Participant",     "Block" },
		{ "Source Control Panel",  "New branch…" },
		{ "Source Control Panel",  "Restore project to this commit…" },
		{ "Source Control",        "Recheck" },
		{ "Source Control",        "Copy winget Command" },
		{ "Source Control",        "git-scm.com" },
		{ "Source Control",        "Save Identity" },
		{ "Report Issue",          "File on GitHub" },
		{ "Report Issue",          "Warnings & errors" },
		// The combos and radio buttons the coverage scan could not see until it
		// learnt about ImGui::BeginCombo and ImGui::RadioButton. They went eight
		// rounds unnoticed, which is the argument for checking them by hand here
		// as well.
		{ "HorizonCode Node",  "Struct" },
		{ "HorizonCode Node",  "Enum" },
		{ "HorizonCode Node",  "Cast to" },
		{ "Function Return",   "Function" },
		{ "Node Parameter",    "Scene" },
		{ "Graph Appearance",  "Compact" },
		{ "Input Action",      "Axis 2D" },
		{ "Export",            "Stop on failure" },
		{ "UI Widget",         "Hover cursor" },
		{ "UI Graph Node",     "Property" },
		{ "Script",            "Class" },
	};
	for (const Case& c : cases)
	{
		Help::Scope scope(c.scope);
		const Help::Entry* e = Help::find(c.label);
		CHECK_MESSAGE(e != nullptr, "no entry for ", std::string(c.scope) + "/" + c.label);
		if (!e) continue;
		// And it has to reach the manual, or F1 over it opens nothing.
		CHECK(!Help::referenceTopic(e->key).empty());
	}

	// The controls whose LABEL is data — a material's name, a paint layer's
	// name, a toolbar cell drawn through the draw list — name their key by hand
	// instead. A typo in one of those is invisible: the lookup simply returns
	// nothing and the control stays as silent as it was before.
	const char* byKey[] = {
		"Landscape/Material", "Landscape/Layer",
		"Landscape/Sculpt",   "Landscape/Paint",
		"Landscape/Raise",    "Landscape/Lower",   "Landscape/Smooth",
		"Landscape/Flatten",  "Landscape/Ramp",    "Landscape/Roughen",
		// The Source Control window's commit button says "Commit 3 changes", so
		// there is no fixed label either.
		"sc.commit",
	};
	for (const char* k : byKey)
	{
		const Help::Entry* e = Help::findKey(k);
		CHECK_MESSAGE(e != nullptr, "no entry for key ", std::string(k));
		if (e) CHECK(!Help::referenceTopic(e->key).empty());
	}
}

TEST_CASE("editor help: every topic it points at exists in the manual")
{
	Docs::Library lib = shipped();
	REQUIRE_MESSAGE(lib.loaded(), lib.error());

	int linked = 0;
	for (int i = 0; i < Help::entryCount(); ++i)
	{
		const Help::Entry& e = Help::entryAt(i);
		if (!e.topic || !e.topic[0]) continue;
		const std::string key(e.key), topic(e.topic);
		int page = -1, section = -1;
		CHECK_MESSAGE(lib.resolve(topic, page, section),
		              "help entry '", key, "' points at a page that does not exist: ", topic);
		// Resolving to the page but not the section means the anchor was
		// renamed on the website. The reader would still open, on the right
		// page but at the top — worth failing on, because it is silent.
		if (topic.find('#') != std::string::npos)
			CHECK_MESSAGE(section >= 0,
			              "help entry '", key, "' names a section that no longer exists: ",
			              topic);
		++linked;
	}
	INFO("help entries linked to the manual: " << linked);
	CHECK(linked > 100);
}

TEST_CASE("editor help: every \"Show me\" points at a real topic")
{
	Docs::Library lib = shipped();
	REQUIRE(lib.loaded());
	for (int i = 0; i < Help::panelTopicCount(); ++i)
	{
		const Help::PanelTopic& p = Help::panelTopicAt(i);
		const std::string topic(p.topic);
		int page = -1, section = -1;
		CHECK_MESSAGE(lib.resolve(topic, page, section),
		              "panel mapping for a topic that does not exist: ", topic);
		const bool hasWindow = p.window != nullptr && p.window[0] != '\0';
		CHECK(hasWindow);
		CHECK(p.menu != nullptr);
	}
	CHECK(Help::panelTopicCount() >= 8);
}

TEST_CASE("editor help: the scope is what tells two identical labels apart")
{
	// Bare, with no scope: "Mass" belongs to two different components and must
	// not resolve to whichever one happens to be first in the table.
	CHECK(Help::find("Mass") == nullptr);
	CHECK(Help::currentScope() == std::string(""));

	{
		Help::Scope scope("Rigid Body");
		CHECK(Help::currentScope() == std::string("Rigid Body"));
		const Help::Entry* e = Help::find("Mass");
		REQUIRE(e != nullptr);
		CHECK(std::string(e->key) == "Rigid Body/Mass");
		CHECK(std::string(e->body).find("kilogram") != std::string::npos);
	}
	CHECK(Help::currentScope() == std::string(""));

	{
		Help::Scope scope("Character Controller");
		const Help::Entry* e = Help::find("Mass (kg)");
		REQUIRE(e != nullptr);
		CHECK(std::string(e->key) == "Character Controller/Mass (kg)");
	}

	// Nesting has to restore, not clear — a component section inside a panel
	// inside a tab, and every one of them a scope.
	{
		Help::Scope outer("Light");
		{
			Help::Scope inner("Mesh");
			CHECK(Help::currentScope() == std::string("Mesh"));
		}
		CHECK(Help::currentScope() == std::string("Light"));
	}
}

TEST_CASE("editor help: the ##suffix is part of the key when a label repeats")
{
	Help::Scope scope("Environment");

	// The Sky component has a cloud "Density" and a fog "Density##fog". The
	// suffix is the only thing between them, so it is tried before the visible
	// label is fallen back on.
	const Help::Entry* fog = Help::find("Density##fog");
	REQUIRE(fog != nullptr);
	CHECK(std::string(fog->key) == "Environment/Density##fog");
	CHECK(std::string(fog->body).find("haze") != std::string::npos);

	const Help::Entry* cloud = Help::find("Density");
	REQUIRE(cloud != nullptr);
	CHECK(std::string(cloud->key) == "Environment/Density");
	CHECK(cloud != fog);

	// A row with a suffix that has no entry of its own still finds the entry for
	// its visible label — which is what lets a panel disambiguate ImGui ids
	// without an entry per spelling.
	const Help::Entry* coverage = Help::find("Coverage##whatever");
	REQUIRE(coverage != nullptr);
	CHECK(std::string(coverage->key) == "Environment/Coverage");
}

TEST_CASE("editor help: shortcuts are shown in the modifier the keyboard has")
{
	const std::string save = Help::shortcutLabel("Ctrl+S");
	const std::string both = Help::shortcutLabel("Ctrl+Shift+Ctrl");
#ifdef __APPLE__
	CHECK(save == "Cmd+S");
	CHECK(both == "Cmd+Shift+Cmd");
#else
	CHECK(save == "Ctrl+S");
	CHECK(both == "Ctrl+Shift+Ctrl");
#endif
	// Anything without a modifier is left exactly as written.
	CHECK(Help::shortcutLabel("F1") == "F1");
	CHECK(Help::shortcutLabel("") == "");
}

// ── The wiring, driven for real ──────────────────────────────────────────────
// Everything above tests the table. This tests the mechanism the table is
// useless without: that a plain labelled row inside a component's scope really
// does find its entry, hover really does queue it, and F1 really does come back
// with the topic to open. ImGui runs headless given a context and a display
// size, so all of that is drivable — including the hover delay, which is the
// part most likely to be silently wrong.
namespace
{
	struct ImGuiCtx
	{
		ImGuiCtx()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			// No renderer: claim texture support so ImGui never waits on a
			// backend to upload the font atlas (1.92's dynamic-font path).
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		}
		~ImGuiCtx() { ImGui::DestroyContext(); }
	};
} // namespace

TEST_CASE("editor help: a Details row explains itself with no call site of its own")
{
	ImGuiCtx guard;
	ImGuiIO& io = ImGui::GetIO();

	float value = 12.0f;
	ImVec2 control{ -1.0f, -1.0f };
	const char* topic = nullptr;

	// One frame of a Details-like panel: a component scope, one labelled row.
	// Exactly what InspectorPanel does, minus the entity.
	auto frame = [&](const char* scopeName) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(420.0f, 300.0f));
		ImGui::Begin("panel", nullptr,
		             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		{
			HE::Ed::Help::Scope scope(scopeName);
			EditorWidgets::Row::dragFloat("Mass", &value, 0.1f);
			const ImVec2 mn = ImGui::GetItemRectMin();
			const ImVec2 mx = ImGui::GetItemRectMax();
			control = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
		}
		ImGui::End();
		topic = EditorWidgets::drawQueuedHelp();
		ImGui::EndFrame();
	};

	// Away from the panel first, so the first hover is a real transition.
	io.AddMousePosEvent(1200.0f, 700.0f);
	frame("Rigid Body");
	REQUIRE(control.x > 0.0f);
	CHECK(topic == nullptr);

	// Now rest the pointer on the control. ImGui's tooltip flags require the
	// mouse to be STATIONARY for a moment before a tooltip is owed — which is
	// the behaviour that keeps a tooltip from flashing at everything the pointer
	// sweeps across — so this takes more than one frame.
	io.AddMousePosEvent(control.x, control.y);
	for (int i = 0; i < 40; ++i) frame("Rigid Body");

	// F1 while it is showing hands back the section of the manual to open — the
	// control's OWN entry in the generated reference. It used to hand back the
	// chapter the concept belongs to ("systems#physics"), which is what the
	// reference pages exist to replace: that chapter is now a link inside this
	// entry rather than the place F1 drops you.
	io.AddKeyEvent(ImGuiKey_F1, true);
	frame("Rigid Body");
	REQUIRE(topic != nullptr);
	CHECK(std::string(topic) == "editor-components#Rigid Body.Mass");
	io.AddKeyEvent(ImGuiKey_F1, false);
	frame("Rigid Body");

	// The same row in a component that has no "Mass" entry: nothing is queued,
	// so F1 over it opens nothing rather than the wrong page.
	for (int i = 0; i < 40; ++i) frame("Foliage");
	io.AddKeyEvent(ImGuiKey_F1, true);
	frame("Foliage");
	CHECK(topic == nullptr);
}

TEST_CASE("editor help: a topic without its own panel falls back to its page")
{
	// Mapped directly.
	const Help::PanelTopic* outliner = Help::panelForTopic("editor#outliner");
	REQUIRE(outliner != nullptr);
	CHECK(std::string(outliner->window) == "World Outliner");

	// A section of the editor manual with no mapping of its own: there is no
	// "editor#editor" entry, so this is a miss rather than a wrong window.
	CHECK(Help::panelForTopic("editor#preferences") == nullptr);
	CHECK(Help::panelForTopic("materials#nodes") == nullptr);
	CHECK(Help::panelForTopic("") == nullptr);
}

// ── The guides ───────────────────────────────────────────────────────────────
// The recipes are the part of the manual a reader opens first, and they have a
// failure mode nothing else reports: a section built without its flat search
// text is IN the manual and invisible to the search box. Someone typing
// "navmesh" then gets nothing, concludes the manual has no answer, and never
// finds the page that has it.
//
// So this asserts the two things that make a guide reachable at all — it is
// installed, and it can be found by typing a word out of it.
TEST_CASE("guides: every recipe is installed and findable by search")
{
	Docs::Library lib = shipped();
	REQUIRE_MESSAGE(lib.loaded(), lib.error());
	HE::Ed::Guides::install(lib);

	const std::vector<std::string>& ids = HE::Ed::Guides::pageIds();
	REQUIRE_FALSE(ids.empty());

	// Every id the panel will ask for resolves. The sidebar drops the ones that
	// do not, silently, so a typo here is a guide that is simply never listed.
	for (const std::string& id : ids)
		CHECK_MESSAGE(lib.pageIndex(id) >= 0,
		              ("guide page missing from the library: " + id).c_str());

	// Ids are unique: half of a topic reference is the page id, so two pages
	// sharing one makes every link into either of them ambiguous.
	std::set<std::string> unique(ids.begin(), ids.end());
	CHECK(unique.size() == ids.size());

	// Section ids are unique WITHIN a page, for the same reason one step further
	// down — a cross-link addresses "page#section".
	for (const std::string& id : ids)
	{
		const int pi = lib.pageIndex(id);
		if (pi < 0) continue;
		std::set<std::string> secIds;
		for (const Docs::Section& s : lib.pages()[static_cast<std::size_t>(pi)].sections)
			CHECK_MESSAGE(secIds.insert(s.id).second,
			              ("duplicate section id " + s.id + " on guide page " + id).c_str());
	}

	// And the search finds them. This is the assertion that catches a section
	// whose flat text was never filled: the page is there, the words are on
	// screen, and searching for them returns nothing.
	auto findsGuide = [&](const char* query)
	{
		for (const Docs::Hit& h : lib.search(query))
		{
			if (h.page < 0) continue;
			const std::string& pid = lib.pages()[static_cast<std::size_t>(h.page)].id;
			if (std::find(ids.begin(), ids.end(), pid) != ids.end()) return true;
		}
		return false;
	};
	CHECK_MESSAGE(findsGuide("navmesh"), "no guide found for \"navmesh\"");
	CHECK_MESSAGE(findsGuide("jump"),    "no guide found for \"jump\"");
	CHECK_MESSAGE(findsGuide("trigger"), "no guide found for \"trigger\"");
}
