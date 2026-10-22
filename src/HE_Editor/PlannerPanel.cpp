#include "PlannerPanel.h"

#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "Planner.h"
#include "PlannerImport.h"
#include "PlannerWorkspace.h"
#include "ProjectManager.h"
#include "TaskBoard.h"
#include "TaskBoardPanel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>   // InputText over std::string

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <vector>

using namespace HE;
using namespace HE::Planner;
namespace Theme = HE::Ed::Theme;

namespace PlannerPanel
{

namespace
{

// Every control below is drawn inside render(), which pushes the help scope
//     Help::Scope helpScope("Planner")
// further down the file than the helpers that draw them. The line above is for
// scripts/editor_help_audit.py, which reads the file top to bottom and has to
// know the scope before it meets the first label.

Workspace s_ws;
UUID      s_dbId, s_viewId;
std::string s_search;
std::string s_notice;

// The text field being typed in: which cell, and what is in it so far. A cell
// shows the stored value until it is activated, then this, and writes when it
// is left.
struct EditKey
{
	UUID row, prop;
	bool operator==(const EditKey& o) const { return row == o.row && prop == o.prop; }
};
EditKey     s_editKey;
bool        s_editing = false;
std::string s_editBuf;

std::string s_newOption, s_newProperty, s_renameView;
int         s_newPropertyType = 0;
UUID        s_armedDelete;        // the property whose "Delete Property" was pressed once
UUID        s_renamingView;

std::vector<std::function<void()>> s_posted;
void post(std::function<void()> fn) { s_posted.push_back(std::move(fn)); }

DrawStats                             s_stats;
std::unordered_map<std::string, Rect> s_rects;

// Remember where the item just submitted is, under a name a test can ask for.
void note(const std::string& name)
{
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	s_rects[name] = { a.x, a.y, b.x, b.y };
}

const char* const kTypeNames[] = { "Text", "Number", "Checkbox", "Date", "Select", "Multi-select",
                                   "Person", "Relation", "Engine link", "URL" };
constexpr int kTypeCount = 10;   // one per PropertyType before Other

const char* const kFilterOps[] = { "equals", "not-equals", "contains", "not-contains", "is-empty",
                                   "is-not-empty", "gt", "gte", "lt", "lte" };
const char* const kFilterOpLabels[] = { "is", "is not", "contains", "does not contain", "is empty",
                                        "is not empty", "is above", "is at least", "is below", "is at most" };

void pushUuid(const UUID& id)
{
	ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(id.hi * 31u ^ id.lo)));
}

ImVec4 colorOf(const std::string& word)
{
	if (word == "blue")   return ImVec4(0.40f, 0.60f, 0.95f, 1);
	if (word == "green")  return Theme::Ok;
	if (word == "red")    return Theme::Bad;
	if (word == "orange") return Theme::Warn;
	if (word == "yellow") return Theme::AccentBright;
	if (word == "purple") return ImVec4(0.70f, 0.52f, 0.90f, 1);
	if (word == "pink")   return ImVec4(0.92f, 0.52f, 0.72f, 1);
	return ImVec4(0.62f, 0.60f, 0.57f, 1);   // gray, and anything unknown
}

// A coloured dot, then the name — one option as it reads in a cell or a menu.
void optionLabel(const SelectOption& o)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float  h = ImGui::GetTextLineHeight();
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.28f,
	                                            Theme::u32(colorOf(o.color)));
	ImGui::Dummy(ImVec2(h * 0.9f, h));
	ImGui::SameLine(0, 0);
	ImGui::TextUnformatted(o.name.c_str());
}

// A menu row for one option: the whole row is the click target, the dot and the
// name are drawn over it.
bool optionRow(const SelectOption& o, bool selected, ImGuiSelectableFlags flags = 0)
{
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::Selectable("##opt", selected, flags);
	ImGui::SetCursorScreenPos(at);
	optionLabel(o);
	return clicked;
}

std::string numberText(double v)
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.10g", v);
	return buf;
}

std::string dateText(const Database& db, const UUID& row, const UUID& prop)
{
	std::int64_t ms = 0;
	if (!db.date(row, prop, ms)) return {};
	return taskTimeToIso(ms).substr(0, 10);   // "2026-10-14"
}

// "2026-10-14" (or a full ISO time) to milliseconds.
bool parseDay(const std::string& text, std::int64_t& ms)
{
	if (text.size() == 10) return taskTimeFromIso(text + "T00:00:00.000Z", ms);
	return taskTimeFromIso(text, ms);
}

std::string joinNames(const std::vector<std::string>& names)
{
	std::string out;
	for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
	return out;
}

std::vector<std::string> splitNames(const std::string& text)
{
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&] {
		const auto b = cur.find_first_not_of(' ');
		const auto e = cur.find_last_not_of(' ');
		if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
		cur.clear();
	};
	for (const char c : text)
	{
		if (c == ',') flush();
		else          cur += c;
	}
	flush();
	return out;
}

