#include "doctest.h"
#include <imgui.h>
#include <string>
#include <vector>

#include "GraphEditor.h"

// ── Headless canvas driver ───────────────────────────────────────────────────
// The graph shortcuts were shipped once without any way to press a key at them:
// the editor's ImGui surface is invisible to the headless renderer dump, so
// "hold B and click" could only ever be checked by hand — and it turned out to
// be dead on arrival. ImGui itself needs no backend, though: with a context, a
// display size and the input queue fed by hand, GraphEditor::draw runs exactly
// as it does in the editor. That is what this harness does, so every shortcut
// has a test that actually presses it.
//
// Frames matter here. ImGui decides which window the mouse is over from the
// PREVIOUS frame's layout, so a click only registers on the canvas once a frame
// has already placed it — every helper below runs enough frames for that.

namespace
{
struct TestNode { int id; float x, y; };

// A toy graph + model: three nodes, one link, and one quick-spawn key (B) that
// appends a node and records what the canvas handed it.
struct Harness
{
	std::vector<TestNode>            nodes;
	std::vector<std::array<int,4>>   links;
	GraphEditor::State               state;
	GraphEditor::Model               model;

	int  spawnCalls = 0;
	GraphEditor::QuickSpawnCtx lastSpawn;
	int  addMenuCalls = 0;
	int  nextId = 1;
	// Give every node a SECOND input pin (id 2, one row below the first). The
	// hit-test cases need two neighbouring targets to prove the nearest centre
	// wins; the shortcut cases leave it off and keep the original two-pin node.
	bool twoInputs = false;
	// Pin ids the host was asked to clear (Alt+click on a pin).
	std::vector<int> clearedPins;

	// Two pins per node: one exec-in (id 0) and one exec-out (id 1).
	std::vector<GraphEditor::Pin> pinsOf() const
	{
		std::vector<GraphEditor::Pin> p = {
			{ 0, "In",  IM_COL32_WHITE, true,  true },
			{ 1, "Out", IM_COL32_WHITE, false, true },
		};
		if (twoInputs) p.push_back({ 2, "In2", IM_COL32_WHITE, true, true });
		return p;
	}

	Harness()
	{
		nodes = { { 1, 40.0f, 40.0f }, { 2, 300.0f, 160.0f }, { 3, 300.0f, 300.0f } };
		nextId = 4;
		links  = { { 1, 1, 2, 0 } };

		model.multiSelect = true;
		model.nodeIds = [this]{ std::vector<int> ids; for (auto& n : nodes) ids.push_back(n.id); return ids; };
		model.getPos  = [this](int id, float& x, float& y){ for (auto& n : nodes) if (n.id == id) { x = n.x; y = n.y; } };
		model.setPos  = [this](int id, float x, float y){ for (auto& n : nodes) if (n.id == id) { n.x = x; n.y = y; } };
		model.title   = [](int id){ return "N" + std::to_string(id); };
		model.headerColor = [](int){ return IM_COL32(80, 80, 90, 255); };
		model.pins    = [this](int){ return pinsOf(); };
		model.clearPinLinks = [this](int, int pin, bool){ clearedPins.push_back(pin); };
		model.links   = [this]{ return links; };
		model.connect = [this](int oN, int oP, int iN, int iP){ links.push_back({ oN, oP, iN, iP }); return true; };
		model.removeNode = [this](int id){
			for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].id == id) { nodes.erase(nodes.begin() + i); break; } };
		model.drawAddMenu = [this]() -> int { ++addMenuCalls; return 0; };

		GraphEditor::QuickSpawn qs;
		qs.key   = ImGuiKey_B;
		qs.spawn = [this](const GraphEditor::QuickSpawnCtx& c) {
			++spawnCalls; lastSpawn = c;
			nodes.push_back({ nextId, c.pos.x, c.pos.y });
			return nextId++;
		};
		model.quickSpawns.push_back(std::move(qs));
	}

	const TestNode* find(int id) const
	{
		for (const auto& n : nodes) if (n.id == id) return &n;
		return nullptr;
	}
};

