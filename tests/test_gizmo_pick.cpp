#include "doctest.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

// ── Transform-gizmo picking, headless ────────────────────────────────────────
// The editor's ImGui surface cannot be screenshotted headless, so the one thing
// that has to be right about the gizmo — that the handle you can GRAB sits
// exactly where the handle you can SEE is drawn, in the SAME frame — could only
// ever be checked by hand. ImGuizmo needs no backend though: given an ImGui
// context, a display size and a hand-fed input queue it runs exactly as it does
// in the viewport, against whatever camera matrices we hand it.
//
// The bug these pin: ImGuizmo builds its whole context (draw positions AND hit
// regions) from the matrix it is handed at the top of Manipulate(), then moves
// that matrix — so everything it drew and everything it could be grabbed by
// described the PREVIOUS frame's transform, while the renderer draws the object
// from the one this frame just wrote. During a drag the grab point trailed the
// object by a frame of mouse movement.

namespace
{
constexpr float kDisplayW = 1280.0f, kDisplayH = 720.0f;

// The viewport rect the gizmo is told about — inset, so a rect origin that is
// wrongly ignored (or a width used where a height belongs) shows up as an
// offset rather than cancelling out at (0,0).
constexpr float kRectX = 40.0f, kRectY = 24.0f;
constexpr float kRectW = 900.0f, kRectH = 600.0f;

// Fixed camera: 10 units back on +Z, looking at the origin.
glm::mat4 testView()
{
	return glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}
glm::mat4 testProj()
{
	return glm::perspective(glm::radians(45.0f), kRectW / kRectH, 0.1f, 1000.0f);
}

// ImGuizmo's own world → screen mapping (worldToPos), so an expectation is
// derived from the camera rather than from a number typed in by hand.
ImVec2 project(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& world)
{
	const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
	REQUIRE(std::fabs(clip.w) > 1e-6f);
	glm::vec4 t = clip * (0.5f / clip.w);
	t.x += 0.5f; t.y += 0.5f;
	t.y = 1.0f - t.y;
	return ImVec2(kRectX + t.x * kRectW, kRectY + t.y * kRectH);
}

float dist(const ImVec2& a, const ImVec2& b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// One frame of a viewport that draws the gizmo, in the editor's own order:
// NewFrame → ImGuizmo::BeginFrame → window → SetDrawlist/SetRect → Manipulate,
// and IsOver() read AFTER Manipulate (that is what the viewport's picking
// suppression asks).
struct Ctx
{
	glm::mat4 view = testView();
	glm::mat4 proj = testProj();
	bool   over = false;      // IsOver() of the last frame
	bool   using_ = false;    // IsUsing() of the last frame
	ImVec2 origin{ 0, 0 };    // where the gizmo's own centre ended up

	Ctx()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
		io.DeltaTime   = 1.0f / 60.0f;
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~Ctx()
	{
		// gContext is process-wide: leave no drag latched for the next case.
		ImGui::DestroyContext();
	}

	void frame(glm::mat4& model, ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE)
	{
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
		ImGui::Begin("viewport", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		ImGuizmo::Enable(true);
		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(kRectX, kRectY, kRectW, kRectH);
		ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
		                     ImGuizmo::LOCAL, glm::value_ptr(model));
		over   = ImGuizmo::IsOver();
		using_ = ImGuizmo::IsUsing();
		origin = ImGuizmo::GetGizmoScreenOrigin();
		ImGui::End();
		ImGui::Render();
	}

	// ImGui hit-tests against the previous frame's layout, so a click only
	// registers once a frame has already placed the window.
	void settle(glm::mat4& model, ImVec2 mouse, ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE)
	{
		for (int i = 0; i < 3; ++i)
		{
			ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
			frame(model, op);
		}
	}
	void mouse(ImVec2 p)   { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
	void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }
};
} // namespace

TEST_CASE("Gizmo picking: the hit region sits where the camera puts the gizmo")
{
	Ctx ctx;
	glm::mat4 model(1.0f);
	const ImVec2 centre = project(ctx.view, ctx.proj, glm::vec3(0.0f));

	ctx.settle(model, centre);
	CHECK(ctx.over);
	// The gizmo's own idea of its centre is the projection of its matrix.
	CHECK(dist(ctx.origin, centre) < 1.0f);

	// Well clear of every handle — the screen square is ±10 px, the axes reach
	// roughly a tenth of the viewport.
	ctx.settle(model, ImVec2(centre.x + 400.0f, centre.y + 260.0f));
	CHECK_FALSE(ctx.over);
}

TEST_CASE("Gizmo picking: a moved object takes its hit region along the same frame")
{
	Ctx ctx;
	glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
	const ImVec2 moved = project(ctx.view, ctx.proj, glm::vec3(2.0f, 0.0f, 0.0f));
	const ImVec2 origin0 = project(ctx.view, ctx.proj, glm::vec3(0.0f));
	REQUIRE(dist(moved, origin0) > 60.0f);

	// Hovering where the object IS grabs; where it WAS does not.
	ctx.settle(model, moved);
	CHECK(ctx.over);
	ctx.settle(model, origin0);
	CHECK_FALSE(ctx.over);
}

TEST_CASE("Gizmo picking: the camera of THIS frame decides, not the last one")
{
	Ctx ctx;
	glm::mat4 model(1.0f);
	ctx.settle(model, project(ctx.view, ctx.proj, glm::vec3(0.0f)));
	REQUIRE(ctx.over);

	// Swing the camera sideways, exactly as an orbit does mid-frame, and park
	// the mouse where the gizmo lands under the NEW camera. One frame is all it
	// may take: a picking ray built from the previous frame's view matrix would
	// still be looking at the old spot.
	ctx.view = glm::lookAt(glm::vec3(6.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	const ImVec2 nowAt = project(ctx.view, ctx.proj, glm::vec3(0.0f));
	ctx.mouse(nowAt);
	ctx.frame(model);
	CHECK(ctx.over);
	CHECK(dist(ctx.origin, nowAt) < 1.0f);
}

TEST_CASE("Gizmo drag: the grab point stays on the object instead of trailing it")
{
	Ctx ctx;
	glm::mat4 model(1.0f);
	const ImVec2 start = project(ctx.view, ctx.proj, glm::vec3(0.0f));

	// Grab the screen-space square in the middle of the gizmo: it moves on the
	// view plane, so a screen-space delta maps to a screen-space move.
	ctx.settle(model, start);
	REQUIRE(ctx.over);
	ctx.button(true);
	ctx.frame(model);
	REQUIRE(ctx.using_);

	// One long drag step — a fast mouse move is where a one-frame lag is worth
	// whole handles.
	const ImVec2 dragged(start.x + 120.0f, start.y - 60.0f);
	ctx.mouse(dragged);
	ctx.frame(model);
	REQUIRE(ctx.using_);

	const glm::vec3 pos(model[3]);
	CHECK(glm::length(pos) > 0.5f);                       // it actually moved
	const ImVec2 nowAt = project(ctx.view, ctx.proj, pos);
	CHECK(dist(nowAt, dragged) < 2.0f);                   // …to under the cursor

	// The point of the whole case: what the gizmo drew and what it can be
	// grabbed by is the transform THIS frame produced — not the one it was
	// handed, which is where it used to sit a frame behind the object.
	CHECK(dist(ctx.origin, nowAt) < 2.0f);
	CHECK(dist(ctx.origin, start) > 60.0f);

	ctx.button(false);
	ctx.frame(model);
	CHECK_FALSE(ctx.using_);
}
