#include "doctest.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>

#include "HcEditorUtil.h"
#include <HorizonCode/HorizonCode.h>

// ── Renaming a function keeps its calls ──────────────────────────────────────
// A HorizonCode function has no identity beyond its name: a FunctionCall and a
// FunctionReturn find their function by comparing strings. So renaming the
// FunctionEntry has to carry to them in the same breath, or the graph is left
// calling something that no longer exists — and nothing says so, because an
// unresolved call is a legal graph that simply does nothing.
//
// Both graph editors had that propagation written down, and in both it was
// dead. The first test here is the reason why, driven through a real ImGui
// InputText rather than argued from the source: it is a frame-timing trap, and
// reading the code is exactly how it survived review twice.
//
// ImGui needs no backend to RUN — a context, a display size and the input queue
// fed by hand are enough (the same trick tests/test_graph_editor_keys.cpp uses
// to press keys at the canvas), so the whole rename, typing included, happens
// here for real.

using namespace HorizonCode;

namespace
{

// One function and everything in the graph that names it: the entry, a call, a
// return — plus a call to a DIFFERENT function, which a rename must not touch.
// Node ids are 1-based and match the vector order, so the accessors below can
// be indices instead of a search.
Graph makeGraph()
{
	Graph g;
	Node e; e.id = 1; e.type = NodeType::FunctionEntry;  e.s = "Fire";
	Node c; c.id = 2; c.type = NodeType::FunctionCall;   c.s = "Fire";
	Node r; r.id = 3; r.type = NodeType::FunctionReturn; r.s = "Fire"; r.subgraph = 1;
	Node o; o.id = 4; o.type = NodeType::FunctionCall;   o.s = "Reload";
	g.nodes = { e, c, r, o };
	return g;
}

Node& entryOf(Graph& g)  { return g.nodes[0]; }
Node& callOf(Graph& g)   { return g.nodes[1]; }
Node& returnOf(Graph& g) { return g.nodes[2]; }
Node& otherOf(Graph& g)  { return g.nodes[3]; }

// ── A details panel with one row in it ───────────────────────────────────────
// `row` is the panel's Name row, handed in per test so the broken shape and the
// shipped one can be driven through the identical sequence of frames.
struct Panel
{
	Panel()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize  = ImVec2(640.0f, 480.0f);
		io.DeltaTime    = 1.0f / 60.0f;
		io.IniFilename  = nullptr;
		io.LogFilename  = nullptr;
		// No renderer: claim texture support so ImGui never waits on a backend to
		// upload the font atlas (1.92's dynamic-font path).
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~Panel() { ImGui::DestroyContext(); }

	ImVec2 field{ 0.0f, 0.0f };   // a point inside the Name field, from the last frame

	template <typename Row>
	void frame(const Row& row)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(320.0f, 200.0f));
		ImGui::Begin("details", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		row();
		// The row leaves the InputText as the last item. Its rect runs from the
		// frame's left edge to the right of the label, so aim near the left edge:
		// the centre of the whole item can land on the label.
		const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
		field = ImVec2(lo.x + 10.0f, (lo.y + hi.y) * 0.5f);
		ImGui::End();
		ImGui::Render();
	}