// The canvas rect inside the test window: the window is placed at (0,0) and the
// canvas fills it below the title bar. Screen point of a graph point:
// screen = canvasOrigin + pan + graph*zoom.
constexpr float kCanvasW = 800.0f, kCanvasH = 600.0f;

struct Ctx
{
	ImVec2 canvasOrigin{ 0, 0 };
	// Draw a text field above the canvas (what a node rename box or a variable
	// name is); the test clicks it to make it take the keyboard.
	bool   textField = false;
	ImVec2 textFieldCenter{ 0, 0 };
	// A drag-drop source above the canvas carrying this payload type (nullptr = none).
	const char* sourceType = nullptr;
	ImVec2      sourceCenter{ 0, 0 };

	Ctx()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize  = ImVec2(1280.0f, 720.0f);
		io.DeltaTime    = 1.0f / 60.0f;
		io.IniFilename  = nullptr;
		io.LogFilename  = nullptr;
		// No renderer: claim texture support so ImGui never waits on a backend
		// to upload the font atlas (1.92's dynamic-font path).
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~Ctx() { ImGui::DestroyContext(); }

	// Run one frame of the canvas. Returns GraphEditor::draw's "graph changed".
	bool frame(Harness& h)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(kCanvasW + 20.0f, kCanvasH + 40.0f));
		ImGui::Begin("canvas", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		if (textField)
		{
			static char buf[32] = "";
			ImGui::InputText("##rename", buf, sizeof buf);
			textFieldCenter = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
			                         (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
		}
		if (sourceType)
		{
			// A side-list row that drags a payload (what LevelScriptPanel's
			// variable rows are), for the drag-cue payload cases.
			ImGui::Button("##src", ImVec2(80.0f, 20.0f));
			sourceCenter = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
			                      (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
			if (ImGui::BeginDragDropSource())
			{
				const int v = 42;
				ImGui::SetDragDropPayload(sourceType, &v, sizeof v);
				ImGui::EndDragDropSource();
			}
		}
		canvasOrigin = ImGui::GetCursorScreenPos();
		const bool changed = GraphEditor::draw("##canvas", h.model, h.state, ImVec2(kCanvasW, kCanvasH));
		ImGui::End();
		ImGui::Render();
		return changed;
	}

	// Settle the layout so the canvas is hoverable (ImGui hit-tests against the
	// previous frame), with the mouse parked on the given screen point.
	void settle(Harness& h, ImVec2 mouse)
	{
		for (int i = 0; i < 3; ++i)
		{
			ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
			frame(h);
		}
	}

	ImVec2 toScreen(const Harness& h, float gx, float gy) const
	{
		return ImVec2(canvasOrigin.x + h.state.pan.x + gx * h.state.zoom,
		              canvasOrigin.y + h.state.pan.y + gy * h.state.zoom);
	}

	// ImGui derives io.KeyCtrl/KeyShift ONLY from the ImGuiMod_* keys, which
	// backends send alongside the physical key (imgui_impl_sdl3.cpp does exactly
	// this) — sending LeftCtrl alone leaves io.KeyCtrl false.
	void key(ImGuiKey k, bool down)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl)  io.AddKeyEvent(ImGuiMod_Ctrl,  down);
		if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) io.AddKeyEvent(ImGuiMod_Shift, down);
		io.AddKeyEvent(k, down);
	}
	void mouse(ImVec2 p)            { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
	void button(bool down)          { ImGui::GetIO().AddMouseButtonEvent(0, down); }

	// Press and release a key over the canvas (no click involved).
	void tapKey(Harness& h, ImGuiKey k)
	{
		key(k, true);  frame(h);
		key(k, false); frame(h);
	}
};

