#include "MeshMaterialSlots.h"

#if __has_include(<imgui.h>)

#include "EditorWidgets.h"   // helpForKey — the row's tooltip
#include <ContentManager/ContentManager.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace HE::Ed::MeshMaterialSlots
{

namespace
{
	// Snapshots the stack keeps before the oldest is dropped. Slot edits are
	// small and rare; 64 is the MaterialEditorPanel's number and more than any
	// session will step back through.
	constexpr size_t kUndoCap = 64;

	bool sameSection(const MeshSection& a, const MeshSection& b)
	{
		return a.indexOffset == b.indexOffset && a.indexCount == b.indexCount &&
		       a.materialPath == b.materialPath && a.materialId == b.materialId;
	}
}

bool Table::operator==(const Table& o) const
{
	if (ownPath != o.ownPath || ownId != o.ownId) return false;
	if (sections.size() != o.sections.size()) return false;
	for (size_t i = 0; i < sections.size(); ++i)
		if (!sameSection(sections[i], o.sections[i])) return false;
	return true;
}

bool setSlot(ContentManager& cm, Table& t, size_t index, HE::UUID id)
{
	if (index >= t.sections.size()) return false;
	std::string path;
	if (id != HE::UUID{})
	{
		const MaterialAsset* mat = cm.getMaterial(id);
		if (!mat) return false;
		path = mat->path;   // copied out: nothing below loads, but the rule is the rule
	}
	MeshSection& sec = t.sections[index];
	sec.materialPath = path;
	// The path is the editor's form of the reference; a baked id left beside a
	// new path would win over it in every resolver (resolveMaterialRef prefers
	// the id), so the old one has to go with the old path.
	sec.materialId   = HE::UUID{};
	if (index == 0)
	{
		t.ownPath = path;
		t.ownId   = HE::UUID{};
	}
	return true;
}

// ── Session ──────────────────────────────────────────────────────────────────
void Session::seed(const Table& t)
{
	if (!undo.empty()) return;
	undo.push_back(t);
	undoPos = 0;
}

void Session::push(const Table& t)
{
	if (undoPos >= 0 && undoPos < static_cast<int>(undo.size()) && undo[undoPos] == t)
		return; // a no-op edit is not an undo step
	undo.resize(static_cast<size_t>(undoPos + 1)); // drop the redo tail
	undo.push_back(t);
	if (undo.size() > kUndoCap) undo.erase(undo.begin());
	undoPos = static_cast<int>(undo.size()) - 1;
}

bool Session::step(int dir, Table& out)
{
	const int target = undoPos + dir;
	if (target < 0 || target >= static_cast<int>(undo.size())) return false;
	undoPos = target;
	out     = undo[static_cast<size_t>(target)];
	return true;
}

HE::UUID slotId(ContentManager& cm, const MeshSection& sec, Session& session)
{
	if (sec.materialId != HE::UUID{}) return sec.materialId;
	if (sec.materialPath.empty())     return {};
	// A pure lookup first: it is what the extractor does too, and it never
	// touches the disk. Only a path nothing resident answers to is loaded — once;
	// a path that is not on disk either goes into `missing` and stays a label.
	const HE::UUID resident = cm.idForPath(sec.materialPath);
	if (resident != HE::UUID{}) return resident;
	if (session.missing.count(sec.materialPath)) return {};
	const HE::UUID loaded = cm.loadAsset(sec.materialPath);
	if (loaded == HE::UUID{} || !cm.getMaterial(loaded))
	{
		session.missing.insert(sec.materialPath);
		return {};
	}
	return loaded;
}

// ── The block ────────────────────────────────────────────────────────────────
bool draw(ContentManager* cm, Table& table, Session& session, const SlotWidget& slot)
{
	session.seed(table);

	const size_t n = table.sections.size();
	char heading[48];
	std::snprintf(heading, sizeof(heading), "Materials (%zu slot%s)", n, n == 1 ? "" : "s");
	ImGui::SeparatorText(heading);

	bool changed = false;
	for (size_t i = 0; i < n; ++i)
	{
		// Read before the widget: `sec` is a reference into `table`, which the
		// widget cannot move (the table is the caller's copy) — but the
		// content manager it may load through CAN move the material the path
		// names, which is why setSlot re-fetches by id rather than keeping one.
		const MeshSection& sec = table.sections[i];
		HE::UUID id = cm ? slotId(*cm, sec, session) : HE::UUID{};

		// What an empty picker says depends on the row. Slot 0 IS the mesh's
		// own material, so empty there is "none at all"; every other slot's
		// empty reference resolves to slot 0 (MeshSection, Assets.h). A path
		// that no longer resolves is printed rather than hidden behind "none":
		// the asset still points at it, and the user should see what it was.
		std::string emptyText;
		if (!sec.materialPath.empty())
			emptyText = "(missing: " + sec.materialPath + ")";
		else
			emptyText = (i == 0) ? "(none)" : "(same as slot 0)";

		ImGui::PushID(static_cast<int>(i));
		ImGui::Text("%zu", i);
		ImGui::SameLine();
		const EditorWidgets::SlotAction act = slot(i, id, emptyText.c_str());
		// Keyed by hand: the row's label is its number, which is data.
		EditorWidgets::helpForKey("Mesh Viewer/Material Slots");
		ImGui::SameLine();
		ImGui::TextDisabled("%u tris", sec.indexCount / 3);
		ImGui::PopID();

		if (act != EditorWidgets::SlotAction::None && cm)
		{
			// Assigned → `id` is the picked material; Cleared → zero. Both go
			// through setSlot so slot 0's mirror never drifts.
			if (setSlot(*cm, table, i, act == EditorWidgets::SlotAction::Cleared ? HE::UUID{} : id))
				changed = true;
		}
	}

	if (changed) session.push(table);
	return changed;
}

bool handleUndoKeys(Table& table, Session& session)
{
	const ImGuiIO& io = ImGui::GetIO();
	const bool mod  = io.KeyCtrl || io.KeySuper;
	const bool kbOk = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
	                  !io.WantTextInput && !ImGui::IsAnyItemActive();
	if (!kbOk || !mod) return false;
	if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
		return session.step(io.KeyShift ? +1 : -1, table);
	if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
		return session.step(+1, table);
	return false;
}

} // namespace HE::Ed::MeshMaterialSlots

#endif // __has_include(<imgui.h>)