// A text field that writes only when it is left. `shown` is what the stored
// value looks like; `commit` gets the new text. Returns true when it committed.
bool textCell(const char* id, const EditKey& key, const std::string& shown,
              const std::function<void(const std::string&)>& commit, const char* hint = nullptr)
{
	const bool mine = s_editing && s_editKey == key;
	std::string text = mine ? s_editBuf : shown;
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool typed = hint ? ImGui::InputTextWithHint(id, hint, &text)
	                        : ImGui::InputText(id, &text);
	if (ImGui::IsItemActivated()) { s_editing = true; s_editKey = key; s_editBuf = shown; }
	if (typed) { s_editing = true; s_editKey = key; s_editBuf = text; }
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string value = s_editBuf;
		s_editing = false;
		commit(value);
		return true;
	}
	if (ImGui::IsItemDeactivated()) s_editing = false;
	return false;
}

// ── Cells ────────────────────────────────────────────────────────────────────

void editCell(const Database& db, const Row& row, const Property& p)
{
	const UUID dbId = db.id, rowId = row.id, propId = p.id;
	auto edit = [dbId](std::function<bool(Database&)> fn) {
		post([dbId, fn = std::move(fn)] { s_ws.edit(dbId, fn); });
	};
	const EditKey key{ rowId, propId };

	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:
		textCell("##cell", key, db.text(rowId, propId), [=](const std::string& v) {
			edit([=](Database& d) { return d.setText(rowId, propId, v, taskNowMs()); });
		});
		break;
	case PropertyType::Number:
		textCell("##cell", key, db.isEmpty(rowId, propId) ? std::string{} : numberText(db.number(rowId, propId)),
		         [=](const std::string& v) {
			edit([=](Database& d) {
				if (v.empty()) return d.clearCell(rowId, propId, taskNowMs());
				char* end = nullptr;
				const double n = std::strtod(v.c_str(), &end);
				if (end == v.c_str() || *end != '\0') return false;   // not a number: keep the old one
				return d.setNumber(rowId, propId, n, taskNowMs());
			});
		});
		break;
	case PropertyType::Checkbox:
	{
		bool on = db.checked(rowId, propId);
		if (EditorWidgets::checkbox("##cell", &on))
			edit([=](Database& d) { return d.setChecked(rowId, propId, on, taskNowMs()); });
		break;
	}
	case PropertyType::Date:
		textCell("##cell", key, dateText(db, rowId, propId), [=](const std::string& v) {
			edit([=](Database& d) {
				if (v.empty()) return d.clearCell(rowId, propId, taskNowMs());
				std::int64_t ms = 0;
				if (!parseDay(v, ms)) return false;   // not a date: keep the old one
				return d.setDate(rowId, propId, ms, taskNowMs());
			});
		}, "YYYY-MM-DD");
		break;
	case PropertyType::Person:
		textCell("##cell", key, joinNames(db.people(rowId, propId)), [=](const std::string& v) {
			edit([=](Database& d) { return d.setPeople(rowId, propId, splitNames(v), taskNowMs()); });
		}, "Names, separated by commas");
		break;
	case PropertyType::Select:
	{
		const SelectOption* cur = p.findOption(db.select(rowId, propId));
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##cell", cur ? cur->name.c_str() : "", ImGuiComboFlags_None))
		{
			if (EditorWidgets::selectable("(none)", cur == nullptr))
				edit([=](Database& d) { return d.setSelect(rowId, propId, UUID{}, taskNowMs()); });
			for (const SelectOption& o : p.options)
			{
				ImGui::PushID(static_cast<int>(&o - p.options.data()));
				if (optionRow(o, cur == &o))
				{
					const UUID oid = o.id;
					edit([=](Database& d) { return d.setSelect(rowId, propId, oid, taskNowMs()); });
				}
				ImGui::PopID();
			}
			ImGui::Separator();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##newoption", "New option", &s_newOption, ImGuiInputTextFlags_EnterReturnsTrue)
			    && !s_newOption.empty())
			{
				const std::string name = s_newOption;
				s_newOption.clear();
				edit([=](Database& d) {
					const SelectOption* o = d.addOption(propId, name);
					return o && d.setSelect(rowId, propId, o->id, taskNowMs());
				});
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PropertyType::MultiSelect:
	{
		const std::vector<UUID> have = db.multiSelect(rowId, propId);
		std::string preview;
		for (const UUID& id : have)
			if (const SelectOption* o = p.findOption(id)) preview += (preview.empty() ? "" : ", ") + o->name;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##cell", preview.c_str(), ImGuiComboFlags_None))
		{
			for (const SelectOption& o : p.options)
			{
				ImGui::PushID(static_cast<int>(&o - p.options.data()));
				const bool on = std::find(have.begin(), have.end(), o.id) != have.end();
				if (optionRow(o, on, ImGuiSelectableFlags_NoAutoClosePopups))
				{
					std::vector<UUID> next = have;
					const UUID oid = o.id;
					if (on) next.erase(std::remove(next.begin(), next.end(), oid), next.end());
					else    next.push_back(oid);
					edit([=](Database& d) { return d.setMultiSelect(rowId, propId, next, taskNowMs()); });
				}
				ImGui::PopID();
			}
			ImGui::Separator();
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##newoption", "New option", &s_newOption, ImGuiInputTextFlags_EnterReturnsTrue)
			    && !s_newOption.empty())
			{
				const std::string name = s_newOption;
				s_newOption.clear();
				std::vector<UUID> next = have;
				edit([=](Database& d) {
					const SelectOption* o = d.addOption(propId, name);
					if (!o) return false;
					std::vector<UUID> all = next;
					all.push_back(o->id);
					return d.setMultiSelect(rowId, propId, all, taskNowMs());
				});
			}
			ImGui::EndCombo();
		}
		break;
	}
	case PropertyType::Relation:
	{
		// Read-only for now: the titles of the rows it points at, where the target is
		// this same database, else just how many.
		const std::vector<UUID> to = db.relation(rowId, propId);
		std::string text;
		for (const UUID& id : to)
		{
			const Row* r = p.relationDb == db.id ? db.findRow(id) : nullptr;
			text += (text.empty() ? "" : ", ") + (r ? r->title : std::string("?"));
		}
		ImGui::TextDisabled("%s", text.c_str());
		break;
	}
	case PropertyType::EngineLink:
	{
		// Read-only for now: what each link was called when it was made.
		std::string text;
		for (const TaskLink& l : db.links(rowId, propId))
			text += (text.empty() ? "" : ", ") + (l.label.empty() ? std::string("(link)") : l.label);
		ImGui::TextDisabled("%s", text.c_str());
		break;
	}
	case PropertyType::Other:
		ImGui::TextDisabled("(%s: not supported by this editor)", p.typeOther.c_str());
		break;
	}
}