// A point on empty canvas: far right/below the three seeded nodes.
ImVec2 emptySpot(const Ctx& c, const Harness& h) { return c.toScreen(h, 560.0f, 420.0f); }
} // namespace

TEST_CASE("GraphEditor shortcuts: hold a key and click drops a node")
{
	Ctx ctx;
	Harness h;
	const ImVec2 spot = emptySpot(ctx, h);
	ctx.settle(h, spot);

	// Hold B, then click empty canvas.
	ctx.key(ImGuiKey_B, true);
	ctx.frame(h);
	ctx.button(true);
	const bool changed = ctx.frame(h);   // the press frame — the spawn fires here
	ctx.button(false);
	ctx.frame(h);
	ctx.key(ImGuiKey_B, false);
	ctx.frame(h);

	REQUIRE(h.spawnCalls == 1);
	CHECK(changed);                       // the host is told to snapshot for undo
	CHECK(h.lastSpawn.linkNode == 0);     // no link drag was involved
	CHECK(h.nodes.size() == 4);
	// Dropped at the cursor, and left selected so it can be dragged straight away.
	const TestNode* fresh = h.find(4);
	REQUIRE(fresh != nullptr);
	CHECK(fresh->x == doctest::Approx(560.0f).epsilon(0.05));
	CHECK(fresh->y == doctest::Approx(420.0f).epsilon(0.05));
	CHECK(h.state.selected == 4);
}

TEST_CASE("GraphEditor shortcuts: a click without the key still box-selects")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	ctx.button(true);
	ctx.frame(h);
	ctx.button(false);
	ctx.frame(h);

	CHECK(h.spawnCalls == 0);
	CHECK(h.nodes.size() == 3);
}

TEST_CASE("GraphEditor shortcuts: a key hit mid link-drag spawns pre-wired")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	// Grab node 1's exec-OUT pin. Pin rows sit below the title bar; the layout
	// puts the first output at the node's right edge, half a row down.
	const ImVec2 pin = ctx.toScreen(h, 40.0f + GraphEditor::kNodeW,
	                                   40.0f + GraphEditor::kTitleH + GraphEditor::kRowH * 0.5f);
	ctx.mouse(pin);
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);                     // press on the pin starts the link drag
	REQUIRE(h.state.linkSrcNode == 1);

	// Drag out to empty canvas and hit B instead of releasing.
	const ImVec2 drop = emptySpot(ctx, h);
	ctx.mouse(drop);
	ctx.frame(h);
	ctx.key(ImGuiKey_B, true);
	ctx.frame(h);
	ctx.key(ImGuiKey_B, false);
	ctx.button(false);
	ctx.frame(h);

	REQUIRE(h.spawnCalls == 1);
	CHECK(h.lastSpawn.linkNode == 1);      // the dragged pin came along…
	CHECK(h.lastSpawn.linkPin  == 1);
	CHECK(h.lastSpawn.linkInput == false);
	CHECK(h.state.linkSrcNode == 0);       // …and the drag ended with it
}

TEST_CASE("GraphEditor shortcuts: Space opens the add palette")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	ctx.key(ImGuiKey_Space, true);
	ctx.frame(h);
	ctx.key(ImGuiKey_Space, false);
	ctx.frame(h);

	CHECK(h.addMenuCalls > 0);
}

TEST_CASE("GraphEditor shortcuts: Ctrl+A selects every node")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	ctx.key(ImGuiKey_LeftCtrl, true);
	ctx.key(ImGuiKey_A, true);
	ctx.frame(h);
	ctx.key(ImGuiKey_A, false);
	ctx.key(ImGuiKey_LeftCtrl, false);
	ctx.frame(h);

	CHECK(h.state.selection.size() == 3);
}

