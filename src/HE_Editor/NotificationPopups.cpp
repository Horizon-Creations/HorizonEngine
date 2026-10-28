#include "NotificationPopups.h"

#include "EditorApplication.h"   // AppContext, and NotificationStore through it
#include "NotificationBar.h"     // where the footer is, so toasts sit above it

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include "EditorWidgets.h"
#include "EditorHelp.h"
#include "EditorTheme.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace NotificationPopups
{
#ifdef HE_IMGUI_ENABLED
namespace
{
	using HE::Ed::NoteLevel;
	using HE::Ed::Notification;
	// levelColor() (NotificationStore.h): the same three the bell and the
	// centre use, so a toast and its row are recognisably one thing.
	using HE::Ed::levelColor;

	// ── Toasts ───────────────────────────────────────────────────────────────
	constexpr float kToastBodyW  = 340.0f;   // text column; the window adds padding + the close button
	constexpr float kToastMargin = 12.0f;    // from the viewport edge and between toasts
	constexpr std::size_t kMaxToasts = 4;    // more than this is a wall, not a notification

	void drawToast(AppContext& ctx, const Notification& n, std::uint64_t now,
	               const ImGuiViewport* vp, float right, float& bottom)
	{
		char name[48];
		std::snprintf(name, sizeof(name), "##notificationToast%llu",
		              static_cast<unsigned long long>(n.id));

		ImGui::SetNextWindowPos(ImVec2(right, bottom), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowViewport(vp->ID);
		ImGui::SetNextWindowBgAlpha(0.96f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

		// NoFocusOnAppearing + NoNav: a toast that arrives while you are typing
		// must not take the keyboard from the field. Clicking a button on it is the
		// user's own choice and focuses it like any other click.
		if (ImGui::Begin(name, nullptr,
		                 ImGuiWindowFlags_NoTitleBar         |
		                 ImGuiWindowFlags_NoResize           |
		                 ImGuiWindowFlags_NoMove             |
		                 ImGuiWindowFlags_NoScrollbar        |
		                 ImGuiWindowFlags_NoSavedSettings    |
		                 ImGuiWindowFlags_NoDocking          |
		                 ImGuiWindowFlags_NoFocusOnAppearing |
		                 ImGuiWindowFlags_NoNav              |
		                 ImGuiWindowFlags_AlwaysAutoResize))
		{
			HE::Ed::Help::Scope helpScope("Notifications");

			// The pointer on a toast keeps it: nobody reads a sentence, reaches for
			// the button, and has it vanish under the cursor.
			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
			                           ImGuiHoveredFlags_ChildWindows))
				ctx.notifications->holdToast(n.id, now);

			const ImVec2 winPos = ImGui::GetWindowPos();
			const float  padX   = ImGui::GetStyle().WindowPadding.x;
			const float  startX = ImGui::GetCursorPosX();
			const ImVec4 col    = levelColor(n.level);

			// Close, top-right, first so the window is as wide as body + button, and
			// the cursor then goes back to where the text starts (a cursor left
			// BEYOND the content is what ImGui asserts on, one before it is fine).
			{
				const ImVec2 origin = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(startX + kToastBodyW + padX * 0.5f, 4.0f));
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				if (EditorWidgets::smallButton("x"))
					ctx.notifications->dismissToast(n.id);
				ImGui::PopStyleColor();
				ImGui::SetCursorPos(origin);
			}

			{
				EditorWidgets::WrapText wrap(startX + kToastBodyW);
				if (!n.title.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, col);
					ImGui::TextUnformatted(n.title.c_str());
					ImGui::PopStyleColor();
				}
				ImGui::TextUnformatted(n.text.c_str());
				if (!n.detail.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextUnformatted(n.detail.c_str());
					ImGui::PopStyleColor();
				}
			}

			if (!n.actions.empty() && !n.handled)
			{
				ImGui::Spacing();
				DrawActions(ctx, n);
			}

			// Accent bar on the left and the time left along the bottom, both
			// painted on the window so they cannot disturb the layout.
			ImDrawList*  dl   = ImGui::GetWindowDrawList();
			const ImVec2 size = ImGui::GetWindowSize();
			dl->AddRectFilled(winPos, ImVec2(winPos.x + 3.0f, winPos.y + size.y),
			                  ImGui::GetColorU32(col), 6.0f, ImDrawFlags_RoundCornersLeft);
			if (n.timeoutMs > 0 && n.toastUntilMs > now)
			{
				const float left = std::min(1.0f, static_cast<float>(n.toastUntilMs - now) /
				                                  static_cast<float>(n.timeoutMs));
				dl->AddRectFilled(ImVec2(winPos.x + 3.0f, winPos.y + size.y - 2.0f),
				                  ImVec2(winPos.x + 3.0f + (size.x - 3.0f) * left, winPos.y + size.y),
				                  ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.55f)));
			}

			bottom -= size.y + kToastMargin;
		}
		else
		{
			// Collapsed away this frame (the auto-size's hidden first frame): keep the
			// stack from jumping by reserving the last known slot.
			bottom -= 60.0f + kToastMargin;
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	void drawToasts(AppContext& ctx)
	{
		const std::uint64_t now = HE::Ed::NotificationStore::nowMs();
		std::vector<Notification> live = ctx.notifications->liveToasts(now);
		if (live.empty()) return;
		if (live.size() > kMaxToasts) live.erase(live.begin(), live.end() - kMaxToasts);

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float right = vp->WorkPos.x + vp->WorkSize.x - kToastMargin;
		// Above the footer when it drew this frame (the bell is in it), else the
		// bottom of the work area. Newest nearest the corner, like every OS's.
		float bottom = vp->WorkPos.y + vp->WorkSize.y - kToastMargin;
		float footerTop = 0.0f;
		if (NotificationBar::FooterTop(footerTop)) bottom = std::min(bottom, footerTop - kToastMargin * 0.5f);

		for (auto it = live.rbegin(); it != live.rend(); ++it)
			drawToast(ctx, *it, now, vp, right, bottom);
	}

	// ── The modal ────────────────────────────────────────────────────────────
	constexpr const char* kModalName = "##notificationModal";
	std::uint64_t s_modalId = 0;   // the notification the open popup is showing

	void drawModal(AppContext& ctx)
	{
		Notification n;
		const bool have = ctx.notifications->nextModal(n);

		// The one on screen was answered, withdrawn or cleared: close it first, and
		// let the next one (if any) open on the following frame, as the recovery
		// dialogs do — OpenPopup in the same frame would race the close.
		if (s_modalId != 0 && (!have || n.id != s_modalId))
		{
			if (ImGui::IsPopupOpen(kModalName) && ImGui::BeginPopupModal(kModalName))
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			s_modalId = 0;
			return;
		}
		if (!have) return;

		if (s_modalId == 0)
		{
			// Never over another root-level modal: OpenPopup at root level replaces
			// whatever is open. The notification is still pending next frame.
			if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
				return;
			ImGui::OpenPopup(kModalName);
			s_modalId = n.id;
		}

		constexpr float kWidth = 460.0f;
		ImGui::SetNextWindowSize(ImVec2(kWidth, 0.0f), ImGuiCond_Appearing);
		EditorWidgets::pinDialogToEditorWindow();
		if (!ImGui::BeginPopupModal(kModalName, nullptr,
		                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
		                            ImGuiWindowFlags_NoSavedSettings))
		{
			// ImGui closed it under us (a click outside does not, but a parent popup
			// going away does): stop tracking, the entry is still pending and reopens.
			s_modalId = 0;
			return;
		}

		HE::Ed::Help::Scope helpScope("Notifications");
		const float wrapX = kWidth - ImGui::GetStyle().WindowPadding.x;
		bool done = false;
		{
			EditorWidgets::WrapText wrap(wrapX);
			ImGui::PushStyleColor(ImGuiCol_Text, levelColor(n.level));
			ImGui::TextUnformatted(n.title.empty() ? "Something went wrong" : n.title.c_str());
			ImGui::PopStyleColor();
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextUnformatted(n.text.c_str());
			if (!n.detail.empty())
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextUnformatted(n.detail.c_str());
				ImGui::PopStyleColor();
			}
			if (!n.assetPath.empty())
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.70f, 0.95f, 0.9f));
				ImGui::TextUnformatted(n.assetPath.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::Spacing();
		}

		ImGui::Separator();
		ImGui::Spacing();

		// The poster's buttons first; a modal always ends in a way out. With nothing
		// to choose between, that way out is the primary button.
		const bool hasActions = !n.actions.empty() && !n.handled;
		if (hasActions)
		{
			// An action settles the entry itself; next frame's nextModal() no longer
			// returns it and the popup closes above. (A keep-open action leaves it up.)
			DrawActions(ctx, n);
			ImGui::SameLine();
			if (EditorWidgets::button("Dismiss")) done = true;
		}
		else if (EditorWidgets::primaryButton("OK"))
		{
			done = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;

		if (done)
		{
			// Idempotent when an action already settled it.
			ctx.notifications->acknowledge(n.id);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
} // namespace
#endif // HE_IMGUI_ENABLED

bool DrawActions(AppContext& ctx, const HE::Ed::Notification& n)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.notifications || n.handled || n.actions.empty()) return false;
	bool pressed = false;
	for (std::size_t i = 0; i < n.actions.size(); ++i)
	{
		const HE::Ed::NoteAction& a = n.actions[i];
		if (i > 0) ImGui::SameLine();
		ImGui::PushID(static_cast<int>(i));
		const bool clicked = a.primary ? EditorWidgets::primaryButton(a.label.c_str())
		                               : EditorWidgets::button(a.label.c_str());
		ImGui::PopID();
		// One press per frame: the callback may rewrite the store under the copy this
		// loop is drawing from, and the rest of the row is stale after it.
		if (clicked && !pressed)
		{
			ctx.notifications->runAction(n.id, i);
			pressed = true;
		}
	}
	return pressed;
#else
	(void)ctx; (void)n;
	return false;
#endif
}

void DrawOverlay(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.notifications) return;
	drawToasts(ctx);
	drawModal(ctx);
#else
	(void)ctx;
#endif
}

} // namespace NotificationPopups