// ── Views ────────────────────────────────────────────────────────────────────

// Change one view of the open database; the closure gets the view looked up by id.
void editView(const UUID& dbId, const UUID& viewId, std::function<bool(View&)> fn)
{
	post([=] {
		s_ws.edit(dbId, [&](Database& d) {
			View* v = d.findView(viewId);
			return v && fn(*v);
		});
	});
}

bool isHidden(const View& v, const UUID& prop)
{
	return std::find(v.hidden.begin(), v.hidden.end(), prop) != v.hidden.end();
}

// ── Filters ──────────────────────────────────────────────────────────────────

// What a filter's operand looks like in the file, from what was typed. Empty:
// not a value this property takes (the filter then says nothing and shows all).
std::string operandJson(const Property* p, const std::string& text)
{
	using nlohmann::json;
	if (p)
		switch (p->type)
		{
		case PropertyType::Number:
		{
			char* end = nullptr;
			const double n = std::strtod(text.c_str(), &end);
			return end != text.c_str() && *end == '\0' ? json(n).dump() : std::string{};
		}
		case PropertyType::Checkbox:
			return json(text == "true").dump();
		case PropertyType::Date:
		{
			std::int64_t ms = 0;
			return parseDay(text, ms) ? json(taskTimeToIso(ms)).dump() : std::string{};
		}
		default:
			break;
		}
	return json(text).dump();
}

std::string operandText(const Property* p, const Filter& f)
{
	if (f.valueJson.empty()) return {};
	const nlohmann::json v = nlohmann::json::parse(f.valueJson, nullptr, false);
	if (v.is_string())
	{
		const std::string s = v.get<std::string>();
		return p && p->type == PropertyType::Date && s.size() >= 10 ? s.substr(0, 10) : s;
	}
	if (v.is_number()) return numberText(v.get<double>());
	if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
	return {};
}

bool takesOperand(const std::string& op) { return op != "is-empty" && op != "is-not-empty"; }