TEST_CASE("GraphEditor shortcuts: Home frames the graph, F frames the selection")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	const ImVec2 panBefore = h.state.pan;
	ctx.tapKey(h, ImGuiKey_Home);
	CHECK((h.state.pan.x != panBefore.x || h.state.pan.y != panBefore.y));

	// F with a selection centres on it: node 3 sits low, so the pan must differ
	// from the whole-graph fit.
	h.state.selection = { 3 };
	h.state.selected  = 3;
	const ImVec2 panFramedAll = h.state.pan;
	ctx.tapKey(h, ImGuiKey_F);
	CHECK((h.state.pan.x != panFramedAll.x || h.state.pan.y != panFramedAll.y));
}

TEST_CASE("GraphEditor shortcuts: F + click drops a node instead of framing")
{
	Ctx ctx;
	Harness h;
	// Bind F as well, so this graph has the For Each key AND the framing tap.
	GraphEditor::QuickSpawn qs;
	qs.key   = ImGuiKey_F;
	qs.spawn = [&h](const GraphEditor::QuickSpawnCtx& c) {
		++h.spawnCalls; h.lastSpawn = c;
		h.nodes.push_back({ h.nextId, c.pos.x, c.pos.y });
		return h.nextId++;
	};
	h.model.quickSpawns.push_back(std::move(qs));

	ctx.settle(h, emptySpot(ctx, h));
	const ImVec2 panBefore = h.state.pan;

	ctx.key(ImGuiKey_F, true);
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	ctx.button(false);
	ctx.frame(h);
	ctx.key(ImGuiKey_F, false);
	ctx.frame(h);

	CHECK(h.spawnCalls == 1);
	// The release must NOT also frame the view — the click already claimed it.
	CHECK(h.state.pan.x == doctest::Approx(panBefore.x));
	CHECK(h.state.pan.y == doctest::Approx(panBefore.y));
}

TEST_CASE("GraphEditor shortcuts: Q straightens a wire")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	// Nodes 1 → 2 are wired but vertically offset; selecting both and hitting Q
	// must line the destination up with the source pin.
	h.state.selection = { 1, 2 };
	h.state.selected  = 1;
	const float before = h.find(2)->y;
	ctx.tapKey(h, ImGuiKey_Q);
	const float after = h.find(2)->y;
	CHECK(after != before);
	// Both pins are the first row of their side, so a straight wire means equal
	// node tops.
	CHECK(after == doctest::Approx(h.find(1)->y));
}

TEST_CASE("GraphEditor shortcuts: a text field takes the keyboard away")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	// A real text field the user clicked into, not a hand-set flag:
	// io.WantTextInput is an OUTPUT recomputed every frame, so faking it proves
	// nothing.
	ctx.textField = true;
	ctx.frame(h);
	ctx.frame(h);
	ctx.mouse(ctx.textFieldCenter);
	ctx.frame(h);
	ctx.button(true);  ctx.frame(h);
	ctx.button(false); ctx.frame(h);
	REQUIRE(ImGui::GetIO().WantTextInput);

	// Back over the canvas, hold B and click: the field owns the keyboard, so
	// this must type into it rather than drop a node.
	ctx.mouse(emptySpot(ctx, h));
	ctx.frame(h);
	ctx.key(ImGuiKey_B, true);
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	ctx.button(false);
	ctx.key(ImGuiKey_B, false);
	ctx.frame(h);

	CHECK(h.spawnCalls == 0);
}

// ── Hit geometry ─────────────────────────────────────────────────────────────
// The same headless canvas, driven at the pixel level instead of the key level.
// A pin is drawn as a 5 px dot centred ON the node's edge, so half of it hangs
// over empty canvas — and the pin test used to run only for a cursor INSIDE the
// node rectangle, which made that outer half dead. On an input pin that is the
// half the cursor arrives from, so the pins hardest to hit were the ones a wire
// ends at. Nothing here can be seen in a screenshot; the mouse is the only
// instrument that can tell a live target from a dead one.

