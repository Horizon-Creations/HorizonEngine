#include "SecondaryViewportPanel.h"
#include "EditorApplication.h"     // AppContext, EditorCamera, EditorConfig
#include "EditorViewportNav.h"     // the Scene window's orbit / pan / fly grammar
#include "ViewportPanel.h"         // show flags, focusSelection, selectionBox, key blocks
#include "ViewportToolbar.h"       // viewPopup — the same picker over this pane's camera
#include "EditorWidgets.h"         // button / checkbox with the help lookup
#include "EditorHelp.h"            // the pane's scope
#include <Renderer/IRenderer.h>
#include <Math/AABB.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdio>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SecondaryViewportPanel
{

namespace
{
	struct Pane
	{
		EditorCamera cam;
		bool open    = false;
		bool seeded  = false;   // camera placed once, on first draw
		bool grid    = true;    // this pane's grid switch (AND the global show flag)
		bool hovered = false;   // the image's hover, from LAST frame (see render)
	};
	Pane s_panes[kCount];

	constexpr const char* kTitles[kCount] = { "Scene 2", "Scene 3", "Scene 4" };

	// The view each pane starts in: the three axis views a four-pane layout
	// is made of, next to the Scene window's perspective.
	constexpr EditorCamera::ViewPreset kDefaultPreset[kCount] = {
		EditorCamera::ViewPreset::Top,
		EditorCamera::ViewPreset::Front,
		EditorCamera::ViewPreset::Right,
	};
}

const char* title(int index)
{
	return kTitles[std::clamp(index, 0, kCount - 1)];
}

bool& open(int index)
{
	return s_panes[std::clamp(index, 0, kCount - 1)].open;
}

void releaseLookCaptures(SDL_Window* win)
{
	for (Pane& p : s_panes)
		EditorViewportNav::releaseLookCaptureFor(&p, win);
}

#ifdef HE_IMGUI_ENABLED

namespace
{
	// The camera's first placement: where the Scene window is looking, swung
	// into this pane's axis view around the same pivot — so "open Scene 2"
	// shows the part of the level the user is working on, from above, rather
	// than the world origin from a default distance.
	void seed(AppContext& ctx, Pane& p, int index)
	{
		if (ctx.editorCamera && ctx.editorCamera->initialised())
		{
			const EditorCamera& main = *ctx.editorCamera;
			p.cam.restoreView(main.position(), main.yaw(), main.pitch(), main.pivotDistance());
		}
		else
		{
			EditorCamera::Input none;
			p.cam.update(none);   // derives the default look-at-origin pose
		}
		p.cam.applyPreset(kDefaultPreset[index]);
		p.seeded = true;
	}

	// The selection's box, projected with this pane's matrix and drawn as
	// twelve lines — the Scene window's amber marker, seen from here. Edges
	// with an end behind the camera are dropped rather than clipped: a
	// wrong-way projection lands anywhere on the screen, and a marker that is
	// missing one edge is better than one with a stray line across the pane.
	void drawSelectionBox(AppContext& ctx, const glm::mat4& viewProj,
	                      const ImVec2& org, const ImVec2& av)
	{
		HE::AABB box;
		if (!ViewportPanel::selectionBox(ctx, box)) return;

		ImVec2 pts[8];
		bool   ok[8];
		for (int i = 0; i < 8; ++i)
		{
			const glm::vec3 corner((i & 1) ? box.max.x : box.min.x,
			                       (i & 2) ? box.max.y : box.min.y,
			                       (i & 4) ? box.max.z : box.min.z);
			const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
			ok[i] = clip.w > 1e-5f;
			if (!ok[i]) continue;
			const float nx = clip.x / clip.w, ny = clip.y / clip.w;
			pts[i] = ImVec2(org.x + (nx + 1.0f) * 0.5f * av.x,
			                org.y + (1.0f - ny) * 0.5f * av.y);
		}
		static const int kEdges[12][2] = {
			{0,1},{2,3},{4,5},{6,7},   // along X
			{0,2},{1,3},{4,6},{5,7},   // along Y
			{0,4},{1,5},{2,6},{3,7},   // along Z
		};
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(org, ImVec2(org.x + av.x, org.y + av.y), true);
		const ImU32 amber = IM_COL32(255, 204, 0, 220);
		for (const auto& e : kEdges)
			if (ok[e[0]] && ok[e[1]])
				dl->AddLine(pts[e[0]], pts[e[1]], amber, 1.5f);
		dl->PopClipRect();
	}

	// The strip above the picture: which way this pane looks, its grid, and
	// a way to bring the Scene window's camera over. Plain widgets rather than
	// the Scene bar's hand-drawn cells — three controls do not need zones.
	void drawStrip(AppContext& ctx, Pane& p)
	{
		HE::Ed::Help::Scope helpScope("Secondary Viewport");
		using VP = EditorCamera::ViewPreset;
		const VP   preset = p.cam.currentPreset();
		const bool ortho  = p.cam.orthographic();
		char viewLabel[48];
		std::snprintf(viewLabel, sizeof(viewLabel), "%s##view",
		              (ortho && preset == VP::Perspective) ? "Ortho" : EditorCamera::presetName(preset));
		if (ImGui::Button(viewLabel))
			ImGui::OpenPopup("##svpView");
		EditorWidgets::helpForKey("secondary-viewport.view");
		if (ImGui::BeginPopup("##svpView"))
		{
			ViewportToolbar::viewPopup(p.cam);
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		EditorWidgets::checkbox("Grid", &p.grid);

		ImGui::SameLine();
		if (EditorWidgets::button("Match Scene") && ctx.editorCamera && ctx.editorCamera->initialised())
		{
			const EditorCamera& main = *ctx.editorCamera;
			p.cam.restoreView(main.position(), main.yaw(), main.pitch(), main.pivotDistance());
			p.cam.setOrthographic(main.orthographic());
		}

		// What this pane is, said once where the eye lands: the picture is
		// the preview pass, and someone comparing it with the Scene window
		// should not have to wonder where the shadows went.
		ImGui::SameLine();
		ImGui::TextDisabled("preview shading");
	}

	void renderPane(AppContext& ctx, Pane& p, int index, float dt)
	{
		SDL_Window* sdlWin = ctx.window ? ctx.window->GetNativeWindow() : nullptr;
		if (!p.open)
		{
			// Closed (the X, the menu) mid-look: let go of the cursor.
			EditorViewportNav::releaseLookCaptureFor(&p, sdlWin);
			p.hovered = false;
			return;
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
		const bool shown = ImGui::Begin(kTitles[index], &p.open,
		                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		if (!shown)
		{
			// A collapsed pane or an inactive dock tab: nothing to render, and
			// RenderWorldPreview is a synchronous GPU round trip — the cheapest
			// pane is the one not drawn.
			EditorViewportNav::releaseLookCaptureFor(&p, sdlWin);
			p.hovered = false;
			ImGui::End();
			return;
		}

		if (!ctx.renderer || !ctx.world || !ctx.contentManager || ctx.appLivePreview)
		{
			ImGui::TextDisabled(ctx.appLivePreview ? "(an application has no scene to look at)"
			                                       : "(no scene)");
			ImGui::End();
			return;
		}

		if (!p.seeded) seed(ctx, p, index);
		p.cam.setFlySpeed(ctx.editorConfig.EditorCameraSpeed);

		drawStrip(ctx, p);

		const ImVec2 av  = ImGui::GetContentRegionAvail();
		const ImVec2 org = ImGui::GetCursorScreenPos();
		if (av.x < 32.0f || av.y < 32.0f) { ImGui::End(); return; }

		// ── Input BEFORE the picture. The renderer draws from whatever the
		// camera is when RenderWorldPreview is called, so gestures read after
		// it would leave the picture a frame behind the mouse. Hover is last
		// frame's answer — the only kind ImGui has for an item not yet
		// submitted, and the same one IsItemHovered resolves against anyway.
		const ImGuiIO& io = ImGui::GetIO();
		EditorCamera::Input cin;
		const bool navigating = EditorViewportNav::gather(ctx, &p, p.hovered, dt, av.y, cin);
		const bool keys = p.hovered && !io.WantTextInput && !navigating;
		if (keys)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_F, false))
				ViewportPanel::focusSelection(ctx, p.cam);
			ViewportPanel::presetKeys(p.cam);
			if (!io.KeyAlt) ViewportPanel::bookmarkKeys(p.cam);
		}
		p.cam.update(cin);

		// ── The picture. Logical size, as the asset viewports pass it: the
		// preview target is a whole-pixel texture the aspect is built from.
		const ViewportPanel::ShowFlags& show = ViewportPanel::showFlags();
		const IRenderer::EnvironmentSettings& sceneEnv = ctx.renderer->GetEnvironment();
		WorldPreviewEnv env;
		// The scene's sky at the scene's hour, so a sunset level reads as one
		// here too — but not in an axis view: with parallel rays every pixel
		// asks the dome for the same direction, and a Top view would sit under
		// one flat colour. There the studio backdrop and headlight take over,
		// the way Blender's orthographic views are lit.
		env.sky           = sceneEnv.skyEnabled && !p.cam.orthographic();
		env.timeOfDay     = sceneEnv.timeOfDay;
		env.cloudCoverage = sceneEnv.cloudCoverage;
		env.grid          = p.grid && show.groundGrid;
		EditorCameraOverride ov = p.cam.makeOverride();
		ov.editorIcons = show.editorIcons;

		glm::mat4 viewProj(1.0f);
		void* tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *ctx.world,
			static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
			ov, glm::vec3(0.0f), env, &viewProj, /*slot=*/static_cast<uint32_t>(index + 1));
		if (!tex)
		{
			ImGui::TextDisabled("(no secondary viewport on this backend)");
			p.hovered = false;
			ImGui::End();
			return;
		}

		// An item WITH an id under the picture, unlike the Scene window's bare
		// Image: there is no gizmo here to be blocked by it, and it is what
		// keeps an Alt+LMB orbit drag from being read as "drag the window".
		ImGui::InvisibleButton("##svpImage", av,
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
		                       ImGuiButtonFlags_MouseButtonMiddle);
		p.hovered = ImGui::IsItemHovered();
		const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
		ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(tex),
			org, ImVec2(org.x + av.x, org.y + av.y),
			flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));

		if (show.selection) drawSelectionBox(ctx, viewProj, org, av);

		ImGui::End();
	}
}

void render(AppContext& ctx, float dt)
{
	for (int i = 0; i < kCount; ++i)
		renderPane(ctx, s_panes[i], i, dt);
}

#else

void render(AppContext&, float) {}

#endif // HE_IMGUI_ENABLED

} // namespace SecondaryViewportPanel