	void mouseTo(ImVec2 p)      { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
	void mouseDown(bool down)   { ImGui::GetIO().AddMouseButtonEvent(0, down); }
	void ch(char c)             { ImGui::GetIO().AddInputCharacter((unsigned int)(unsigned char)c); }
	void key(ImGuiKey k, bool d){ ImGui::GetIO().AddKeyEvent(k, d); }

	template <typename Row>
	void tap(const Row& row, ImGuiKey k)
	{
		key(k, true);  frame(row);
		key(k, false); frame(row);
	}

	// Click into the field, the way a person starts a rename.
	template <typename Row>
	void focusField(const Row& row)
	{
		for (int i = 0; i < 3; ++i) frame(row);   // hit-testing reads the previous frame's layout
		mouseTo(field);     frame(row);
		mouseDown(true);    frame(row);
		mouseDown(false);   frame(row);
		// Without this a test that never managed to focus anything would go on to
		// type into nothing and pass for the wrong reason.
		REQUIRE(ImGui::GetIO().WantTextInput);
	}

	// Rub the old name out and type a new one. No modifier key is involved on
	// purpose: with ConfigMacOSXBehaviors (the default on this platform) ImGui
	// swaps Ctrl and Cmd, and aliases a Ctrl+left click into a RIGHT click — so
	// "ctrl-click to select all" would do something different per platform, and
	// on macOS it never even presses the left button.
	template <typename Row>
	void retype(const Row& row, const char* text)
	{
		tap(row, ImGuiKey_End);
		for (int i = 0; i < 16; ++i) tap(row, ImGuiKey_Backspace);
		for (const char* p = text; *p; ++p) { ch(*p); frame(row); }
	}

	// Enter ends the edit. ImGui clears the active id at the END of that frame,
	// so IsItemDeactivatedAfterEdit() only fires on the NEXT one.
	template <typename Row>
	void pressEnter(const Row& row)
	{
		key(ImGuiKey_Enter, true);  frame(row);
		key(ImGuiKey_Enter, false); frame(row);
		frame(row);
	}

	// The other way a person ends an edit: click somewhere that is not the field.
	template <typename Row>
	void clickAway(const Row& row)
	{
		mouseTo(ImVec2(160.0f, 160.0f));   // empty space below the row
		frame(row);
		mouseDown(true);  frame(row);
		mouseDown(false); frame(row);
		frame(row);
	}
};

} // namespace

TEST_CASE("HorizonCode rename: a name re-read every frame is never the old name")
{
	// The shape both graph editors shipped. It reads as if it works, and the
	// only thing wrong with it is WHEN each line runs: ImGui::InputText writes
	// into the bound string on every keystroke, so by the frame the edit ends
	// `n->s` is already the new name — and `oldName`, captured at the top of that
	// same frame, is the new name too. The comparison can never differ, the loop
	// can never run, and the calls are left pointing at a function that is gone.
	Panel p;
	Graph g = makeGraph();

	auto row = [&]
	{
		Node& e = entryOf(g);
		std::string oldName = e.s;
		ImGui::InputText("Name", &e.s);
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (!e.s.empty() && e.s != oldName)
				for (Node& c : g.nodes)
					if ((c.type == NodeType::FunctionCall || c.type == NodeType::FunctionReturn) &&
					    c.s == oldName)
						c.s = e.s;
		}
	};

	p.focusField(row);
	p.retype(row, "Ignite");
	p.pressEnter(row);

	// The typing itself landed — this is a rename that happened, not a test that
	// failed to press anything.
	REQUIRE(entryOf(g).s == "Ignite");
	// And this is the bug: everything that called it still names the old one.
	CHECK(callOf(g).s == "Fire");
	CHECK(returnOf(g).s == "Fire");
}

TEST_CASE("HorizonCode rename: the entry's new name carries to Call and Return")
{
	// The shipped shape: the field edits a scratch buffer, and the rename is
	// committed in one step when the edit ends — at which point the node still
	// holds the OLD name, which is the whole point.
	Panel p;
	Graph g = makeGraph();
	HcEditorUtil::FnNameEdit ed;
	bool committed = false;

	auto row = [&]
	{
		Node& e = entryOf(g);
		HcEditorUtil::seedFunctionName(e, ed);
		ImGui::InputText("Name", &ed.buf);
		if (ImGui::IsItemDeactivatedAfterEdit())
			committed |= HcEditorUtil::commitFunctionName(g, e, ed);
	};

	SUBCASE("committed with Enter")
	{
		p.focusField(row);
		p.retype(row, "Ignite");
		p.pressEnter(row);
	}
	SUBCASE("committed by clicking away")
	{
		p.focusField(row);
		p.retype(row, "Ignite");
		p.clickAway(row);
	}

	REQUIRE(committed);                    // the commit branch really ran
	CHECK(entryOf(g).s  == "Ignite");
	CHECK(callOf(g).s   == "Ignite");
	CHECK(returnOf(g).s == "Ignite");
	CHECK(otherOf(g).s  == "Reload");      // a call to another function is left alone
	// The field goes on showing the name it committed, instead of being re-seeded
	// from a node it just changed.
	CHECK(ed.buf == "Ignite");
}