namespace
{
// Centre of a node's Nth input pin, in screen space: the left edge, half a row
// below the title bar. Same rule the canvas lays pins out with.
ImVec2 inputPin(const Ctx& c, const Harness& h, const TestNode& n, int row)
{
	return c.toScreen(h, n.x,
		n.y + GraphEditor::kTitleH + (static_cast<float>(row) + 0.5f) * GraphEditor::kRowH);
}
ImVec2 outputPin(const Ctx& c, const Harness& h, const TestNode& n, int row)
{
	return c.toScreen(h, n.x + GraphEditor::kNodeW,
		n.y + GraphEditor::kTitleH + (static_cast<float>(row) + 0.5f) * GraphEditor::kRowH);
}
} // namespace

TEST_CASE("GraphEditor hit-test: the outer half of an input pin is live")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode* n3 = h.find(3);
	REQUIRE(n3 != nullptr);
	// 8 px LEFT of the pin's centre: inside the hit radius, outside the node
	// box. Before the pin test moved out of the node loop this press fell
	// through to the canvas and started a box-select.
	const ImVec2 pin = inputPin(ctx, h, *n3, 0);
	REQUIRE(8.0f < GraphEditor::pinHitRadius(h.state.zoom));
	ctx.mouse(ImVec2(pin.x - 8.0f, pin.y));
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);

	REQUIRE(h.state.linkSrcNode == 3);
	CHECK(h.state.linkSrcPin == 0);
	CHECK(h.state.linkSrcInput);
	CHECK_FALSE(h.state.boxSel);        // the same press must not do both

	// Drop it on node 1's output → a real link, oriented output→input.
	const TestNode* n1 = h.find(1);
	REQUIRE(n1 != nullptr);
	ctx.mouse(outputPin(ctx, h, *n1, 0));
	ctx.frame(h);
	ctx.button(false);
	ctx.frame(h);

	REQUIRE(h.links.size() == 2);
	CHECK(h.links.back() == std::array<int,4>{ 1, 1, 3, 0 });
}

TEST_CASE("GraphEditor hit-test: a click between two pins takes the closer one")
{
	Ctx ctx;
	Harness h;
	h.twoInputs = true;                 // two input rows, kRowH apart
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode* n2 = h.find(2);
	REQUIRE(n2 != nullptr);
	const ImVec2 first  = inputPin(ctx, h, *n2, 0);
	const ImVec2 second = inputPin(ctx, h, *n2, 1);
	REQUIRE(second.y - first.y == doctest::Approx(GraphEditor::kRowH));

	// 12 px below the first pin is 8 px above the second: both are within reach
	// at low zoom, and the closer centre has to win.
	ctx.mouse(ImVec2(first.x, first.y + 12.0f));
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);

	REQUIRE(h.state.linkSrcNode == 2);
	CHECK(h.state.linkSrcPin == 2);     // the second input, not the first
	ctx.button(false);
	ctx.frame(h);
}

TEST_CASE("GraphEditor hit-test: pins stay separable when the canvas is zoomed out")
{
	Ctx ctx;
	Harness h;
	h.twoInputs = true;
	h.state.zoom = 0.4f;                // rows only 8 px apart on screen
	ctx.settle(h, emptySpot(ctx, h));

	// The floor keeps the target hittable at all, which necessarily makes the
	// two circles overlap — the point of the nearest-centre rule.
	REQUIRE(GraphEditor::pinHitRadius(0.4f) > GraphEditor::kRowH * 0.4f);

	const TestNode* n2 = h.find(2);
	REQUIRE(n2 != nullptr);
	const ImVec2 second = inputPin(ctx, h, *n2, 1);
	ctx.mouse(ImVec2(second.x, second.y + 1.0f));
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);

	REQUIRE(h.state.linkSrcNode == 2);
	CHECK(h.state.linkSrcPin == 2);
	ctx.button(false);
	ctx.frame(h);
}

