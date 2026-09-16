#include "UndoHistoryPanel.h"
#include "EditorApplication.h"     // AppContext, EditorUndo, CollabController
#include "EditorWidgets.h"         // the help-aware button
#include "EditorHelp.h"            // "Undo History/<label>" scope for its controls
#include "EditorTheme.h"           // the accent for the current-state row
#include "CollabController.h"      // inSession(): the snapshot stack is idle then

#include <cstdio>
#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace UndoHistoryPanel
{
#ifdef HE_IMGUI_ENABLED

void DrawUndoHistoryWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Undo History/<its label>".
	HE::Ed::Help::Scope helpScope("Undo History");
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(300.0f, 380.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Undo History", &open)) { ImGui::End(); return; }

	if (!ctx.projectLoaded || !ctx.world)
	{
		ImGui::TextDisabled("No scene is open.");
		ImGui::End();
		return;
	}
	// The two states in which this list is not the one being edited: say so
	// rather than show a stale or empty stack.
	if (ctx.collab && ctx.collab->inSession())
	{
		ImGui::TextWrapped("While collaborating, undo applies only to your own changes "
		                   "as inverse operations; the snapshot history is not in use.");
		ImGui::End();
		return;
	}
	EditorUndo* undo = ctx.undoSys;
	if (!undo)
	{
		ImGui::TextDisabled(ctx.isPlaying ? "No undo while playing." : "Undo is unavailable.");
		ImGui::End();
		return;
	}

	// The list is the whole window: a row per step, oldest at the top. One
	// click = one jump. The rows above the marker are what Ctrl+Z walks back
	// through (the nearest to the marker first); the rows below it are what
	// Ctrl+Y walks forward through.
	const size_t nUndo = undo->undoDepth();
	const size_t nRedo = undo->redoDepth();
	{
		char head[96];
		std::snprintf(head, sizeof(head), "%zu step%s to undo, %zu to redo",
		              nUndo, nUndo == 1 ? "" : "s", nRedo, nRedo == 1 ? "" : "s");
		ImGui::TextDisabled("%s", head);
		ImGui::SameLine();
		const float clearW = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - clearW);
		ImGui::BeginDisabled(nUndo == 0 && nRedo == 0);
		if (EditorWidgets::smallButton("Clear")) undo->clearHistory();
		ImGui::EndDisabled();
	}
	ImGui::Separator();

	ImGui::BeginChild("##undoRows", ImVec2(0, 0), ImGuiChildFlags_None);
	int jumpBack = 0, jumpForward = 0;   // rows are read first, the jump runs after

	// Undo rows, oldest first. Row i names the operation that FOLLOWED
	// snapshot i; clicking it restores the state BEFORE that operation, which
	// is (nUndo - i) steps back.
	for (size_t i = 0; i < nUndo; ++i)
	{
		ImGui::PushID((int)i);
		const std::string& label = undo->undoLabelAt(i);
		if (ImGui::Selectable(label.c_str(), false))
			jumpBack = (int)(nUndo - i);
		if (ImGui::IsItemHovered())
		{
			const size_t steps = nUndo - i;
			ImGui::SetTooltip(steps == 1 ? "Undo this step"
			                             : "Undo back to before this step (%zu steps)", steps);
		}
		ImGui::PopID();
	}
	// The marker: where the scene is now. Selected-looking, not clickable —
	// clicking "here" is a no-op and drawing it as a row that does nothing
	// would read as a row that failed.
	{
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::AccentBright);
		ImGui::Selectable("(current state)", true, ImGuiSelectableFlags_Disabled);
		ImGui::PopStyleColor();
		if (nUndo == 0 && nRedo == 0)
			ImGui::TextDisabled("Nothing to undo yet.");
	}
	// Redo rows, nearest first (the top of the redo stack is what Ctrl+Y
	// brings back next). Row k restores the state AFTER its operation, k+1
	// steps forward.
	for (size_t k = 0; k < nRedo; ++k)
	{
		ImGui::PushID(1000 + (int)k);
		const std::string& label = undo->redoLabelAt(nRedo - 1 - k);
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextDim);
		if (ImGui::Selectable(label.c_str(), false))
			jumpForward = (int)k + 1;
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(k == 0 ? "Redo this step"
			                         : "Redo forward through this step (%zu steps)", k + 1);
		ImGui::PopID();
	}
	ImGui::EndChild();

	// One restore for the whole jump; the selection is cleared like every
	// other path through undo/redo (entity handles are remapped on restore).
	if (jumpBack > 0 && undo->undoSteps((size_t)jumpBack))       ctx.selection.clear();
	if (jumpForward > 0 && undo->redoSteps((size_t)jumpForward)) ctx.selection.clear();

	ImGui::End();
}

#else
void DrawUndoHistoryWindow(AppContext&, bool&) {}
#endif

} // namespace UndoHistoryPanel
