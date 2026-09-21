#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "InspectorPanel.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityActive.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/InactiveComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/NetworkComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <imgui.h>
#include <imgui_internal.h>   // GetHoveredID, the open-popup stack

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── The Details panel, driven headless ───────────────────────────────────────
// Two things on the panel that a unit test of the serializer cannot vouch for:
// that the Active box at the top really is a click away and lands in the undo
// history, and that a component header's right-click menu really offers Copy /
// Paste Component Values / Reset to Default / Remove Component and that those
// items do what they say to the world. So this drives the real panel
// (InspectorPanel::renderFor with a real AppContext) in a headless ImGui
// context, finds the items by asking ImGui what is under the pointer, clicks
// them, and reads the world and the clipboard afterwards. Same harness shape
// as tests/test_outliner_ui.cpp.

using namespace HE::Ed;

namespace
{
	constexpr int W = 420, H = 520;

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(H));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
			}
#endif
			applyHorizonDarkTheme();
		}
		~Harness() { ImGui::DestroyContext(); }
	};

	// Everything AppContext insists on being given, in one place.
	struct ContextBits
	{
		EditorConfig    config;
		bool            vsync = false;
		std::string     backendName = "Software";
		EditorSelection selection;
		std::string     scenePath;
		bool            exitRequested = false, projectLoaded = true;
		bool            refreshPending = false, refreshDone = false;
		int   fpsOffset = 0, fpsCount = 0;
		float fpsAccum = 0.0f, smoothFps = 0.0f;
		std::vector<AppContext::EditorTab> tabs;
		int   activeTab = 0;
		float cbTreeWidth = 200.0f;
		int   hubPreset = 0, hubLang = 0;
		bool  hubFx = false;
		std::string hubCreateError, hubOpenError;
		int   hubRemoveIndex = -1;
		bool  hubRemoveRequested = false;
		std::string dirResult, fileResult;
		bool  dirReady = false, fileReady = false;

		AppContext make(HorizonWorld& world, EditorUndo& undo)
		{
			AppContext ctx{
				.editorConfig          = config,
				.vsync                 = vsync,
				.backendName           = backendName,
				.selection             = selection,
				.currentScenePath      = scenePath,
				.exitRequested         = exitRequested,
				.projectLoaded         = projectLoaded,
				.contentRefreshPending = refreshPending,
				.contentRefreshDone    = refreshDone,
				.fpsHistoryOffset      = fpsOffset,
				.fpsAccum              = fpsAccum,
				.fpsAccumCount         = fpsCount,
				.smoothFps             = smoothFps,
				.tabs                  = tabs,
				.activeTab             = activeTab,
				.cbTreeWidth           = cbTreeWidth,
				.hubSelectedPreset     = hubPreset,
				.hubSelectedLang       = hubLang,
				.hubAdvancedShaderFx   = hubFx,
				.hubCreateError        = hubCreateError,
				.hubOpenError          = hubOpenError,
				.hubRemoveIndex        = hubRemoveIndex,
				.hubRemoveRequested    = hubRemoveRequested,
				.pendingDirResult      = dirResult,
				.pendingDirReady       = dirReady,
				.pendingFileResult     = fileResult,
				.pendingFileReady      = fileReady,
			};
			ctx.world   = &world;
			ctx.undoSys = &undo;
			return ctx;
		}
	};

	struct Panel
	{
		AppContext&  ctx;
		HorizonWorld& world;
		Entity        entity;
		ImGuiID       lightHeader   = 0;   // the "Light" CollapsingHeader's id, read inside the window
		ImGuiID       networkHeader = 0;   // the "Network" one, likewise
		ImGuiID       activeBox     = 0;   // the Active checkbox's id, likewise
	};

	// One frame of the panel at a fixed place, with the pointer where the
	// caller put it and the two mouse buttons as given. Returns the id ImGui
	// says the pointer is on.
	ImGuiID frame(Panel& p, bool left, bool right = false, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
		io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		ImGui::Begin("Details");
		p.lightHeader   = ImGui::GetID("Light");
		p.networkHeader = ImGui::GetID("Network");
		p.activeBox     = ImGui::GetID("##entity_active");
		InspectorPanel::renderFor(p.ctx, p.world, p.entity, p.ctx.undoSys);
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		const ImGuiID hovered = ImGui::GetHoveredID();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return hovered;
	}

	// What is under the pointer at (x, y), settled: ImGui resolves hover from
	// the previous frame, so the answer is the second frame's.
	ImGuiID idAt(Panel& p, float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(p, false);
		return frame(p, false);
	}

	void clickAt(Panel& p, float x, float y, bool rightButton = false)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
		frame(p, false);
		frame(p, false);
		frame(p, !rightButton, rightButton);
		frame(p, false);
		frame(p, false);
	}

	// Where the pointer first lands on `wanted`, walking down at x. -1 if
	// never.
	float yOf(Panel& p, ImGuiID wanted, float x)
	{
		for (float y = 12.0f; y < float(H) - 12.0f; y += 2.0f)
			if (idAt(p, x, y) == wanted) return y + 4.0f;
		return -1.0f;
	}

	// The items below (x, yFrom), top to bottom, as the pointer sees them:
	// every new non-zero id is a new item. What a just-opened context menu's
	// entries look like from the outside.
	struct Row { ImGuiID id; float yMid; };
	std::vector<Row> itemsBelow(Panel& p, float x, float yFrom, float yTo)
	{
		std::vector<Row> rows;
		ImGuiID last = 0; float yStart = 0.0f;
		for (float y = yFrom; y < yTo; y += 2.0f)
		{
			const ImGuiID id = idAt(p, x, y);
			if (id == last) continue;
			if (last != 0) rows.push_back({ last, (yStart + y - 2.0f) * 0.5f });
			last = id; yStart = y;
		}
		if (last != 0) rows.push_back({ last, (yStart + yTo) * 0.5f });
		return rows;
	}

	bool popupOpen() { return GImGui->OpenPopupStack.Size > 0; }
}

