#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "NotificationPopups.h"
#include "NotificationStore.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, the nav window

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ── Toasts in the corner and the critical modal, driven headless ─────────────
// What the store decides is covered in test_notification_store.cpp; this is the
// drawing half. What matters: an Important notification shows as a toast in the
// bottom-right corner that does not take focus, its buttons reach the store, a
// Critical one opens a modal that waits for any dialog already open and is
// dismissed by Escape or its button, and Info never draws anything. HE_UI_DUMP_DIR
// writes the frames as BMP.

using namespace HE::Ed;

namespace
{
	constexpr int W = 900, H = 600;

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
			applyHorizonDarkTheme();
		}
		~Harness() { ImGui::DestroyContext(); }
	};

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
		NotificationStore store;

		AppContext make()
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
			ctx.notifications = &store;
			return ctx;
		}
	};

	// One frame of just the popups, optionally with a modal of someone else's open.
	void frame(ContextBits& bits, he_ui::Image* shot = nullptr, bool otherModal = false)
	{
		AppContext ctx = bits.make();
		ImGui::NewFrame();
		if (otherModal)
		{
			if (!ImGui::IsPopupOpen("Other dialog")) ImGui::OpenPopup("Other dialog");
			if (ImGui::BeginPopupModal("Other dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted("a dialog the notification must not replace");
				ImGui::EndPopup();
			}
		}
		NotificationPopups::DrawOverlay(ctx);
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	}

	void frames(ContextBits& bits, int n)
	{
		for (int i = 0; i < n; ++i) frame(bits);
	}

	void dump(ContextBits& bits, const char* name, bool otherModal = false)
	{
		he_ui::Image img;
		frame(bits, &img, otherModal);
		if (const char* d = std::getenv("HE_UI_DUMP_DIR"); d && *d)
			he_ui::writeBmp(img, std::string(d) + "/" + name + ".bmp");
	}

	ImGuiWindow* toastWindow(std::uint64_t id)
	{
		char name[48];
		std::snprintf(name, sizeof(name), "##notificationToast%llu", (unsigned long long)id);
		return ImGui::FindWindowByName(name);
	}

	int toastCount(ContextBits& bits)
	{
		int n = 0;
		for (const Notification& e : bits.store.snapshot())
			if (ImGuiWindow* w = toastWindow(e.id); w && w->WasActive) ++n;
		return n;
	}

	NoteSpec invitation(int* joined, int* ignored)
	{
		NoteSpec s;
		s.level = NoteLevel::Important;
		s.title = "Collaboration invitation";
		s.text  = "Anna invited you to a collaboration session.";
		s.actions.push_back({ "Join",   [joined]  { ++*joined; },  true });
		s.actions.push_back({ "Ignore", [ignored] { ++*ignored; }, false });
		return s;
	}

	// Press and release at (x, y), one frame each.
	void click(ContextBits& bits, float x, float y)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		frame(bits);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		frame(bits);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame(bits);
	}

	// Sweeps the toast's button row until a click lands on something that counts.
	template <class Done>
	bool sweepClick(ContextBits& bits, ImGuiWindow* w, Done done)
	{
		const float y = w->Pos.y + w->Size.y - 10.0f - 11.0f;   // padding + half a button
		for (float x = w->Pos.x + 16.0f; x < w->Pos.x + w->Size.x - 8.0f; x += 5.0f)
		{
			click(bits, x, y);
			if (done()) return true;
			if (!w->WasActive) return false;
		}
		return false;
	}
}

TEST_CASE("Notification popups: Info draws nothing")
{
	Harness h;
	ContextBits bits;
	const auto id = bits.store.post(NoteLevel::Info, "A teammate joined.");
	frames(bits, 4);
	CHECK(toastWindow(id) == nullptr);
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);
}

TEST_CASE("Notification popups: an Important notification is a toast in the bottom-right corner")
{
	Harness h;
	ContextBits bits;
	int joined = 0, ignored = 0;
	const auto id = bits.store.post(invitation(&joined, &ignored));
	frames(bits, 4);

	ImGuiWindow* w = toastWindow(id);
	REQUIRE(w != nullptr);
	REQUIRE(w->WasActive);
	// Hugging the bottom-right corner, fully on screen.
	CHECK(w->Pos.x > float(W) * 0.5f);
	CHECK(w->Pos.y > float(H) * 0.5f);
	CHECK(w->Pos.x + w->Size.x <= float(W));
	CHECK(w->Pos.y + w->Size.y <= float(H));
	CHECK(w->Pos.x + w->Size.x > float(W) - 40.0f);
	CHECK(w->Pos.y + w->Size.y > float(H) - 40.0f);
	// It must not take the keyboard: nothing was clicked, so nothing is focused on it.
	CHECK(ImGui::GetCurrentContext()->NavWindow != w);
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);

	dump(bits, "notification_toast_invite");
}

TEST_CASE("Notification popups: Join runs the action, answers the row and closes the toast")
{
	Harness h;
	ContextBits bits;
	int joined = 0, ignored = 0;
	const auto id = bits.store.post(invitation(&joined, &ignored));
	frames(bits, 4);
	ImGuiWindow* w = toastWindow(id);
	REQUIRE(w != nullptr);

	REQUIRE(sweepClick(bits, w, [&] { return joined + ignored > 0; }));
	CHECK(joined == 1);      // the first button the sweep reaches is the primary one
	CHECK(ignored == 0);
	frames(bits, 3);
	CHECK(toastCount(bits) == 0);
	const auto rows = bits.store.snapshot();
	REQUIRE(rows.size() == 1);
	CHECK(rows[0].handledBy == "Join");
}