void drawFilterPopup(const Database& db, const View& view)
{
	const UUID dbId = db.id, viewId = view.id;
	ImGui::TextDisabled("Show rows where all of these hold");
	for (std::size_t i = 0; i < view.filters.size(); ++i)
	{
		const Filter& f = view.filters[i];
		const Property* prop = f.property == UUID{} ? nullptr : db.findProperty(f.property);
		ImGui::PushID(static_cast<int>(i));

		ImGui::SetNextItemWidth(130);
		if (ImGui::BeginCombo("##fprop", prop ? prop->name.c_str() : "Title"))
		{
			if (ImGui::Selectable("Title", prop == nullptr))
				editView(dbId, viewId, [i](View& v) {
					if (i >= v.filters.size()) return false;
					v.filters[i] = { UUID{}, "contains", "" };
					return true;
				});
			for (const Property& p : db.properties)
				if (ImGui::Selectable(p.name.c_str(), prop == &p))
				{
					const UUID pid = p.id;
					editView(dbId, viewId, [i, pid](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i] = { pid, "equals", "" };
						return true;
					});
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		int opIndex = 0;
		for (int k = 0; k < 10; ++k)
			if (f.op == kFilterOps[k]) opIndex = k;
		ImGui::SetNextItemWidth(130);
		if (ImGui::BeginCombo("##fop", kFilterOpLabels[opIndex]))
		{
			for (int k = 0; k < 10; ++k)
				if (ImGui::Selectable(kFilterOpLabels[k], k == opIndex))
					editView(dbId, viewId, [i, k](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].op = kFilterOps[k];
						return true;
					});
			ImGui::EndCombo();
		}
		if (takesOperand(f.op))
		{
			ImGui::SameLine();
			if (prop && (prop->type == PropertyType::Select || prop->type == PropertyType::MultiSelect))
			{
				ImGui::SetNextItemWidth(150);
				if (ImGui::BeginCombo("##fvalue", operandText(prop, f).c_str()))
				{
					for (const SelectOption& o : prop->options)
						if (ImGui::Selectable(o.name.c_str(), operandText(prop, f) == o.name))
						{
							const std::string name = o.name;
							editView(dbId, viewId, [i, name](View& v) {
								if (i >= v.filters.size()) return false;
								v.filters[i].valueJson = nlohmann::json(name).dump();
								return true;
							});
						}
					ImGui::EndCombo();
				}
			}
			else if (prop && prop->type == PropertyType::Checkbox)
			{
				bool on = operandText(prop, f) == "true";
				if (EditorWidgets::checkbox("##fvalue", &on))
					editView(dbId, viewId, [i, on](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].valueJson = nlohmann::json(on).dump();
						return true;
					});
			}
			else
			{
				ImGui::SetNextItemWidth(150);
				textCell("##fvalue", { viewId, UUID{ 0, i + 1 } }, operandText(prop, f), [=](const std::string& text) {
					const std::string json = operandJson(prop, text);
					editView(dbId, viewId, [i, json](View& v) {
						if (i >= v.filters.size()) return false;
						v.filters[i].valueJson = json;
						return true;
					});
				});
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("x"))
			editView(dbId, viewId, [i](View& v) {
				if (i >= v.filters.size()) return false;
				v.filters.erase(v.filters.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			});
		ImGui::PopID();
	}
	if (EditorWidgets::button("Add Filter"))
		editView(dbId, viewId, [](View& v) { v.filters.push_back({ UUID{}, "contains", "" }); return true; });
}

// ── Properties ───────────────────────────────────────────────────────────────

void drawPropertiesPopup(const Database& db, const View& view)
{
	const UUID dbId = db.id, viewId = view.id;
	ImGui::TextDisabled("Columns of this database. The checkbox shows one in this view.");
	for (const Property& p : db.properties)
	{
		const UUID pid = p.id;
		pushUuid(pid);
		bool shown = !isHidden(view, pid);
		if (EditorWidgets::checkbox("##visible", &shown))
			editView(dbId, viewId, [pid, shown](View& v) {
				auto it = std::find(v.hidden.begin(), v.hidden.end(), pid);
				if (shown && it != v.hidden.end()) v.hidden.erase(it);
				else if (!shown && it == v.hidden.end()) v.hidden.push_back(pid);
				return true;
			});
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		textCell("##pname", { dbId, pid }, p.name, [=](const std::string& name) {
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					Property* prop = d.findProperty(pid);
					if (!prop || name.empty()) return false;
					prop->name = name;
					return true;
				});
			});
		});
		ImGui::SameLine();
		ImGui::TextDisabled("%s", p.type == PropertyType::Other ? p.typeOther.c_str()
		                                                        : kTypeNames[static_cast<int>(p.type)]);
		ImGui::SameLine();
		const bool armed = s_armedDelete == pid;
		if (EditorWidgets::dangerSmallButton(armed ? "Really delete?" : "Delete Property"))
		{
			if (armed)
			{
				s_armedDelete = UUID{};
				post([=] { s_ws.edit(dbId, [&](Database& d) { return d.removeProperty(pid); }); });
			}
			else
				s_armedDelete = pid;
		}
		ImGui::PopID();
	}
	ImGui::Separator();
	ImGui::SetNextItemWidth(150);
	ImGui::InputTextWithHint("##newprop", "New property", &s_newProperty);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120);
	if (ImGui::BeginCombo("##newtype", kTypeNames[s_newPropertyType]))
	{
		for (int i = 0; i < kTypeCount; ++i)
			if (ImGui::Selectable(kTypeNames[i], i == s_newPropertyType)) s_newPropertyType = i;
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (EditorWidgets::button("Add Property") && !s_newProperty.empty())
	{
		const std::string name = s_newProperty;
		const PropertyType type = static_cast<PropertyType>(s_newPropertyType);
		s_newProperty.clear();
		post([=] { s_ws.edit(dbId, [&](Database& d) { return d.addProperty(name, type) != nullptr; }); });
	}
}

// ── The table ────────────────────────────────────────────────────────────────

void cycleSort(const UUID& dbId, const UUID& viewId, const UUID& prop)
{
	editView(dbId, viewId, [prop](View& v) {
		if (v.sorts.empty() || !(v.sorts[0].property == prop)) v.sorts = { { prop, false } };
		else if (!v.sorts[0].descending)                       v.sorts[0].descending = true;
		else                                                   v.sorts.clear();
		return true;
	});
}