TEST_CASE("inspector ui: the Active box is the first thing on the panel and one click flips it, undoably")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity lamp = world.createEntity("Lamp");
	reg.emplace<TransformComponent>(lamp);
	LightComponent light; light.intensity = 7.5f;
	reg.emplace<LightComponent>(lamp, light);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	Panel p{ ctx, world, lamp };

	// Pointer away, a few frames to settle the layout; one shot to look at.
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(p, false, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(img, std::string(dir) + "/inspector-lamp.bmp");

	// Down the LEFT edge, by id: the Active box is the first item of the panel
	// proper, in front of the name field, under the window's title bar. Found
	// by its id rather than by counting — the border grip and the title bar's
	// arrow are items too, and how many of them the pointer meets on the way
	// down is the window's business, not this test's.
	const float leftX = 10.0f + ImGui::GetStyle().WindowPadding.x + 6.0f;
	REQUIRE(p.activeBox != 0);
	const float boxY = yOf(p, p.activeBox, leftX);
	REQUIRE_MESSAGE(boxY > 0.0f, "the Active box is not under the pointer anywhere down the left edge");
	// It sits ABOVE the first component header — it is the entity's, not a
	// component's.
	CHECK(boxY < yOf(p, p.lightHeader, 10.0f + (float(W) - 20.0f) * 0.5f));

	REQUIRE(HE::isEntityActiveSelf(reg, lamp));
	REQUIRE_FALSE(undo.canUndo());
	clickAt(p, leftX, boxY);
	CHECK_FALSE(HE::isEntityActiveSelf(reg, lamp));
	CHECK_FALSE(HE::isEntityActive(reg, lamp));
	// It was an undo step ...
	CHECK(undo.canUndo());
	// ... and the click did not touch the light beside it.
	CHECK(reg.get<LightComponent>(lamp).intensity == doctest::Approx(7.5f));
	// Again: on.
	clickAt(p, leftX, boxY);
	CHECK(HE::isEntityActiveSelf(reg, lamp));

	// Undo takes the LAST flip back — the entity is off again — and no second
	// tag appeared anywhere along the way.
	REQUIRE(undo.undo());
	CHECK(reg.view<InactiveComponent>().size() == 1);
	REQUIRE(undo.undo());
	CHECK(reg.view<InactiveComponent>().size() == 0);
}

