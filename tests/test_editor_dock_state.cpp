#include "doctest.h"

#include "EditorDockState.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <vector>

// ── "Is this panel docked into the layout?" ──────────────────────────────────
// The editor remembers which View-menu panels were open, but only the DOCKED
// ones: a docked panel is part of how the user's editor looks, a floating one
// was pulled up to glance at. That distinction rests entirely on the predicate
// below, and getting it wrong fails in the quietest possible way — panels
// simply stop coming back, with nothing in a log and nothing to see until
// someone notices they keep re-ticking the same menu item.
//
// So the cases are driven through REAL docking rather than asserted from the
// header: imgui runs headless given a context and a display size, and
// DockBuilder puts windows exactly where a user's drag would.
//
// Note for anyone extending this: windows under test must NOT carry
// ImGuiWindowFlags_NoSavedSettings. That flag makes imgui skip applying stored
// window settings, and the DockId that DockBuilderDockWindow assigns to a
// not-yet-created window travels in exactly those settings — so the window
// comes up undocked and every case here passes for the wrong reason. Disk is
// kept clean with io.IniFilename = nullptr instead.

namespace {

struct ImGuiCtx
{
	ImGuiCtx()
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize   = ImVec2(1280.0f, 720.0f);
		io.DeltaTime     = 1.0f / 60.0f;
		io.IniFilename   = nullptr;   // never touch the developer's imgui.ini
		io.LogFilename   = nullptr;
		io.ConfigFlags  |= ImGuiConfigFlags_DockingEnable;
		// No renderer: claim texture support so imgui never waits on a backend
		// to upload the font atlas (1.92's dynamic-font path).
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	}
	~ImGuiCtx() { ImGui::DestroyContext(); }
};

// The editor's own arrangement in miniature: a dockspace with one side split
// off it, which is what dragging a panel to an edge produces. `panels` are
// docked into that split, in order — so anything after the first lands as a
// background tab.
void runDockedFrames(const std::vector<const char*>& panels, int frames = 3)
{
	for (int frame = 0; frame < frames; ++frame)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
		ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoSavedSettings);
		const ImGuiID dockspaceId = ImGui::GetID("TestDockspace");
		if (frame == 0)
		{
			ImGui::DockBuilderRemoveNode(dockspaceId);
			ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(dockspaceId, ImVec2(1280.0f, 720.0f));
			ImGuiID main  = dockspaceId;
			ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.26f, nullptr, &main);
			for (const char* p : panels) ImGui::DockBuilderDockWindow(p, right);
			ImGui::DockBuilderDockWindow("Scene", main);
			ImGui::DockBuilderFinish(dockspaceId);
		}
		ImGui::DockSpace(dockspaceId);
		ImGui::End();

		ImGui::Begin("Scene"); ImGui::End();
		for (const char* p : panels) { ImGui::Begin(p); ImGui::End(); }
		ImGui::Render();
	}
}

} // namespace

TEST_CASE("A panel docked alone in a split counts as docked")
{
	// One panel dragged to an edge — the ordinary case, and the one the whole
	// feature exists for.
	ImGuiCtx ctx;
	runDockedFrames({ "Source Control" });
	CHECK(EditorDockState::isDockedInLayout("Source Control"));
}

TEST_CASE("A panel docked as a background tab still counts as docked")
{
	// The case most likely to regress: Source Control sharing a tab strip with
	// another panel and NOT being the selected tab. It is still exactly as
	// docked as the visible one, and quitting from here must remember it — a
	// predicate keyed off visibility instead of membership would say otherwise.
	ImGuiCtx ctx;
	runDockedFrames({ "Details", "Source Control" });

	// Assert the setup before the result: two windows sharing one node, exactly
	// one of them the selected tab. Without this the test could quietly become
	// two side-by-side panels and still pass, proving nothing about tabs.
	const ImGuiWindow* a = ImGui::FindWindowByName("Details");
	const ImGuiWindow* b = ImGui::FindWindowByName("Source Control");
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	REQUIRE(a->DockNode != nullptr);
	REQUIRE(a->DockNode == b->DockNode);
	REQUIRE(a->DockNode->Windows.Size == 2);
	CHECK(a->DockTabIsVisible != b->DockTabIsVisible);

	CHECK(EditorDockState::isDockedInLayout("Details"));
	CHECK(EditorDockState::isDockedInLayout("Source Control"));
}

TEST_CASE("A floating panel does not count as docked")
{
	ImGuiCtx ctx;

	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(100, 100));
	ImGui::SetNextWindowSize(ImVec2(300, 200));
	ImGui::Begin("Collaboration");
	ImGui::End();
	ImGui::Render();

	CHECK_FALSE(EditorDockState::isDockedInLayout("Collaboration"));
}

TEST_CASE("A panel that was never opened does not count as docked")
{
	ImGuiCtx ctx;

	ImGui::NewFrame();
	ImGui::Render();

	// The startup case: the saved preference says "was open", but nothing has
	// drawn it yet. Answering anything but false would let the editor persist a
	// guess about a window that does not exist.
	CHECK_FALSE(EditorDockState::isDockedInLayout("Performance Profiler"));

	// Degenerate inputs — this is reached from a table of panel names.
	CHECK_FALSE(EditorDockState::isDockedInLayout(nullptr));
	CHECK_FALSE(EditorDockState::isDockedInLayout(""));
}

TEST_CASE("Docking is answered without a live ImGui context")
{
	// savePanelVisibility runs at shutdown, alongside ImGui teardown. Reaching
	// into a destroyed context would turn quitting the editor into a crash.
	CHECK(ImGui::GetCurrentContext() == nullptr);
	CHECK_FALSE(EditorDockState::isDockedInLayout("Source Control"));
}