void deleteRowMenu(const Database& db, const Row& row)
{
	if (ImGui::BeginPopupContextItem("##rowmenu"))
	{
		const UUID dbId = db.id, rowId = row.id;
		if (EditorWidgets::dangerMenuItem("Delete Row"))
			post([=] { s_ws.edit(dbId, [&](Database& d) { return d.removeRow(rowId); }); });
		ImGui::EndPopup();
	}
}

void drawTable(const Database& db, const View& view, const std::vector<std::size_t>& idx)
{
	std::vector<const Property*> cols;
	for (const Property& p : db.properties)
		if (!isHidden(view, p.id)) cols.push_back(&p);

	const float rowH = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
	                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("##grid", 1 + static_cast<int>(cols.size()), flags)) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthFixed, 280.0f);
	for (const Property* p : cols) ImGui::TableSetupColumn(p->name.c_str(), ImGuiTableColumnFlags_WidthFixed, 170.0f);

	// The header is drawn by hand: a click on a name sorts by it.
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int c = 0; c <= static_cast<int>(cols.size()); ++c)
	{
		ImGui::TableSetColumnIndex(c);
		ImGui::PushID(c);
		const bool isTitle = c == 0;
		const UUID prop = isTitle ? UUID{} : cols[static_cast<std::size_t>(c - 1)]->id;
		const std::string& name = isTitle ? std::string("Title") : cols[static_cast<std::size_t>(c - 1)]->name;
		const bool sorted = !view.sorts.empty() && view.sorts[0].property == prop;
		if (ImGui::Selectable(isTitle ? "Title" : name.c_str(), false, 0, ImVec2(0, 0)))
			cycleSort(db.id, view.id, prop);
		if (isTitle) note("header:Title"); else note("header:" + name);
		if (sorted)
		{
			const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			const float h = (b.y - a.y) * 0.22f, cx = b.x - h * 2.0f, cy = (a.y + b.y) * 0.5f;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImU32 col = Theme::u32(Theme::AccentHi);
			if (view.sorts[0].descending) dl->AddTriangleFilled({ cx - h, cy - h * 0.6f }, { cx + h, cy - h * 0.6f }, { cx, cy + h * 0.8f }, col);
			else                          dl->AddTriangleFilled({ cx - h, cy + h * 0.6f }, { cx + h, cy + h * 0.6f }, { cx, cy - h * 0.8f }, col);
		}
		ImGui::PopID();
	}

	// Only the rows on screen are laid out; every row is one frame high.
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(idx.size()), rowH);
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
		{
			const Row& row = db.rows[idx[static_cast<std::size_t>(i)]];
			ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
			++s_stats.rowsDrawn;
			pushUuid(row.id);

			ImGui::TableSetColumnIndex(0);
			const UUID dbId = db.id, rowId = row.id;
			textCell("##title", { rowId, UUID{} }, row.title, [=](const std::string& title) {
				post([=] { s_ws.edit(dbId, [&](Database& d) { return d.setTitle(rowId, title, taskNowMs()); }); });
			}, "Untitled");
			note("title:" + row.title);
			deleteRowMenu(db, row);

			for (std::size_t c = 0; c < cols.size(); ++c)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
				pushUuid(cols[c]->id);
				editCell(db, row, *cols[c]);
				if (i == clipper.DisplayStart) note("cell:" + cols[c]->name);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	ImGui::EndTable();
}

// ── The board ────────────────────────────────────────────────────────────────

std::string summary(const Database& db, const Row& row, const Property& p)
{
	switch (p.type)
	{
	case PropertyType::Text:
	case PropertyType::Url:         return db.text(row.id, p.id);
	case PropertyType::Number:      return db.isEmpty(row.id, p.id) ? std::string{} : numberText(db.number(row.id, p.id));
	case PropertyType::Checkbox:    return db.checked(row.id, p.id) ? "yes" : "";
	case PropertyType::Date:        return dateText(db, row.id, p.id);
	case PropertyType::Person:      return joinNames(db.people(row.id, p.id));
	case PropertyType::Select:
	{
		const SelectOption* o = p.findOption(db.select(row.id, p.id));
		return o ? o->name : std::string{};
	}
	case PropertyType::MultiSelect:
	{
		std::string out;
		for (const UUID& id : db.multiSelect(row.id, p.id))
			if (const SelectOption* o = p.findOption(id)) out += (out.empty() ? "" : ", ") + o->name;
		return out;
	}
	default:
		return {};
	}
}

void dropOnLane(const UUID& dbId, const UUID& groupProp, const UUID& option)
{
	if (!ImGui::BeginDragDropTarget()) return;
	if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HE_PLANNER_ROW"))
		if (pl->DataSize == static_cast<int>(sizeof(UUID)))
		{
			const UUID rowId = *static_cast<const UUID*>(pl->Data);
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					const UUID cur = d.select(rowId, groupProp);
					if (cur == option) return false;
					return d.setSelect(rowId, groupProp, option, taskNowMs());
				});
			});
		}
	ImGui::EndDragDropTarget();
}