TEST_CASE("Notification popups: Ignore is the secondary button")
{
	Harness h;
	ContextBits bits;
	int joined = 0, ignored = 0;
	const auto id = bits.store.post(invitation(&joined, &ignored));
	frames(bits, 4);
	ImGuiWindow* w = toastWindow(id);
	REQUIRE(w != nullptr);

	// From the right edge inwards, so the second button is the one reached first.
	const float y = w->Pos.y + w->Size.y - 10.0f - 11.0f;
	for (float x = w->Pos.x + w->Size.x - 30.0f; x > w->Pos.x + 16.0f; x -= 5.0f)
	{
		click(bits, x, y);
		if (joined + ignored > 0) break;
	}
	CHECK(ignored == 1);
	CHECK(joined == 0);
}

TEST_CASE("Notification popups: the toast's close button dismisses it but keeps the row unread")
{
	Harness h;
	ContextBits bits;
	const auto id = bits.store.post(NoteLevel::Important, "Something half-worked.");
	frames(bits, 4);
	ImGuiWindow* w = toastWindow(id);
	REQUIRE(w != nullptr);

	// The close button sits in the top-right padding.
	bool closed = false;
	for (float y = w->Pos.y + 2.0f; y < w->Pos.y + 26.0f && !closed; y += 3.0f)
		for (float x = w->Pos.x + w->Size.x - 4.0f; x > w->Pos.x + w->Size.x - 34.0f && !closed; x -= 3.0f)
		{
			click(bits, x, y);
			closed = bits.store.liveToasts(NotificationStore::nowMs()).empty();
		}
	CHECK(closed);
	CHECK(bits.store.unseenCount() == 1);
}

TEST_CASE("Notification popups: at most four toasts are drawn")
{
	Harness h;
	ContextBits bits;
	for (int i = 0; i < 7; ++i)
		bits.store.post(NoteLevel::Important, "toast number " + std::to_string(i));
	frames(bits, 4);
	CHECK(toastCount(bits) == 4);
	dump(bits, "notification_toast_stack");
}

TEST_CASE("Notification popups: a Critical notification opens a modal that Escape acknowledges")
{
	Harness h;
	ContextBits bits;
	int retried = 0;
	NoteSpec s;
	s.level = NoteLevel::Critical;
	s.title = "Could not join the session";
	s.text  = "Could not reach 10.0.0.5:7777.";
	s.actions.push_back({ "Try again", [&retried] { ++retried; }, true });
	bits.store.post(s);

	frames(bits, 4);
	REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
	dump(bits, "notification_modal_join_failed");

	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(bits);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	frames(bits, 3);

	CHECK(ImGui::GetTopMostPopupModal() == nullptr);
	CHECK(retried == 0);   // dismissing is not choosing
	Notification m;
	CHECK_FALSE(bits.store.nextModal(m));
	CHECK(bits.store.unseenCount() == 0);
}

TEST_CASE("Notification popups: a modal's own button runs the action and closes the dialog")
{
	Harness h;
	ContextBits bits;
	int retried = 0;
	NoteSpec s;
	s.level = NoteLevel::Critical;
	s.text  = "The link dropped.";
	s.actions.push_back({ "Rejoin", [&retried] { ++retried; }, true });
	bits.store.post(s);
	frames(bits, 4);
	ImGuiWindow* w = ImGui::GetTopMostPopupModal();
	REQUIRE(w != nullptr);

	// The action row is the last line of the dialog.
	const float y = w->Pos.y + w->Size.y - 10.0f - 11.0f;
	for (float x = w->Pos.x + 16.0f; x < w->Pos.x + 200.0f && retried == 0; x += 5.0f)
		click(bits, x, y);
	CHECK(retried == 1);
	frames(bits, 3);
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);
}

TEST_CASE("Notification popups: a modal waits for a dialog that is already open")
{
	Harness h;
	ContextBits bits;
	NoteSpec s;
	s.level = NoteLevel::Critical;
	s.text  = "Wait your turn.";
	bits.store.post(s);

	for (int i = 0; i < 5; ++i) frame(bits, nullptr, /*otherModal*/ true);
	ImGuiWindow* top = ImGui::GetTopMostPopupModal();
	REQUIRE(top != nullptr);
	CHECK(std::string(top->Name).find("Other dialog") != std::string::npos);
	dump(bits, "notification_modal_waiting", true);

	// Still pending, not lost.
	Notification m;
	CHECK(bits.store.nextModal(m));

	// The other dialog goes away; the notification takes its place.
	ImGui::ClosePopupToLevel(0, false);
	frames(bits, 4);
	REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);
	CHECK(std::string(ImGui::GetTopMostPopupModal()->Name).find("notificationModal") != std::string::npos);
}

TEST_CASE("Notification popups: a modal withdrawn while open closes without being clicked")
{
	Harness h;
	ContextBits bits;
	NoteSpec s;
	s.level = NoteLevel::Critical;
	s.text  = "No longer true.";
	s.key   = "gone";
	bits.store.post(s);
	frames(bits, 4);
	REQUIRE(ImGui::GetTopMostPopupModal() != nullptr);

	bits.store.resolve("gone");
	frames(bits, 4);
	CHECK(ImGui::GetTopMostPopupModal() == nullptr);
}