TEST_CASE("GraphEditor hit-test: the node border belongs to the node")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode* n2 = h.find(2);
	REQUIRE(n2 != nullptr);
	// Just outside the top-left corner of the box, and far enough above the
	// first pin row that no pin is in reach — a click here used to deselect.
	const ImVec2 corner = ctx.toScreen(h, n2->x, n2->y);
	ctx.mouse(ImVec2(corner.x - 2.0f, corner.y + 2.0f));
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	ctx.button(false);
	ctx.frame(h);

	CHECK(h.state.selected == 2);
	CHECK(h.state.linkSrcNode == 0);
}

TEST_CASE("GraphEditor hit-test: Alt+click on a pin still clears its links")
{
	Ctx ctx;
	Harness h;
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode* n2 = h.find(2);
	REQUIRE(n2 != nullptr);
	const ImVec2 pin = inputPin(ctx, h, *n2, 0);
	ctx.mouse(ImVec2(pin.x - 6.0f, pin.y));
	ctx.frame(h);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	ctx.button(false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	ctx.frame(h);

	REQUIRE(h.clearedPins.size() == 1);
	CHECK(h.clearedPins.front() == 0);
	CHECK(h.state.linkSrcNode == 0);    // clearing is not the start of a drag
	CHECK(h.state.dragNode == 0);       // …and it must not move the node either
}

// ── Drag cues (EditorDragCues.h, topic 140) ──────────────────────────────────
// The canvas reports pickup / over a pin that fits or not / drop / cancel
// through Model::onDragCue, once per event. These press the real gestures and
// read the sequence; the last case checks the drag itself does exactly the same
// with and without the callback.

namespace
{
using Cue = GraphEditor::DragCue;

struct CueLog
{
	std::vector<Cue> cues;
	void attach(Harness& h) { h.model.onDragCue = [this](Cue c){ cues.push_back(c); }; }
};

// Press on `from`, move through `path` (two frames each, so a pin is entered
// once and then rested on), release on the last point.
void dragWire(Ctx& ctx, Harness& h, ImVec2 from, const std::vector<ImVec2>& path)
{
	ctx.mouse(from);
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	for (const ImVec2& p : path) { ctx.mouse(p); ctx.frame(h); ctx.frame(h); }
	ctx.button(false);
	ctx.frame(h);
	ctx.frame(h);
}
} // namespace

TEST_CASE("GraphEditor drag cues: pickup, hover edges and a drop")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	// Node 2 refuses this wire; everything else fits.
	h.model.canConnect = [](int, int, int iN, int){ return iN != 2; };
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode& n1 = *h.find(1);
	const TestNode& n2 = *h.find(2);
	const TestNode& n3 = *h.find(3);
	dragWire(ctx, h, outputPin(ctx, h, n1, 0), {
		inputPin(ctx, h, n3, 0),    // fits
		inputPin(ctx, h, n2, 0),    // canConnect says no
		outputPin(ctx, h, n2, 0),   // output on output: wrong side
		emptySpot(ctx, h),          // leaving a pin is silent
		inputPin(ctx, h, n3, 0),    // fits again — and released here
	});

	const std::vector<Cue> want = { Cue::Pickup, Cue::OverValid, Cue::OverInvalid,
	                                Cue::OverInvalid, Cue::OverValid, Cue::Drop };
	CHECK(log.cues == want);
	REQUIRE(h.links.size() == 2);
	CHECK(h.links.back() == std::array<int,4>{ 1, 1, 3, 0 });
}