void drawBoard(const Database& db, const View& view)
{
	const Property* group = db.findProperty(view.groupBy);
	if (!group || group->type != PropertyType::Select)
	{
		ImGui::TextDisabled("Pick a Select property to make the lanes from.");
		ImGui::SetNextItemWidth(200);
		if (ImGui::BeginCombo("##groupby", "Group by"))
		{
			for (const Property& p : db.properties)
				if (p.type == PropertyType::Select && ImGui::Selectable(p.name.c_str()))
				{
					const UUID pid = p.id;
					editView(db.id, view.id, [pid](View& v) { v.groupBy = pid; return true; });
				}
			ImGui::EndCombo();
		}
		return;
	}

	const UUID dbId = db.id, groupId = group->id;
	// Lanes: "No value" first, then the options in the order the property lists them.
	std::vector<UUID> lanes{ UUID{} };
	for (const SelectOption& o : group->options) lanes.push_back(o.id);

	const float cardH = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 4.0f;
	if (!ImGui::BeginTable("##lanes", static_cast<int>(lanes.size()),
	                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
	                       ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	for (const UUID& lane : lanes)
	{
		const SelectOption* o = group->findOption(lane);
		ImGui::TableSetupColumn(o ? o->name.c_str() : "No value", ImGuiTableColumnFlags_WidthFixed, 200.0f);
	}
	ImGui::TableHeadersRow();
	ImGui::TableNextRow();
	for (std::size_t c = 0; c < lanes.size(); ++c)
	{
		ImGui::TableSetColumnIndex(static_cast<int>(c));
		ImGui::PushID(static_cast<int>(c));
		const UUID lane = lanes[c];
		const std::vector<std::size_t> inLane = db.column(view, lane);

		if (ImGui::SmallButton("+"))
			post([=] {
				s_ws.edit(dbId, [&](Database& d) {
					const Row* r = d.addRow("", s_ws.author(), taskNowMs());
					return r && (lane == UUID{} || d.setSelect(r->id, groupId, lane, taskNowMs()));
				});
			});
		ImGui::SameLine();
		ImGui::TextDisabled("%d", static_cast<int>(inLane.size()));
		dropOnLane(dbId, groupId, lane);

		for (const std::size_t ri : inLane)
		{
			const Row& row = db.rows[ri];
			pushUuid(row.id);
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const float  w  = ImGui::GetContentRegionAvail().x;
			ImDrawList*  dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + cardH), Theme::u32(Theme::warm(0.17f)), 4.0f);
			ImGui::Selectable("##card", false, 0, ImVec2(w, cardH));
			note("card:" + row.title);
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload("HE_PLANNER_ROW", &row.id, sizeof(UUID));
				ImGui::TextUnformatted(row.title.empty() ? "Untitled" : row.title.c_str());
				ImGui::EndDragDropSource();
			}
			dropOnLane(dbId, groupId, lane);
			deleteRowMenu(db, row);

			// Title, then the first two things worth saying about the row.
			const float pad = ImGui::GetStyle().FramePadding.x + 2.0f;
			dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + cardH), true);
			dl->AddText(ImVec2(p0.x + pad, p0.y + ImGui::GetStyle().FramePadding.y + 1.0f),
			            Theme::u32(row.title.empty() ? Theme::TextDim : Theme::Text),
			            row.title.empty() ? "Untitled" : row.title.c_str());
			std::string line;
			int shown = 0;
			for (const Property& p : db.properties)
			{
				if (p.id == groupId || isHidden(view, p.id) || shown >= 2) continue;
				const std::string s = summary(db, row, p);
				if (s.empty()) continue;
				line += (line.empty() ? "" : "  ") + s;
				++shown;
			}
			dl->AddText(ImVec2(p0.x + pad, p0.y + cardH * 0.5f + 2.0f), Theme::u32(Theme::TextDim), line.c_str());
			dl->PopClipRect();
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, 4));
		}
		// Room to drop a card into an empty lane.
		ImGui::InvisibleButton("##drop", ImVec2(-FLT_MIN, std::max(60.0f, ImGui::GetContentRegionAvail().y)));
		{
			const SelectOption* laneOption = group->findOption(lane);
			note("lane:" + (laneOption ? laneOption->name : std::string("No value")));
		}
		dropOnLane(dbId, groupId, lane);
		ImGui::PopID();
	}
	ImGui::EndTable();
}

// ── One database ─────────────────────────────────────────────────────────────

