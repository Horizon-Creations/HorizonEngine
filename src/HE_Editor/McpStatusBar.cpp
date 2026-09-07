#include "McpStatusBar.h"

#include "EditorApplication.h"        // AppContext, EditorConfig, McpBridge
#include "EditorSettingsPanel.h"      // clicking the line opens the Remote Control page

#ifdef HE_IMGUI_ENABLED
#include "EditorWidgets.h"            // WrapText, for the tooltip
#include <imgui.h>
#endif

#include <cstdio>
#include <string>

namespace McpStatusBar
{

#ifdef HE_IMGUI_ENABLED
namespace
{
	enum class State { Off, Failed, Idle, Busy };

	State stateOf(AppContext& ctx)
	{
		if (!ctx.mcp) return State::Off;
		if (!ctx.mcp->isRunning())
			// Wanted but not up. The config says on and nothing is listening,
			// because setEnabled ignores a value that has not changed and so a
			// failed start() is never retried. Worth saying out loud; the silent
			// version of this is a user waiting for a client that cannot arrive.
			return ctx.editorConfig.McpServerEnabled ? State::Failed : State::Off;
		return ctx.mcp->clientCount() > 0 ? State::Busy : State::Idle;
	}

	// The whole line, computed once so FooterWidth and DrawFooter cannot drift
	// apart — the sync bar next door learned that the hard way, and a footer
	// widget that measures one string and draws another silently overlaps its
	// left-hand neighbour.
	std::string textFor(AppContext& ctx, State s)
	{
		switch (s)
		{
		case State::Off:    return {};
		case State::Failed: return "Remote Control: not listening";
		case State::Idle:   return "Remote Control: on";
		case State::Busy:
		{
			const std::size_t n = ctx.mcp->clientCount();
			char buf[64];
			std::snprintf(buf, sizeof(buf), "Remote Control: %zu client%s",
			              n, n == 1 ? "" : "s");
			return buf;
		}
		}
		return {};
	}

	ImVec4 colourFor(State s)
	{
		switch (s)
		{
		// Amber, the same one the settings page and the toolbar use for "needs
		// attention": the setting is on and it is not doing what it says.
		case State::Failed: return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
		// On but nobody connected — true and not urgent, so it is dimmed like
		// the rest of the ambient footer text.
		case State::Idle:   return ImVec4(0.65f, 0.65f, 0.65f, 0.9f);
		// Somebody IS driving the editor. Deliberately the brightest of the
		// three: this is the state where an edit appearing under the cursor was
		// not the user's, and it should not read as ambient.
		case State::Busy:   return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
		default:            return ImVec4(0.65f, 0.65f, 0.65f, 0.9f);
		}
	}
}
#endif

float FooterWidth(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	const State s = stateOf(ctx);
	if (s == State::Off) return 0.0f;
	return ImGui::CalcTextSize(textFor(ctx, s).c_str()).x;
#else
	(void)ctx;
	return 0.0f;
#endif
}

void DrawFooter(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	const State s = stateOf(ctx);
	if (s == State::Off) return;

	const std::string text = textFor(ctx, s);

	ImGui::PushStyleColor(ImGuiCol_Text, colourFor(s));
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopStyleColor();

	const bool clicked = ImGui::IsItemClicked();
	if (ImGui::IsItemHovered())
	{
		// Spelled out rather than SetTooltip: the endpoint path is of unbounded
		// length, and a tooltip has no width of its own — an unwrapped one paints
		// a strip of text across the whole screen. Same reasoning, and the same
		// explicit wrap column, as the download queue beside it.
		if (ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 32.0f);
				switch (s)
				{
				case State::Failed:
					ImGui::TextUnformatted(
						"Remote control is switched on, but the listener could not be "
						"opened — the port may be taken. No external tool can connect. "
						"Click to open Preferences.");
					break;
				case State::Idle:
					ImGui::Text(
						"This editor can be driven by an external tool on this machine.\n"
						"Listening on 127.0.0.1:%u, nobody connected.\n"
						"Endpoint: %s\n\nClick to open Preferences.",
						static_cast<unsigned>(ctx.mcp->port()),
						ctx.mcp->endpointFile().string().c_str());
					break;
				case State::Busy:
					// Names the consequence, not the count: the useful thing to
					// know is that changes in this scene may not be yours.
					ImGui::Text(
						"An external tool is connected to this editor and may be "
						"changing the scene — its edits go through the same gateway as "
						"yours, so they are in Undo.\n"
						"%zu connected on 127.0.0.1:%u.\n\nClick to open Preferences.",
						ctx.mcp->clientCount(), static_cast<unsigned>(ctx.mcp->port()));
					break;
				default:
					break;
				}
			}
			ImGui::EndTooltip();
		}
	}

	if (clicked)
		EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::RemoteControl);
#else
	(void)ctx;
#endif
}

} // namespace McpStatusBar
