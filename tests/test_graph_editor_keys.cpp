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

	// Two pins per node: one exec-in (id 0) and one exec-out (id 1).
	static std::vector<GraphEditor::Pin> pinsOf()
	{
		return {
			{ 0, "In",  IM_COL32_WHITE, true,  true },
			{ 1, "Out", IM_COL32_WHITE, false, true },
		};
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
		model.pins    = [](int){ return pinsOf(); };
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