void drawDatabase(AppContext& ctx, const Database& db)
{
	const UUID dbId = db.id;
	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	ImGui::SetNextItemWidth(360);
	textCell("##dbname", { dbId, UUID{} }, db.name, [=](const std::string& name) {
		post([=] {
			s_ws.edit(dbId, [&](Database& d) {
				if (name.empty() || name == d.name) return false;
				d.name = name;
				return true;
			});
		});
	}, "Untitled database");
	if (ctx.fontHeading) ImGui::PopFont();
	ImGui::SameLine();
	ImGui::TextDisabled("%d rows", static_cast<int>(db.rows.size()));

	// Views as tabs.
	const View* view = db.findView(s_viewId);
	if (!view && !db.views.empty()) { s_viewId = db.views.front().id; view = &db.views.front(); }
	for (const View& v : db.views)
	{
		pushUuid(v.id);
		if (ImGui::Selectable(v.name.c_str(), v.id == s_viewId, 0,
		                      ImVec2(ImGui::CalcTextSize(v.name.c_str()).x + 24.0f, 0)))
			s_viewId = v.id;
		note("view:" + v.name);
		if (ImGui::BeginPopupContextItem("##viewmenu"))
		{
			const UUID vid = v.id;
			ImGui::TextDisabled("Rename");
			textCell("##viewname", { vid, UUID{ 0, 9 } }, v.name, [=](const std::string& name) {
				editView(dbId, vid, [name](View& vv) {
					if (name.empty()) return false;
					vv.name = name;
					return true;
				});
			});
			if (EditorWidgets::dangerMenuItem("Delete View", db.views.size() > 1))
				post([=] {
					s_ws.edit(dbId, [&](Database& d) { return d.removeView(vid); });
					if (s_viewId == vid) s_viewId = UUID{};
				});
			ImGui::EndPopup();
		}
		ImGui::PopID();
		ImGui::SameLine();
	}
	if (EditorWidgets::button("+ Table"))
		post([=] {
			s_ws.edit(dbId, [&](Database& d) { s_viewId = d.addView("Table", ViewKind::Table)->id; return true; });
		});
	note("+ Table");
	ImGui::SameLine();
	if (EditorWidgets::button("+ Board"))
		post([=] {
			s_ws.edit(dbId, [&](Database& d) {
				View* v = d.addView("Board", ViewKind::Board);
				s_viewId = v->id;
				for (const Property& p : d.properties)
					if (p.type == PropertyType::Select) { v->groupBy = p.id; break; }
				return true;
			});
		});
	note("+ Board");

	if (!view) return;
	const UUID viewId = view->id;

	// Toolbar.
	if (EditorWidgets::primaryButton("New Row"))
		post([=] {
			s_ws.edit(dbId, [&](Database& d) { return d.addRow("", s_ws.author(), taskNowMs()) != nullptr; });
		});
	note("New Row");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(180);
	ImGui::InputTextWithHint("##search", "Search titles", &s_search);
	ImGui::SameLine();
	const std::string filterLabel = view->filters.empty() ? std::string("Filter")
	                                : "Filter (" + std::to_string(view->filters.size()) + ")";
	if (ImGui::Button(filterLabel.c_str())) ImGui::OpenPopup("##filters");
	note("Filter");
	if (ImGui::BeginPopup("##filters")) { drawFilterPopup(db, *view); ImGui::EndPopup(); }
	ImGui::SameLine();
	if (EditorWidgets::button("Properties")) ImGui::OpenPopup("##properties");
	note("Properties");
	if (ImGui::BeginPopup("##properties")) { drawPropertiesPopup(db, *view); ImGui::EndPopup(); }
	else s_armedDelete = UUID{};
	if (view->kind == ViewKind::Board)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		const Property* g = db.findProperty(view->groupBy);
		if (ImGui::BeginCombo("##boardgroup", g ? g->name.c_str() : "Group by"))
		{
			for (const Property& p : db.properties)
				if (p.type == PropertyType::Select && ImGui::Selectable(p.name.c_str(), g == &p))
				{
					const UUID pid = p.id;
					editView(dbId, viewId, [pid](View& v) { v.groupBy = pid; return true; });
				}
			ImGui::EndCombo();
		}
	}
	if (!s_notice.empty())
	{
		ImGui::TextColored(Theme::Ok, "%s", s_notice.c_str());
	}

	// The rows this view shows: its own filters and sorts, plus the search box.
	View effective = *view;
	if (!s_search.empty())
		effective.filters.push_back({ UUID{}, "contains", nlohmann::json(s_search).dump() });
	const std::vector<std::size_t> idx = db.query(effective);
	s_stats.rows = static_cast<int>(idx.size());

	switch (view->kind)
	{
	case ViewKind::Table: drawTable(db, effective, idx); break;
	case ViewKind::Board: drawBoard(db, effective);      break;
	default:
		ImGui::TextDisabled("This view type is not available in this editor yet.");
		break;
	}
}

// ── The sidebar ──────────────────────────────────────────────────────────────

void importTasks(AppContext& ctx)
{
	TaskBoard* board = TaskBoardPanel::board(ctx);
	if (!board)
	{
		s_notice = "No project is open.";
		return;
	}
	if (!s_ws.ensure(tasksDatabaseId(), "Tasks", [](Database& db) { ensureTasksSchema(db); }))
		return;   // the workspace says why (lastError)
	ImportResult r;
	s_ws.edit(tasksDatabaseId(), [&](Database& db) {
		r = importTaskBoard(*board, db);
		return r.added + r.updated > 0;
	});
	s_dbId   = tasksDatabaseId();
	s_viewId = UUID{};
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Imported %d new and updated %d of %d tasks from the Task Board.",
	              r.added, r.updated, static_cast<int>(board->tasks.size()));
	s_notice = buf;
}