TEST_CASE("GraphEditor drag cues: a refused drop and a drop on its own node cancel")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	h.model.connect = [&h](int oN, int oP, int iN, int iP){
		if (iN == 2) return false;           // the host refuses (a type mismatch)
		h.links.push_back({ oN, oP, iN, iP });
		return true; };
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode& n1 = *h.find(1);
	const TestNode& n2 = *h.find(2);
	dragWire(ctx, h, outputPin(ctx, h, n1, 0), { inputPin(ctx, h, n2, 0) });
	// canConnect unset: by side alone it looked fine; the release says no.
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::OverValid, Cue::Cancel });
	CHECK(h.links.size() == 1);

	log.cues.clear();
	dragWire(ctx, h, outputPin(ctx, h, n1, 0), { inputPin(ctx, h, n1, 0) });
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::OverInvalid, Cue::Cancel });
	CHECK(h.links.size() == 1);
}

TEST_CASE("GraphEditor drag cues: a detached wire let go in empty space cancels")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	ctx.settle(h, emptySpot(ctx, h));

	// Node 2's input is wired (1 → 2): grabbing it lifts the wire off.
	dragWire(ctx, h, inputPin(ctx, h, *h.find(2), 0), { emptySpot(ctx, h) });
	CHECK(h.clearedPins == std::vector<int>{ 0 });
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::Cancel });
}

TEST_CASE("GraphEditor drag cues: the drag-off menu drops or cancels when it ends")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	int menuFrames = 0;
	bool pick = true;
	h.model.drawPinDragMenu = [&](int, int, bool, ImVec2) -> int {
		// Open for a couple of frames, then the user picks (or not).
		if (++menuFrames < 3) return 0;
		ImGui::CloseCurrentPopup();
		if (!pick) return 0;
		h.nodes.push_back({ h.nextId, 500.0f, 400.0f });
		return h.nextId++; };
	ctx.settle(h, emptySpot(ctx, h));

	dragWire(ctx, h, outputPin(ctx, h, *h.find(1), 0), { emptySpot(ctx, h) });
	// Released: the menu is up, and that is no cue yet.
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup });
	for (int i = 0; i < 4; ++i) ctx.frame(h);
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::Drop });

	log.cues.clear();
	menuFrames = 0;
	pick = false;
	dragWire(ctx, h, outputPin(ctx, h, *h.find(1), 0), { emptySpot(ctx, h) });
	for (int i = 0; i < 4; ++i) ctx.frame(h);
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::Cancel });
}

TEST_CASE("GraphEditor drag cues: a quick-spawn key mid-drag drops")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	ctx.settle(h, emptySpot(ctx, h));

	ctx.mouse(outputPin(ctx, h, *h.find(1), 0));
	ctx.frame(h);
	ctx.button(true);
	ctx.frame(h);
	ctx.mouse(emptySpot(ctx, h));
	ctx.frame(h);
	ctx.key(ImGuiKey_B, true);
	ctx.frame(h);
	ctx.key(ImGuiKey_B, false);
	ctx.button(false);
	ctx.frame(h);
	ctx.frame(h);

	REQUIRE(h.spawnCalls == 1);
	CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::Drop });
}

TEST_CASE("GraphEditor drag cues: moving a node and clicking the canvas stay silent")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	ctx.settle(h, emptySpot(ctx, h));

	const TestNode& n3 = *h.find(3);
	const ImVec2 body = ctx.toScreen(h, n3.x + 40.0f, n3.y + 6.0f);   // the title bar
	dragWire(ctx, h, body, { ImVec2(body.x + 60.0f, body.y + 30.0f) });
	CHECK(h.find(3)->x != doctest::Approx(300.0f));   // it did move
	dragWire(ctx, h, emptySpot(ctx, h), { ImVec2(emptySpot(ctx, h).x + 30.0f, emptySpot(ctx, h).y + 20.0f) });
	CHECK(log.cues.empty());
}

