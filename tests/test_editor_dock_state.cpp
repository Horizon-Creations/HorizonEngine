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

// ── The editor's two tab states ──────────────────────────────────────────────
// Same arrangement as above, but built on EditorUI's REAL host window name and
// dockspace id, because that pairing is what the keep-alive depends on.
// `dockedPanel` goes into the split, the Scene into the centre.
void buildEditorLayout(const char* dockedPanel)
{
	const ImGuiID dockspaceId = EditorDockState::mainDockspaceId();
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspaceId, ImVec2(1280.0f, 720.0f));
	ImGuiID main  = dockspaceId;
	ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.26f, nullptr, &main);
	ImGui::DockBuilderDockWindow(dockedPanel, right);
	ImGui::DockBuilderDockWindow("Scene", main);
	ImGui::DockBuilderFinish(dockspaceId);
}

// One editor frame, either tab state:
//   sceneTab  — the scene tab is active: the dockspace host window is submitted
//               and draws the layout (EditorUI's normal path).
//   !sceneTab — an asset tab fills the editor: the host window is not submitted
//               at all, only the keep-alive runs.
// The panel is submitted EITHER way, which is the situation being tested: the
// View-menu panels are drawn on every tab so a floating one stays usable.
// `buildLayoutDocking` (first frame only) names the window the layout is built
// around — pass the panel to have it docked, another name to leave it floating.
// Returns what the panel's Begin() said — i.e. whether it is on screen.
bool runEditorFrame(bool sceneTab, const char* panel,
                    const char* buildLayoutDocking = nullptr, bool keepAlive = true)
{
	ImGui::NewFrame();
	if (!sceneTab && keepAlive)
		EditorDockState::keepMainDockspaceAlive();
	if (sceneTab)
	{
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
		ImGui::Begin(EditorDockState::kHostWindowName, nullptr, ImGuiWindowFlags_NoSavedSettings);
		if (buildLayoutDocking) buildEditorLayout(buildLayoutDocking);
		ImGui::DockSpace(EditorDockState::mainDockspaceId());
		ImGui::End();
		ImGui::Begin("Scene"); ImGui::End();
	}
	const bool panelVisible = ImGui::Begin(panel);
	ImGui::End();
	ImGui::Render();
	return panelVisible;
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

TEST_CASE("The dockspace id matches the one the host window produces")
{
	// mainDockspaceId() is ImGui::GetID("##MainDockSpace") called inside the
	// "##EditorDockSpace" window, spelled without needing that window. If the
	// two ever disagree, every saved layout in every user's imgui.ini is filed
	// under an id the editor no longer asks for — the dock arrangement would
	// come up empty and be silently replaced by the default one.
	ImGuiCtx ctx;

	ImGui::NewFrame();
	ImGui::Begin(EditorDockState::kHostWindowName, nullptr, ImGuiWindowFlags_NoSavedSettings);
	const ImGuiID fromHostWindow = ImGui::GetID(EditorDockState::kDockspaceLabel);
	ImGui::End();
	ImGui::Render();

	CHECK(fromHostWindow != 0);
	CHECK(fromHostWindow == EditorDockState::mainDockspaceId());
}

TEST_CASE("A docked panel is hidden while the dockspace is not drawn")
{
	// The rule this whole change is about: a panel docked into the layout
	// belongs to the scene tab. Open a material graph and it has to go with the
	// layout — not hang over the graph because it happens to be submitted on
	// every tab for the sake of the floating case.
	ImGuiCtx ctx;
	runEditorFrame(true, "Performance Profiler", /*dock it*/ "Performance Profiler");
	runEditorFrame(true, "Performance Profiler");
	REQUIRE(EditorDockState::isDockedInLayout("Performance Profiler"));
	REQUIRE(runEditorFrame(true, "Performance Profiler"));   // on screen here

	// Asset tab: submitted, docked — must not draw.
	CHECK_FALSE(runEditorFrame(false, "Performance Profiler"));
	CHECK_FALSE(runEditorFrame(false, "Performance Profiler"));

	// And it kept its place: switching tabs is not allowed to cost the user
	// their layout (see the undock case below for what that looks like).
	CHECK(EditorDockState::isDockedInLayout("Performance Profiler"));

	// Back on the scene tab it returns by itself.
	runEditorFrame(true, "Performance Profiler");
	CHECK(runEditorFrame(true, "Performance Profiler"));
}

TEST_CASE("Without the keep-alive a docked panel is torn out of the layout")
{
	// What keepMainDockspaceAlive() prevents, asserted so the call is not
	// mistaken for a no-op and dropped: ImGui reacts to a docked window whose
	// dockspace node went missing by undocking it. The panel then floats over
	// the asset tab — which is exactly the reported symptom — and its slot in
	// the layout is gone for good.
	ImGuiCtx ctx;
	runEditorFrame(true, "Source Control", /*dock it*/ "Source Control");
	runEditorFrame(true, "Source Control");
	REQUIRE(EditorDockState::isDockedInLayout("Source Control"));

	runEditorFrame(false, "Source Control", nullptr, /*keepAlive*/ false);
	CHECK_FALSE(EditorDockState::isDockedInLayout("Source Control"));
}

TEST_CASE("A panel restored into the layout survives starting on an asset tab")
{
	// The editor reopens the tab that was active last time, and that can be a
	// material graph — so the dockspace host window is never submitted, not even
	// once, and the panel is the first thing to touch the layout. This is why
	// the id is derived from the window NAME instead of read off the live
	// window: there is no live window to read it off here.
	ImGuiCtx ctx;

	ImGui::NewFrame();
	buildEditorLayout("Source Control");           // stands in for the saved ini
	EditorDockState::keepMainDockspaceAlive();
	ImGui::Begin("Source Control"); ImGui::End();
	ImGui::Render();

	CHECK_FALSE(runEditorFrame(false, "Source Control"));
	CHECK(EditorDockState::isDockedInLayout("Source Control"));
}

TEST_CASE("A floating panel is visible on every tab")
{
	// The other half of the rule, and the reason the panels are submitted
	// outside the tab gating at all: a panel the user left floating was pulled
	// up to look at, and it keeps drawing over whichever tab is open.
	ImGuiCtx ctx;
	runEditorFrame(true, "Environment", /*docks someone else*/ "Details");
	runEditorFrame(true, "Environment");
	REQUIRE_FALSE(EditorDockState::isDockedInLayout("Environment"));

	CHECK(runEditorFrame(false, "Environment"));
	CHECK(runEditorFrame(false, "Environment"));
}

TEST_CASE("Docking is answered without a live ImGui context")
{
	// savePanelVisibility runs at shutdown, alongside ImGui teardown. Reaching
	// into a destroyed context would turn quitting the editor into a crash.
	CHECK(ImGui::GetCurrentContext() == nullptr);
	CHECK_FALSE(EditorDockState::isDockedInLayout("Source Control"));
}