TEST_CASE("inspector ui: a component header's right-click menu copies, resets and pastes the component")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity lamp = world.createEntity("Lamp");
	reg.emplace<TransformComponent>(lamp);
	LightComponent light; light.intensity = 7.5f; light.range = 42.0f;
	reg.emplace<LightComponent>(lamp, light);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	Panel p{ ctx, world, lamp };
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 4; ++i) frame(p, false);

	// The clipboard starts out holding no component.
	ImGui::SetClipboardText("");
	CHECK(InspectorPanel::clipboardComponentKey().empty());

	// Find the "Light" header by its id, walking down the middle.
	const float midX = 10.0f + (float(W) - 20.0f) * 0.5f;
	REQUIRE(p.lightHeader != 0);
	const float headerY = yOf(p, p.lightHeader, midX);
	REQUIRE_MESSAGE(headerY > 0.0f, "the Light header is not under the pointer anywhere");

	// ── Right-click: the menu opens, with four entries ──
	clickAt(p, midX, headerY, /*rightButton=*/true);
	REQUIRE(popupOpen());
	// The popup hangs off the pointer; its entries are below and to the right.
	const std::vector<Row> items = itemsBelow(p, midX + 40.0f, headerY + 2.0f, headerY + 140.0f);
	REQUIRE_MESSAGE(items.size() >= 4, "found " << items.size() << " items in the header menu");
	const Row copyItem  = items[0];   // Copy Component
	const Row pasteItem = items[1];   // Paste Component Values (greyed: nothing to paste yet)
	const Row resetItem = items[2];   // Reset to Default
	// items[3] is Remove Component, left alone here.

	// ── Copy: the clipboard now names the light, the world is untouched ──
	clickAt(p, midX + 40.0f, copyItem.yMid);
	CHECK_FALSE(popupOpen());
	CHECK(InspectorPanel::clipboardComponentKey() == "light");
	CHECK(reg.get<LightComponent>(lamp).intensity == doctest::Approx(7.5f));
	CHECK_FALSE(undo.canUndo());   // a copy changes nothing, so it is no step

	// ── Reset: the values are the defaults, in one undo step ──
	clickAt(p, midX, headerY, /*rightButton=*/true);
	REQUIRE(popupOpen());
	clickAt(p, midX + 40.0f, resetItem.yMid);
	CHECK_FALSE(popupOpen());
	CHECK(reg.get<LightComponent>(lamp).intensity == doctest::Approx(LightComponent{}.intensity));
	CHECK(reg.get<LightComponent>(lamp).range     == doctest::Approx(LightComponent{}.range));
	CHECK(reg.all_of<TransformComponent>(lamp));   // only the one component
	CHECK(undo.canUndo());

	// ── Paste Component Values: the copied 7.5 / 42 are back ──
	clickAt(p, midX, headerY, /*rightButton=*/true);
	REQUIRE(popupOpen());
	clickAt(p, midX + 40.0f, pasteItem.yMid);
	CHECK_FALSE(popupOpen());
	CHECK(reg.get<LightComponent>(lamp).intensity == doctest::Approx(7.5f));
	CHECK(reg.get<LightComponent>(lamp).range     == doctest::Approx(42.0f));

	// The clipboard text is the serializer's envelope, so it also reads back
	// through the serializer alone — what a second editor would do with it.
	const char* text = ImGui::GetClipboardText();
	REQUIRE(text != nullptr);
	CHECK(SceneSerializer::componentKeyOfText(text) == "light");
	HorizonWorld other;
	const Entity twin = other.createEntity("Twin");
	CHECK(SceneSerializer{}.importComponentText(other, twin, text));
	CHECK(other.registry().get<LightComponent>(twin).intensity == doctest::Approx(7.5f));

	he_ui::Image img;
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	for (int i = 0; i < 3; ++i) frame(p, false, false, i == 2 ? &img : nullptr);
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(img, std::string(dir) + "/inspector-lamp-after.bmp");
}

