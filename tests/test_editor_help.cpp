#include "doctest.h"

#include "DocsLibrary.h"
#include "EditorHelp.h"
#include "EditorReference.h"
#include "EditorWidgets.h"

#include <imgui.h>

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
