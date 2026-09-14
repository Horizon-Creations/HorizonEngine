#include "doctest.h"

// Compiled with HE_IMGUI_ENABLED (tests/CMakeLists.txt sets it on this file
// and on EditorTransformGizmo.cpp alone): the gizmo's header is guarded by the
// flag the editor target defines, and this is the one test that reaches
// through it.
#include "EditorTransformGizmo.h"
#include "EditorUndo.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <string>

// ── The gizmo on a light ─────────────────────────────────────────────────────
// A light has a TransformComponent and nothing to draw. Now that the Scene
// window shows it as an icon and a click selects it, the next thing a user
// does is drag the gizmo that appears on it — through EditorTransformGizmo,
// the same call the viewport makes for a mesh. That the gizmo neither needs a
// MeshComponent nor a picture is asserted here the way test_gizmo_pick.cpp
// drives raw ImGuizmo: an ImGui context, a display size, a hand-fed mouse.

namespace
{
constexpr float kDisplayW = 1280.0f, kDisplayH = 720.0f;
constexpr float kRectX = 40.0f, kRectY = 24.0f, kRectW = 900.0f, kRectH = 600.0f;

glm::mat4 testView()
{
	return glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}
glm::mat4 testProj()
{
	return glm::perspective(glm::radians(45.0f), kRectW / kRectH, 0.1f, 1000.0f);
}

ImVec2 project(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& world)
{
	const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
	REQUIRE(std::fabs(clip.w) > 1e-6f);
	glm::vec4 t = clip * (0.5f / clip.w);
	t.x += 0.5f; t.y += 0.5f;
	t.y = 1.0f - t.y;
	return ImVec2(kRectX + t.x * kRectW, kRectY + t.y * kRectH);
}

Entity findByName(HorizonWorld& w, const std::string& name)
{
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (n.name == name) return e;
	return entt::null;
}

// One viewport frame in the editor's own order: propagate the hierarchy (the
// extractor does this before the picture is drawn; the gizmo reads worldMatrix
// and an entity created this frame would otherwise sit at the identity),
// NewFrame, ImGuizmo::BeginFrame, the Scene window, manipulate.
struct Ctx
{
	HorizonWorld world;
	EditorUndo   undo;
	ViewportToolbar::State tb;      // Move, Local, no snap — the defaults
	glm::mat4 view = testView();
	glm::mat4 proj = testProj();
	bool active = false;            // manipulate()'s return: hovered or dragging
	bool changed = false;           // …and whether it wrote the transform

	Ctx()
	{
		undo.setWorld(&world);
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~Ctx() { ImGui::DestroyContext(); }

	void frame(Entity e)
	{
		HE::propagateTransforms(world);
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
		ImGui::Begin("viewport", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		changed = false;
		active  = EditorTransformGizmo::manipulate(world, std::vector<Entity>{ e },
			view, proj, ImVec2(kRectX, kRectY), ImVec2(kRectX + kRectW, kRectY + kRectH),
			tb, /*enabled=*/true, &undo, &changed);
		ImGui::End();
		ImGui::Render();
	}

	// ImGui hit-tests against the previous frame's layout.
	void settle(Entity e, ImVec2 mouse)
	{
		for (int i = 0; i < 3; ++i)
		{
			ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
			frame(e);
		}
	}
	void mouse(ImVec2 p)   { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
	void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }

	// Every case ends its drag: the gizmo latches the drag in file statics that
	// outlive the ImGui context, and the next case must not inherit one.
	void release(Entity e)
	{
		button(false);
		frame(e);
		frame(e);
	}
};
} // namespace

TEST_CASE("Gizmo on a light: a mesh-less entity gets the gizmo where it stands")
{
	Ctx ctx;
	const Entity light = ctx.world.createEntity("Lamp");
	TransformComponent t;
	t.position = { 2.0f, 1.0f, 0.0f };
	ctx.world.addComponent(light, t);
	ctx.world.registry().emplace<LightComponent>(light, LightComponent{});

	// Hovering the light's own screen position grabs its gizmo; the world
	// origin, where an unpropagated transform would have put it, does not.
	const ImVec2 at     = project(ctx.view, ctx.proj, { 2.0f, 1.0f, 0.0f });
	const ImVec2 origin = project(ctx.view, ctx.proj, { 0.0f, 0.0f, 0.0f });
	ctx.settle(light, at);
	CHECK(ctx.active);
	ctx.settle(light, origin);
	CHECK_FALSE(ctx.active);
	// Hovering alone writes nothing.
	CHECK_FALSE(ctx.changed);
	ctx.release(light);
}

TEST_CASE("Gizmo on a light: a drag moves the light and lands on the undo stack once")
{
	Ctx ctx;
	const Entity light = ctx.world.createEntity("Lamp");
	TransformComponent t;
	t.position = { 2.0f, 1.0f, 0.0f };
	ctx.world.addComponent(light, t);
	ctx.world.registry().emplace<LightComponent>(light, LightComponent{});

	// Grab the screen-space square at the gizmo's centre (it moves on the view
	// plane) and pull it to the right and up.
	const ImVec2 start = project(ctx.view, ctx.proj, { 2.0f, 1.0f, 0.0f });
	ctx.settle(light, start);
	REQUIRE(ctx.active);
	ctx.button(true);
	ctx.frame(light);
	REQUIRE(ctx.active);

	const ImVec2 dragged(start.x + 120.0f, start.y - 60.0f);
	ctx.mouse(dragged);
	ctx.frame(light);
	REQUIRE(ctx.active);
	CHECK(ctx.changed);

	// The light itself moved — the TransformComponent, which is all a light
	// has — in the direction of the drag: right (+x) and up (+y) as seen from a
	// camera on +Z, and not along the view axis.
	const glm::vec3 moved = ctx.world.registry().get<TransformComponent>(light).position;
	CHECK(moved.x > 2.0f + 0.5f);
	CHECK(moved.y > 1.0f + 0.25f);
	CHECK(moved.z == doctest::Approx(0.0f).epsilon(1e-3));
	// …and its gizmo followed to where it now is.
	const ImVec2 nowAt = project(ctx.view, ctx.proj, moved);
	CHECK(std::fabs(nowAt.x - dragged.x) < 2.0f);
	CHECK(std::fabs(nowAt.y - dragged.y) < 2.0f);

	// Release: exactly ONE undo entry for the whole drag, and undo puts the
	// light back where it was.
	CHECK_FALSE(ctx.undo.canUndo());   // pending until the button goes up
	ctx.release(light);
	REQUIRE(ctx.undo.canUndo());
	REQUIRE(ctx.undo.undo());
	CHECK_FALSE(ctx.undo.canUndo());   // it was one entry, not one per frame

	const Entity restored = findByName(ctx.world, "Lamp");
	REQUIRE((restored != entt::null));
	const glm::vec3 back = ctx.world.registry().get<TransformComponent>(restored).position;
	CHECK(back.x == doctest::Approx(2.0f));
	CHECK(back.y == doctest::Approx(1.0f));
	CHECK(ctx.world.registry().all_of<LightComponent>(restored));
}