TEST_CASE("inspector ui: the Network section is on the panel, and its menu copies and resets by the scene key")
{
	// The section is the anti-cheat plan's precondition (docs/anti-cheat-plan.md
	// §6.2.1): a place in the Details panel where maxSpeed can be typed. One
	// header with a working Copy / Reset proves the whole chain — the section
	// draws, its label is in the prefab-key table, and that key is one the
	// serializer exports and resets — the way the Light test above does.
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity player = world.createEntity("Player");
	reg.emplace<TransformComponent>(player);
	NetworkComponent nc;
	nc.relevanceRadius    = 80.0f;
	nc.replicateTransform = false;
	nc.maxSpeed           = 7.0f;
	nc.maxVerticalSpeed   = 3.0f;
	reg.emplace<NetworkComponent>(player, nc);

	ContextBits bits;
	AppContext ctx = bits.make(world, undo);
	Panel p{ ctx, world, player };
	ImGui::GetIO().AddMousePosEvent(float(W) - 2.0f, float(H) - 2.0f);
	he_ui::Image img;
	for (int i = 0; i < 4; ++i) frame(p, false, false, i == 3 ? &img : nullptr);
	REQUIRE(img.valid());
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(img, std::string(dir) + "/inspector-network.bmp");

	// The header is there, under the pointer somewhere down the middle.
	const float midX = 10.0f + (float(W) - 20.0f) * 0.5f;
	REQUIRE(p.networkHeader != 0);
	const float headerY = yOf(p, p.networkHeader, midX);
	REQUIRE_MESSAGE(headerY > 0.0f, "the Network header is not under the pointer anywhere");

	ImGui::SetClipboardText("");
	clickAt(p, midX, headerY, /*rightButton=*/true);
	REQUIRE(popupOpen());
	const std::vector<Row> items = itemsBelow(p, midX + 40.0f, headerY + 2.0f, headerY + 140.0f);
	REQUIRE_MESSAGE(items.size() >= 4, "found " << items.size() << " items in the header menu");
	const Row copyItem  = items[0];
	const Row resetItem = items[2];

	// ── Copy: the clipboard names the component by its scene key ──
	clickAt(p, midX + 40.0f, copyItem.yMid);
	CHECK_FALSE(popupOpen());
	CHECK(InspectorPanel::clipboardComponentKey() == "network");
	const char* text = ImGui::GetClipboardText();
	REQUIRE(text != nullptr);
	// And what it carries is the authored config, without a session id.
	HorizonWorld other;
	const Entity twin = other.createEntity("Twin");
	CHECK(SceneSerializer{}.importComponentText(other, twin, text));
	{
		const auto& t = other.registry().get<NetworkComponent>(twin);
		CHECK(t.maxSpeed         == doctest::Approx(7.0f));
		CHECK(t.maxVerticalSpeed == doctest::Approx(3.0f));
		CHECK(t.relevanceRadius  == doctest::Approx(80.0f));
		CHECK_FALSE(t.replicateTransform);
		CHECK(t.netId == 0u);
	}

	// ── Reset: back to the defaults (0 = unchecked), as one undo step ──
	REQUIRE_FALSE(undo.canUndo());
	clickAt(p, midX, headerY, /*rightButton=*/true);
	REQUIRE(popupOpen());
	clickAt(p, midX + 40.0f, resetItem.yMid);
	CHECK_FALSE(popupOpen());
	{
		const auto& r = reg.get<NetworkComponent>(player);
		CHECK(r.maxSpeed         == doctest::Approx(NetworkComponent{}.maxSpeed));
		CHECK(r.maxVerticalSpeed == doctest::Approx(NetworkComponent{}.maxVerticalSpeed));
		CHECK(r.relevanceRadius  == doctest::Approx(NetworkComponent{}.relevanceRadius));
		CHECK(r.replicateTransform == NetworkComponent{}.replicateTransform);
	}
	CHECK(reg.all_of<TransformComponent>(player));
	CHECK(undo.canUndo());
	REQUIRE(undo.undo());
	// Undo restores the world by clear + reload, so every handle is re-minted
	// and `player` is stale (entt would assert on it). Find the entity again
	// by the component it carries — and check it IS the same one by name.
	const auto restored = reg.view<NetworkComponent>();
	REQUIRE(restored.size() == 1);
	const Entity playerAgain = restored.front();
	CHECK(reg.get<NameComponent>(playerAgain).name == "Player");
	CHECK(reg.get<NetworkComponent>(playerAgain).maxSpeed         == doctest::Approx(7.0f));
	CHECK(reg.get<NetworkComponent>(playerAgain).maxVerticalSpeed == doctest::Approx(3.0f));
}