void drawSidebar(AppContext& ctx)
{
	if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
	ImGui::TextUnformatted("Planner");
	if (ctx.fontSubheading) ImGui::PopFont();
	ImGui::Spacing();

	for (Database* db : s_ws.databases())
	{
		pushUuid(db->id);
		const std::string label = (db->name.empty() ? std::string("Untitled database") : db->name) + "##db";
		if (ImGui::Selectable(label.c_str(), db->id == s_dbId))
		{
			s_dbId   = db->id;
			s_viewId = UUID{};
		}
		note("database:" + db->name);
		ImGui::PopID();
	}
	if (s_ws.databases().empty()) ImGui::TextDisabled("No databases yet.");
	ImGui::Spacing();

	if (EditorWidgets::button("New Database", ImVec2(-FLT_MIN, 0)))
		post([] {
			if (Database* db = s_ws.create("New Database"))
			{
				s_dbId   = db->id;
				s_viewId = UUID{};
			}
		});
	note("New Database");
	if (EditorWidgets::button("Import Task Board", ImVec2(-FLT_MIN, 0))) post([&ctx] { importTasks(ctx); });
	note("Import Task Board");
	if (EditorWidgets::button("Reload", ImVec2(-FLT_MIN, 0))) post([] { s_ws.poll(true); });

	for (const Workspace::Unreadable& u : s_ws.unreadable())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
		ImGui::Text("%s.json cannot be read: %s", uuidToHex(u.id).substr(0, 8).c_str(), u.error.c_str());
		ImGui::PopStyleColor();
	}
}

} // namespace

// ── The tab ──────────────────────────────────────────────────────────────────

void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	Workspace* ws = workspace(ctx);
	if (ws)
	{
		s_ws.setAuthor(TaskBoardPanel::author());
		s_ws.poll();
	}
	s_stats = {};
	s_rects.clear();
	HE::Ed::Help::Scope helpScope("Planner");

	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##PlannerTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoSavedSettings);

	if (!ws)
	{
		ImGui::TextDisabled("Open a project to plan in it.");
		ImGui::End();
		return;
	}

	// The selection may point at a database that is gone (a pull removed it).
	if (!s_ws.find(s_dbId))
	{
		const std::vector<Database*> all = s_ws.databases();
		s_dbId   = all.empty() ? UUID{} : all.front()->id;
		s_viewId = UUID{};
	}

	ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.075f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
	ImGui::BeginChild("##plannernav", ImVec2(230.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	drawSidebar(ctx);
	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::SameLine();
	ImGui::BeginChild("##plannermain", ImVec2(0, 0), ImGuiChildFlags_None);
	if (!s_ws.lastError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		EditorWidgets::WrapText wrap(ImGui::GetContentRegionAvail().x);
		ImGui::TextUnformatted(s_ws.lastError().c_str());
		ImGui::PopStyleColor();
	}
	if (const Database* db = s_ws.find(s_dbId))
		drawDatabase(ctx, *db);
	else
	{
		ImGui::Spacing();
		ImGui::TextDisabled("Make a database with New Database, or copy the Task Board's tasks into one with Import Task Board.");
	}
	ImGui::EndChild();
	ImGui::End();

	// Edits asked for while drawing, now that nothing holds a reference into a database.
	for (int guard = 0; guard < 8 && !s_posted.empty(); ++guard)
	{
		std::vector<std::function<void()>> now;
		now.swap(s_posted);
		for (auto& fn : now) fn();
	}
	s_posted.clear();
}

Workspace* workspace(AppContext& ctx)
{
	if (!ctx.projectLoaded || !ctx.projectManager)
	{
		s_ws.setProject({});
		return nullptr;
	}
	const std::string root = ctx.projectManager->projectRoot();
	if (root.empty())
	{
		s_ws.setProject({});
		return nullptr;
	}
	s_ws.setProject(root);
	return &s_ws;
}

void selectDatabase(const UUID& id) { s_dbId = id; s_viewId = UUID{}; }
UUID selectedDatabase() { return s_dbId; }
UUID selectedView() { return s_viewId; }
DrawStats lastDrawStats() { return s_stats; }
const std::string& lastError() { return s_ws.lastError(); }
const std::string& notice() { return s_notice; }

Rect controlRect(const std::string& name)
{
	const auto it = s_rects.find(name);
	return it == s_rects.end() ? Rect{} : it->second;
}

void reset()
{
	s_ws = Workspace{};
	s_dbId = s_viewId = UUID{};
	s_search.clear();
	s_notice.clear();
	s_editing = false;
	s_editBuf.clear();
	s_posted.clear();
	s_rects.clear();
	s_stats = {};
	s_armedDelete = UUID{};
}

} // namespace PlannerPanel