TEST_CASE("HorizonCode rename: the scratch buffer follows the panel, not the keystrokes")
{
	Graph g = makeGraph();
	HcEditorUtil::FnNameEdit ed;

	// First frame on a node: the field shows what the node is called.
	HcEditorUtil::seedFunctionName(entryOf(g), ed);
	CHECK(ed.buf == "Fire");

	// Half-typed text survives the frames that follow — re-seeding here is what
	// made the obvious fix (buffer refilled every frame) drop every character.
	ed.buf = "Ign";
	HcEditorUtil::seedFunctionName(entryOf(g), ed);
	CHECK(ed.buf == "Ign");

	// A different function is shown: the buffer belongs to that one now, and
	// whatever was half-typed for the previous one is gone rather than committed
	// onto it.
	Node second; second.id = 7; second.type = NodeType::FunctionEntry; second.s = "Reload";
	HcEditorUtil::seedFunctionName(second, ed);
	CHECK(ed.buf == "Reload");

	// Same id, different name: the node was renamed behind the panel's back —
	// an undo, a collaborator, a graph that reuses the id. Keying the buffer on
	// the id alone would leave the field showing a name nothing has.
	second.s = "Holster";
	HcEditorUtil::seedFunctionName(second, ed);
	CHECK(ed.buf == "Holster");
}

TEST_CASE("HorizonCode rename: what a commit does and does not touch")
{
	SUBCASE("nothing typed, nothing committed")
	{
		Graph g = makeGraph();
		HcEditorUtil::FnNameEdit ed;
		HcEditorUtil::seedFunctionName(entryOf(g), ed);
		CHECK_FALSE(HcEditorUtil::commitFunctionName(g, entryOf(g), ed));
		CHECK(entryOf(g).s == "Fire");
	}

	SUBCASE("cleared to nothing: the calls keep the name they had")
	{
		// Renaming every call to "" would point them all at the unnamed function
		// the entry has just become — silently binding calls that were fine.
		Graph g = makeGraph();
		HcEditorUtil::FnNameEdit ed;
		HcEditorUtil::seedFunctionName(entryOf(g), ed);
		ed.buf.clear();
		CHECK(HcEditorUtil::commitFunctionName(g, entryOf(g), ed));
		CHECK(entryOf(g).s.empty());
		CHECK(callOf(g).s   == "Fire");
		CHECK(returnOf(g).s == "Fire");
	}

	SUBCASE("naming a function that never had one does not adopt loose calls")
	{
		// A freshly dropped Call node has no function picked yet, which is the
		// same empty string. Matching on it would hand every unset call in the
		// graph to whichever function got named first.
		Graph g = makeGraph();
		entryOf(g).s.clear();
		callOf(g).s.clear();
		HcEditorUtil::FnNameEdit ed;
		HcEditorUtil::seedFunctionName(entryOf(g), ed);
		ed.buf = "Ignite";
		CHECK(HcEditorUtil::commitFunctionName(g, entryOf(g), ed));
		CHECK(entryOf(g).s == "Ignite");
		CHECK(callOf(g).s.empty());
	}

	SUBCASE("a second function keeping its own name is not renamed along")
	{
		Graph g = makeGraph();
		Node twin; twin.id = 5; twin.type = NodeType::FunctionEntry; twin.s = "Reload";
		g.nodes.push_back(twin);
		HcEditorUtil::FnNameEdit ed;
		HcEditorUtil::seedFunctionName(entryOf(g), ed);
		ed.buf = "Ignite";
		CHECK(HcEditorUtil::commitFunctionName(g, entryOf(g), ed));
		CHECK(g.nodes[4].s == "Reload");
		CHECK(otherOf(g).s == "Reload");
	}
}