// ── Add Component: grouped, searchable, keyboard-complete ────────────────────
// The menu used to be twenty-eight rows in no order. Now it is seven groups
// with a search box above them, and the search has a keyboard ending: type,
// Enter, added. This drives the real menu (InspectorPanel::addComponentMenu)
// inside a popup, reads the rows the pointer finds, types into the box and
// reads the world afterwards.
namespace
{
	// One frame of a window that holds the Add Component popup. `open` asks
	// for the popup on this frame; the menu draws for as long as ImGui keeps
	// it open. Returns what the menu reported.
	bool menuFrame(HorizonWorld& world, Entity entity, EditorUndo* undo, bool open,
	               bool leftDown = false, he_ui::Image* shot = nullptr)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, leftDown);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
		ImGui::SetNextWindowSize(ImVec2(float(W) - 20.0f, float(H) - 20.0f));
		ImGui::Begin("Details");
		if (open) ImGui::OpenPopup("##add_component");
		bool added = false;
		if (ImGui::BeginPopup("##add_component"))
		{
			added = InspectorPanel::addComponentMenu(world, entity, undo);
			ImGui::EndPopup();
		}
		ImGui::End();
		EditorWidgets::drawQueuedHelp();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
		return added;
	}

	// The distinct hoverable ids down a column of the open popup, top to
	// bottom — the same reading the header-menu tests take.
	std::vector<ImGuiID> menuRowsAt(HorizonWorld& world, Entity entity, float x)
	{
		std::vector<ImGuiID> rows;
		ImGuiID last = 0;
		for (float y = 12.0f; y < float(H) - 12.0f; y += 2.0f)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			menuFrame(world, entity, nullptr, false);
			menuFrame(world, entity, nullptr, false);
			const ImGuiID id = ImGui::GetHoveredID();
			if (id == 0 || id == last) { if (id == 0) last = 0; continue; }
			rows.push_back(id);
			last = id;
		}
		return rows;
	}
}