TEST_CASE("GraphEditor drag cues: a payload from a side list")
{
	Ctx ctx;
	Harness h;
	CueLog log;
	log.attach(h);
	int drops = 0;
	h.model.dropPayloads = { "HC_TEST_VAR" };
	h.model.onDrop = [&drops](const char*, const void* data, ImVec2){
		CHECK(*static_cast<const int*>(data) == 42);
		++drops; };

	auto dragFromSource = [&](ImVec2 to) {
		ctx.mouse(ctx.sourceCenter);
		ctx.frame(h);
		ctx.button(true);
		ctx.frame(h);
		ctx.mouse(ImVec2(ctx.sourceCenter.x + 20.0f, ctx.sourceCenter.y + 20.0f));
		ctx.frame(h);
		ctx.frame(h);
		ctx.mouse(to);
		ctx.frame(h);
		ctx.frame(h);
		ctx.button(false);
		ctx.frame(h);
		ctx.frame(h);
		ctx.frame(h);
	};

	SUBCASE("dropped on the canvas")
	{
		ctx.sourceType = "HC_TEST_VAR";
		ctx.settle(h, emptySpot(ctx, h));
		dragFromSource(emptySpot(ctx, h));
		CHECK(drops == 1);
		CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::OverValid, Cue::Drop });
	}
	SUBCASE("let go outside the canvas")
	{
		ctx.sourceType = "HC_TEST_VAR";
		ctx.settle(h, emptySpot(ctx, h));
		dragFromSource(ImVec2(1100.0f, 680.0f));   // outside the canvas window
		CHECK(drops == 0);
		CHECK(log.cues == std::vector<Cue>{ Cue::Pickup, Cue::Cancel });
	}
	SUBCASE("a type the canvas does not take")
	{
		ctx.sourceType = "HE_ASSET_PATH";
		ctx.settle(h, emptySpot(ctx, h));
		dragFromSource(emptySpot(ctx, h));
		CHECK(drops == 0);
		// Not ours to pick up or cancel — only "this does not go here".
		CHECK(log.cues == std::vector<Cue>{ Cue::OverInvalid });
	}
}

TEST_CASE("GraphEditor drag cues: the drag does the same with and without them")
{
	// One script of gestures, run on two identical graphs: one listening (and
	// with a canConnect that says no to everything, the worst case for a
	// predicate leaking into the drop), one plain. Links, cleared pins,
	// selection, node positions and every frame's return value must match.
	struct Run { std::vector<std::array<int,4>> links; std::vector<int> cleared;
	             std::vector<int> selection; std::vector<float> xs; std::vector<bool> changed; };
	auto script = [](bool cues) {
		Ctx ctx;
		Harness h;
		CueLog log;
		if (cues) { log.attach(h); h.model.canConnect = [](int, int, int, int){ return false; }; }
		Run r;
		auto f = [&]{ r.changed.push_back(ctx.frame(h)); };
		ctx.settle(h, emptySpot(ctx, h));
		auto drag = [&](ImVec2 from, std::vector<ImVec2> path) {
			ctx.mouse(from); f(); ctx.button(true); f();
			for (auto p : path) { ctx.mouse(p); f(); f(); }
			ctx.button(false); f(); f();
		};
		drag(outputPin(ctx, h, *h.find(1), 0), { inputPin(ctx, h, *h.find(2), 0), inputPin(ctx, h, *h.find(3), 0) });
		drag(inputPin(ctx, h, *h.find(2), 0), { emptySpot(ctx, h) });
		drag(outputPin(ctx, h, *h.find(2), 0), { inputPin(ctx, h, *h.find(2), 0) });
		drag(ctx.toScreen(h, 340.0f, 306.0f), { ctx.toScreen(h, 380.0f, 340.0f) });
		r.links = h.links; r.cleared = h.clearedPins; r.selection = h.state.selection;
		for (auto& n : h.nodes) r.xs.push_back(n.x);
		if (cues) CHECK_FALSE(log.cues.empty());
		return r;
	};
	const Run with = script(true), without = script(false);
	CHECK(with.links == without.links);
	CHECK(with.cleared == without.cleared);
	CHECK(with.selection == without.selection);
	CHECK(with.xs == without.xs);
	CHECK(with.changed == without.changed);
}