TEST_CASE("inspector ui: Add Component is grouped, and a typed search ends on Enter")
{
	Harness harness;
	HorizonWorld world;
	EditorUndo   undo;
	undo.setWorld(&world);
	auto& reg = world.registry();

	const Entity crate = world.createEntity("Crate");
	reg.emplace<TransformComponent>(crate);

	// The popup opens at the pointer, so the pointer sits near the window's
	// top-left corner: a popup opened at the bottom-right would be pushed back
	// inside the display and its rows would be anywhere.
	ImGuiIO& io = ImGui::GetIO();
	io.AddMousePosEvent(40.0f, 40.0f);
	for (int i = 0; i < 3; ++i) menuFrame(world, crate, &undo, false);

	// ── Open: seven groups, not twenty-eight rows ──
	menuFrame(world, crate, &undo, true);
	menuFrame(world, crate, &undo, false);
	REQUIRE(popupOpen());
	he_ui::Image grouped;
	menuFrame(world, crate, &undo, false, false, &grouped);
	REQUIRE(grouped.valid());
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(grouped, std::string(dir) + "/inspector-add-component.bmp");

	// The popup hangs at the pointer's position when it opened, so its rows
	// are found in a column just inside its left edge.
	// Transform is on the entity already but Transform 2D is not, so all seven
	// groups have something to offer; the Animation group is greyed (no
	// skeleton) and a greyed row is still a row. Fewer than seven would mean a
	// group vanished; many more would mean the flat list is back.
	const std::vector<ImGuiID> rows = menuRowsAt(world, crate, 70.0f);
	REQUIRE(popupOpen());
	CHECK_MESSAGE(rows.size() >= 6, "found " << rows.size() << " rows in the grouped menu");
	CHECK_MESSAGE(rows.size() <= 10, "found " << rows.size() << " rows — that is the flat list");

	// ── Type "camera r" ──
	// The box has focus when the popup opens; the scan above hovered the
	// groups, though, and a submenu that opens takes the focus with it (as it
	// would for a user who wandered over a group), so click the box first —
	// it is the first row, just under the popup's top edge.
	// The rows give way to the hits; Enter takes the first. "camera r" is
	// what it takes to make that Camera Rig: a bare "rig" finds Rigid Body
	// first, Physics being the earlier group — which is the rule working, not
	// failing. A rig brings its Camera along, in one undo step.
	CHECK_FALSE(reg.all_of<CameraRigComponent>(crate));
	CHECK_FALSE(reg.all_of<CameraComponent>(crate));
	io.AddMousePosEvent(150.0f, 60.0f);
	menuFrame(world, crate, &undo, false);
	menuFrame(world, crate, &undo, false);
	menuFrame(world, crate, &undo, false, /*leftDown=*/true);
	menuFrame(world, crate, &undo, false);
	REQUIRE(popupOpen());
	io.AddInputCharactersUTF8("camera r");
	menuFrame(world, crate, &undo, false);
	menuFrame(world, crate, &undo, false);
	REQUIRE(popupOpen());
	io.AddKeyEvent(ImGuiKey_Enter, true);
	bool added = menuFrame(world, crate, &undo, false);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	added = menuFrame(world, crate, &undo, false) || added;
	CHECK(added);
	CHECK(reg.all_of<CameraRigComponent>(crate));
	CHECK(reg.all_of<CameraComponent>(crate));
	CHECK_FALSE(reg.all_of<RigidBodyComponent>(crate));
	CHECK(undo.canUndo());
	// The popup closed with the add — one Enter, one component, back to the panel.
	menuFrame(world, crate, &undo, false);
	CHECK_FALSE(popupOpen());

	// ── Opened again: the rig is on the entity, so Gameplay still has rows
	// (Movement, Script…) but a search for "rig" now finds nothing to add.
	menuFrame(world, crate, &undo, true);
	menuFrame(world, crate, &undo, false);
	REQUIRE(popupOpen());
	io.AddInputCharactersUTF8("camera rig");
	menuFrame(world, crate, &undo, false);
	io.AddKeyEvent(ImGuiKey_Enter, true);
	added = menuFrame(world, crate, &undo, false);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	added = menuFrame(world, crate, &undo, false) || added;
	CHECK_FALSE(added);
	CHECK(popupOpen());   // nothing matched, nothing closed
	he_ui::Image empty;
	menuFrame(world, crate, &undo, false, false, &empty);
	if (const char* dir = std::getenv("HE_UI_DUMP_DIR"); dir && *dir)
		he_ui::writeBmp(empty, std::string(dir) + "/inspector-add-component-nomatch.bmp");
}
