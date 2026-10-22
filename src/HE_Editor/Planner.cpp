#include "Planner.h"
#include "TaskBoardJson.h"

#include <Diagnostics/Log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_set>

namespace HE::Planner
{

namespace TJ = HE::TaskJson;
using ojson = TJ::ojson;

namespace
{

// ── Words ────────────────────────────────────────────────────────────────────

template <class E, std::size_t N>
bool fromName(std::string_view name, const char* const (&names)[N], E& out)
{
	for (std::size_t i = 0; i < N; ++i)
		if (name == names[i]) { out = static_cast<E>(i); return true; }
	return false;
}

template <class E, std::size_t N>
const char* toName(E e, const char* const (&names)[N])
{
	const auto i = static_cast<std::size_t>(e);
	return i < N ? names[i] : "";
}

const char* const kPropertyNames[] = { "text", "number", "checkbox", "date", "select", "multi-select",
                                       "person", "relation", "engine-link", "url" };
const char* const kViewNames[]     = { "table", "board", "timeline", "calendar" };
const char* const kBlockNames[]    = { "paragraph", "heading-1", "heading-2", "heading-3", "bullet",
                                       "numbered", "todo", "quote", "callout", "divider", "code",
                                       "engine-link", "database-view" };

bool isSelectType(PropertyType t) { return t == PropertyType::Select || t == PropertyType::MultiSelect; }

std::string lower(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool isNull(const UUID& id) { return id == UUID{}; }

// ── JSON in and out ──────────────────────────────────────────────────────────

void writeUuidIf(ojson& j, const char* key, const UUID& id)
{
	if (!isNull(id)) j[key] = uuidToHex(id);
}

// A list entry's id: the file's, or a fresh one when it has none or repeats an
// earlier one (a copy-pasted entry must not shadow, or be shadowed by, the original).
void settleId(UUID& id, std::unordered_set<UUID>& seen)
{
	if (isNull(id) || !seen.insert(id).second)
	{
		id = UUID::generate();
		seen.insert(id);
	}
}

ojson parseOrNull(const std::string& text)
{
	ojson j = ojson::parse(text, nullptr, /*allow_exceptions=*/false);
	return j.is_discarded() ? ojson(nullptr) : j;
}

SelectOption readOption(const ojson& j)
{
	SelectOption o;
	TJ::readUuid(j, "id", o.id);
	TJ::readString(j, "name", o.name);
	TJ::readString(j, "color", o.color);
	o.extraJson = TJ::extrasOf(j, { "id", "name", "color" });
	return o;
}

ojson writeOption(const SelectOption& o)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(o.id);
	j["name"] = o.name;
	if (!o.color.empty()) j["color"] = o.color;
	TJ::appendExtras(j, o.extraJson);
	return j;
}

Property readProperty(const ojson& j, int& dropped)
{
	Property p;
	TJ::readUuid(j, "id", p.id);
	TJ::readString(j, "name", p.name);
	std::string word;
	TJ::readString(j, "type", word);
	if (!propertyTypeFromName(word, p.type))
	{
		p.type      = PropertyType::Other;
		p.typeOther = word;
	}
	if (const auto it = j.find("options"); it != j.end() && it->is_array())
	{
		std::unordered_set<UUID> seen;
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++dropped; continue; }
			SelectOption o = readOption(e);
			settleId(o.id, seen);
			p.options.push_back(std::move(o));
		}
	}
	TJ::readUuid(j, "relation", p.relationDb);
	p.extraJson = TJ::extrasOf(j, { "id", "name", "type", "options", "relation" });
	return p;
}

ojson writeProperty(const Property& p)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(p.id);
	j["name"] = p.name;
	j["type"] = p.type == PropertyType::Other ? p.typeOther : propertyTypeName(p.type);
	if (isSelectType(p.type) || !p.options.empty())
	{
		ojson list = ojson::array();
		for (const SelectOption& o : p.options) list.push_back(writeOption(o));
		j["options"] = std::move(list);
	}
	writeUuidIf(j, "relation", p.relationDb);
	TJ::appendExtras(j, p.extraJson);
	return j;
}

Row readRow(const ojson& j, int& dropped)
{
	Row r;
	TJ::readUuid(j, "id", r.id);
	TJ::readString(j, "title", r.title);
	TJ::readString(j, "icon", r.icon);
	TJ::readString(j, "createdBy", r.createdBy);
	TJ::readTime(j, "created", r.createdMs);
	TJ::readTime(j, "modified", r.modifiedMs);
	if (const auto it = j.find("cells"); it != j.end() && it->is_object())
		for (auto c = it->begin(); c != it->end(); ++c)
		{
			Cell cell;
			if (!uuidFromHex(c.key(), cell.property)) { ++dropped; continue; }
			cell.json = c.value().dump();
			r.cells.push_back(std::move(cell));
		}
	r.extraJson = TJ::extrasOf(j, { "id", "title", "icon", "createdBy", "created", "modified", "cells" });
	return r;
}

ojson writeRow(const Row& r)
{
	ojson j = ojson::object();
	j["id"]    = uuidToHex(r.id);
	j["title"] = r.title;
	if (!r.icon.empty()) j["icon"] = r.icon;
	j["createdBy"] = r.createdBy;
	j["created"]   = taskTimeToIso(r.createdMs);
	j["modified"]  = taskTimeToIso(r.modifiedMs);
	ojson cells = ojson::object();
	for (const Cell& c : r.cells) cells[uuidToHex(c.property)] = parseOrNull(c.json);
	j["cells"] = std::move(cells);
	TJ::appendExtras(j, r.extraJson);
	return j;
}

View readView(const ojson& j, int& dropped)
{
	View v;
	TJ::readUuid(j, "id", v.id);
	TJ::readString(j, "name", v.name);
	std::string word;
	TJ::readString(j, "kind", word);
	if (!viewKindFromName(word, v.kind))
	{
		v.kind      = ViewKind::Other;
		v.kindOther = word;
	}
	if (const auto it = j.find("filters"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++dropped; continue; }
			Filter f;
			TJ::readUuid(e, "property", f.property);
			TJ::readString(e, "op", f.op);
			if (const auto val = e.find("value"); val != e.end()) f.valueJson = val->dump();
			v.filters.push_back(std::move(f));
		}
	if (const auto it = j.find("sorts"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++dropped; continue; }
			Sort s;
			TJ::readUuid(e, "property", s.property);
			if (const auto d = e.find("descending"); d != e.end() && d->is_boolean()) s.descending = d->get<bool>();
			v.sorts.push_back(s);
		}
	TJ::readUuid(j, "groupBy", v.groupBy);
	TJ::readUuid(j, "dateProperty", v.dateProperty);
	TJ::readUuid(j, "endProperty", v.endProperty);
	if (const auto it = j.find("hidden"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			UUID id;
			if (e.is_string() && uuidFromHex(e.get<std::string>(), id)) v.hidden.push_back(id);
		}
	v.extraJson = TJ::extrasOf(j, { "id", "name", "kind", "filters", "sorts", "groupBy",
	                                "dateProperty", "endProperty", "hidden" });
	return v;
}

ojson writeView(const View& v)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(v.id);
	j["name"] = v.name;
	j["kind"] = v.kind == ViewKind::Other ? v.kindOther : viewKindName(v.kind);
	ojson filters = ojson::array();
	for (const Filter& f : v.filters)
	{
		ojson e = ojson::object();
		writeUuidIf(e, "property", f.property);
		e["op"] = f.op;
		if (!f.valueJson.empty()) e["value"] = parseOrNull(f.valueJson);
		filters.push_back(std::move(e));
	}
	j["filters"] = std::move(filters);
	ojson sorts = ojson::array();
	for (const Sort& s : v.sorts)
	{
		ojson e = ojson::object();
		writeUuidIf(e, "property", s.property);
		e["descending"] = s.descending;
		sorts.push_back(std::move(e));
	}
	j["sorts"] = std::move(sorts);
	writeUuidIf(j, "groupBy", v.groupBy);
	writeUuidIf(j, "dateProperty", v.dateProperty);
	writeUuidIf(j, "endProperty", v.endProperty);
	ojson hidden = ojson::array();
	for (const UUID& id : v.hidden) hidden.push_back(uuidToHex(id));
	j["hidden"] = std::move(hidden);
	TJ::appendExtras(j, v.extraJson);
	return j;
}

Block readBlock(const ojson& j, int& dropped, std::unordered_set<UUID>& seen)
{
	Block b;
	TJ::readUuid(j, "id", b.id);
	settleId(b.id, seen);
	std::string word;
	TJ::readString(j, "type", word);
	if (!blockTypeFromName(word, b.type))
	{
		b.type      = BlockType::Other;
		b.typeOther = word;
	}
	TJ::readString(j, "text", b.text);
	if (const auto it = j.find("checked"); it != j.end() && it->is_boolean()) b.checked = it->get<bool>();
	TJ::readString(j, "language", b.language);
	if (const auto it = j.find("link"); it != j.end() && it->is_object()) b.link = TJ::readLink(*it);
	TJ::readUuid(j, "database", b.database);
	TJ::readUuid(j, "view", b.view);
	if (const auto it = j.find("children"); it != j.end() && it->is_array())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++dropped; continue; }
			b.children.push_back(readBlock(e, dropped, seen));
		}
	b.extraJson = TJ::extrasOf(j, { "id", "type", "text", "checked", "language", "link",
	                                "database", "view", "children" });
	return b;
}

ojson writeBlock(const Block& b)
{
	ojson j = ojson::object();
	j["id"]   = uuidToHex(b.id);
	j["type"] = b.type == BlockType::Other ? b.typeOther : blockTypeName(b.type);
	if (!b.text.empty()) j["text"] = b.text;
	if (b.type == BlockType::Todo) j["checked"] = b.checked;
	if (b.type == BlockType::Code && !b.language.empty()) j["language"] = b.language;
	if (b.type == BlockType::EngineLink) j["link"] = TJ::writeLink(b.link);
	if (b.type == BlockType::DatabaseView)
	{
		writeUuidIf(j, "database", b.database);
		writeUuidIf(j, "view", b.view);
	}
	if (!b.children.empty())
	{
		ojson kids = ojson::array();
		for (const Block& c : b.children) kids.push_back(writeBlock(c));
		j["children"] = std::move(kids);
	}
	TJ::appendExtras(j, b.extraJson);
	return j;
}

// The top of a document: parse, check the shape and the version. Shared by the
// database and the page; false with r.error set when the file is not usable.
bool openDocument(std::string_view text, const char* listKey, ojson& out, int& version,
                  Database::LoadResult& r)
{
	out = ojson::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
	if (out.is_discarded())
	{
		r.error = text.find("<<<<<<<") != std::string_view::npos
		        ? "the file has unresolved merge-conflict markers"
		        : "the file is not valid JSON";
		return false;
	}
	if (!out.is_object())
	{
		r.error = "the file is not a JSON object";
		return false;
	}
	version = 1;
	if (const auto it = out.find("version"); it != out.end())
	{
		if (!it->is_number_integer() || it->get<std::int64_t>() < 1 ||
		    it->get<std::int64_t>() > 1000000)
		{
			r.error = "\"version\" is not a positive whole number";
			return false;
		}
		version = static_cast<int>(it->get<std::int64_t>());
	}
	if (listKey)
		if (const auto it = out.find(listKey); it != out.end() && !it->is_array())
		{
			r.error = std::string("\"") + listKey + "\" is not a list";
			return false;
		}
	return true;
}

// ── Files ────────────────────────────────────────────────────────────────────

bool readFile(const std::filesystem::path& path, std::string& out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in.is_open()) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return !in.bad();
}

bool isHexName(const std::string& s)
{
	UUID id;
	return uuidFromHex(s, id);
}

std::vector<UUID> listDir(const std::filesystem::path& dir)
{
	std::vector<UUID> out;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		const std::filesystem::path& p = it->path();
		if (p.extension() != ".json" || !isHexName(p.stem().string())) continue;
		UUID id;
		uuidFromHex(p.stem().string(), id);
		out.push_back(id);
	}
	std::sort(out.begin(), out.end(), [](const UUID& a, const UUID& b) {
		return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
	});
	return out;
}

using ParseFn = std::function<Database::LoadResult(std::string_view)>;

Database::LoadResult loadFile(const std::filesystem::path& path, const char* what,
                              const ParseFn& parse, const std::function<void()>& reset,
                              bool& hadFile, std::string& bytes)
{
	Database::LoadResult r;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
	{
		reset();   // remembers "no file"
		r.ok      = true;
		r.version = 1;
		return r;
	}
	std::string text;
	if (!readFile(path, text))
	{
		reset();
		r.error = "cannot read " + path.string();
		HE_LOG_ERROR(Editor, "Planner %s: %s", what, r.error.c_str());
		return r;
	}
	r = parse(text);
	hadFile = true;
	bytes   = std::move(text);
	if (!r.ok)
		HE_LOG_WARN(Editor, "Planner %s: '%s' not loaded — %s. It is left untouched.",
		            what, path.string().c_str(), r.error.c_str());
	else if (r.dropped > 0)
		HE_LOG_WARN(Editor, "Planner %s: '%s' — %d malformed entr%s skipped",
		            what, path.string().c_str(), r.dropped, r.dropped == 1 ? "y" : "ies");
	return r;
}

// The TaskBoard's save rules (see saveTaskBoard): temp + rename, never over a file
// that does not parse, never over one that changed since we last saw it.
bool saveFile(const std::filesystem::path& path, const char* what, const std::string& text,
              bool nothingToSay, const ParseFn& parse, bool& hadFile, std::string& bytes,
              std::string* error)
{
	auto fail = [&](std::string why) {
		if (error) *error = why;
		return false;
	};

	std::error_code ec;
	const bool exists = std::filesystem::exists(path, ec);
	if (!exists && nothingToSay && !hadFile) return true;

	std::string onDisk;
	if (exists && !readFile(path, onDisk))
		return fail("cannot read " + path.string());

	if (exists && onDisk == text)
	{
		hadFile = true;
		bytes   = text;
		return true;   // nothing to write: leave the file and its mtime alone
	}
	if (exists)
	{
		const Database::LoadResult r = parse(onDisk);
		if (!r.ok)
		{
			HE_LOG_WARN(Editor, "Planner %s: not saving over '%s' — %s", what,
			            path.string().c_str(), r.error.c_str());
			return fail(r.error + " — fix or remove " + path.string() + " first");
		}
	}
	if (exists != hadFile || (exists && onDisk != bytes))
	{
		HE_LOG_WARN(Editor, "Planner %s: '%s' changed on disk since it was loaded — not saved",
		            what, path.string().c_str());
		return fail(path.string() + " changed on disk since it was loaded — reload it first");
	}

	std::filesystem::create_directories(path.parent_path(), ec);
	const std::filesystem::path tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			return fail("cannot create " + tmp.string());
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.flush();
		if (!out.good())
		{
			out.close();
			std::filesystem::remove(tmp, ec);
			return fail("write to " + tmp.string() + " failed — the previous file is intact");
		}
	}
	std::filesystem::rename(tmp, path, ec);
	if (ec)
	{
		std::error_code ec2;
		std::filesystem::remove(tmp, ec2);
		HE_LOG_ERROR(Editor, "Planner %s: could not replace '%s' (%s)", what,
		             path.string().c_str(), ec.message().c_str());
		return fail("could not replace " + path.string() + " (" + ec.message() + ")");
	}
	hadFile = true;
	bytes   = text;
	return true;
}

bool deleteFile(const std::filesystem::path& path, std::string* error)
{
	std::error_code ec;
	std::filesystem::remove(path, ec);
	if (ec)
	{
		if (error) *error = "could not delete " + path.string() + " (" + ec.message() + ")";
		return false;
	}
	return true;
}

// ── Cells: what a value means, for filtering and sorting ─────────────────────

bool jsonIsEmpty(const ojson& v)
{
	if (v.is_null()) return true;
	if (v.is_string()) return v.get<std::string>().empty();
	if (v.is_array() || v.is_object()) return v.empty();
	if (v.is_boolean()) return !v.get<bool>();
	return false;
}

// The operand of a date filter: an ISO string or a number of milliseconds.
bool jsonToMs(const ojson& v, std::int64_t& out)
{
	if (v.is_string()) return taskTimeFromIso(v.get<std::string>(), out);
	if (v.is_number_integer()) { out = v.get<std::int64_t>(); return true; }
	return false;
}

// The value a select operand names: an option id, or (for a hand-written view)
// the option's name.
std::string optionKey(const Property& p, const ojson& operand)
{
	if (!operand.is_string()) return {};
	const std::string s = operand.get<std::string>();
	UUID id;
	if (uuidFromHex(s, id)) return uuidToHex(id);
	if (const SelectOption* o = p.findOptionByName(s)) return uuidToHex(o->id);
	return s;
}

bool arrayHas(const ojson& arr, const std::string& s)
{
	if (!arr.is_array()) return false;
	for (const ojson& e : arr)
		if (e.is_string() && e.get<std::string>() == s) return true;
	return false;
}

struct SortKey
{
	bool        empty = true;
	double      num = 0;
	std::string str;
};

SortKey sortKeyOf(const Property* p, const ojson& v, const std::string& title)
{
	SortKey k;
	if (!p)   // the title
	{
		k.empty = title.empty();
		k.str   = lower(title);
		return k;
	}
	if (jsonIsEmpty(v)) return k;
	k.empty = false;
	switch (p->type)
	{
	case PropertyType::Text:
	case PropertyType::Url:
		k.str = lower(v.is_string() ? v.get<std::string>() : std::string{});
		break;
	case PropertyType::Number:
		k.num = v.is_number() ? v.get<double>() : 0;
		break;
	case PropertyType::Checkbox:
		k.num = v.is_boolean() && v.get<bool>() ? 1 : 0;
		break;
	case PropertyType::Date:
	{
		std::int64_t ms = 0;
		if (!jsonToMs(v, ms)) { k.empty = true; break; }
		k.num = static_cast<double>(ms);
		break;
	}
	case PropertyType::Select:
	{
		// By the option's place in the list, so a sort follows the order the
		// team put "Open, In progress, Blocked, Done" in, not the alphabet.
		const std::string key = v.is_string() ? v.get<std::string>() : std::string{};
		k.empty = true;
		for (std::size_t i = 0; i < p->options.size(); ++i)
			if (uuidToHex(p->options[i].id) == key) { k.empty = false; k.num = static_cast<double>(i); break; }
		break;
	}
	case PropertyType::MultiSelect:
	case PropertyType::Person:
	case PropertyType::Relation:
		k.str = v.is_array() && !v.empty() && v[0].is_string() ? lower(v[0].get<std::string>()) : std::string{};
		k.empty = k.str.empty();
		break;
	default:
		k.empty = true;   // engine links and unknown types do not order anything
		break;
	}
	return k;
}

} // namespace

// ── Words, public ────────────────────────────────────────────────────────────

const char* propertyTypeName(PropertyType t) { return toName(t, kPropertyNames); }
const char* viewKindName(ViewKind k)         { return toName(k, kViewNames); }
const char* blockTypeName(BlockType t)       { return toName(t, kBlockNames); }
bool propertyTypeFromName(std::string_view n, PropertyType& out) { return fromName(n, kPropertyNames, out); }
bool viewKindFromName(std::string_view n, ViewKind& out)         { return fromName(n, kViewNames, out); }
bool blockTypeFromName(std::string_view n, BlockType& out)       { return fromName(n, kBlockNames, out); }

// ── Property / Row lookups ───────────────────────────────────────────────────

const SelectOption* Property::findOption(const UUID& optionId) const
{
	for (const SelectOption& o : options)
		if (o.id == optionId) return &o;
	return nullptr;
}

const SelectOption* Property::findOptionByName(std::string_view n) const
{
	for (const SelectOption& o : options)
		if (o.name == n) return &o;
	return nullptr;
}

const Cell* Row::cell(const UUID& property) const
{
	for (const Cell& c : cells)
		if (c.property == property) return &c;
	return nullptr;
}

// ── Database: schema ─────────────────────────────────────────────────────────

Database Database::create(std::string name)
{
	Database db;
	db.id   = UUID::generate();
	db.name = std::move(name);
	db.addView("Table", ViewKind::Table);
	return db;
}

Property* Database::addProperty(std::string n, PropertyType type)
{
	Property p;
	p.id   = UUID::generate();
	p.name = std::move(n);
	p.type = type;
	properties.push_back(std::move(p));
	return &properties.back();
}

Property* Database::findProperty(const UUID& pid)
{
	for (Property& p : properties)
		if (p.id == pid) return &p;
	return nullptr;
}

const Property* Database::findProperty(const UUID& pid) const
{
	return const_cast<Database*>(this)->findProperty(pid);
}

const Property* Database::findPropertyByName(std::string_view n) const
{
	for (const Property& p : properties)
		if (p.name == n) return &p;
	return nullptr;
}

bool Database::removeProperty(const UUID& pid)
{
	const auto it = std::find_if(properties.begin(), properties.end(),
	                             [&](const Property& p) { return p.id == pid; });
	if (it == properties.end()) return false;
	properties.erase(it);
	for (Row& r : rows)
		r.cells.erase(std::remove_if(r.cells.begin(), r.cells.end(),
		                             [&](const Cell& c) { return c.property == pid; }), r.cells.end());
	for (View& v : views)
	{
		v.filters.erase(std::remove_if(v.filters.begin(), v.filters.end(),
		                               [&](const Filter& f) { return f.property == pid; }), v.filters.end());
		v.sorts.erase(std::remove_if(v.sorts.begin(), v.sorts.end(),
		                             [&](const Sort& s) { return s.property == pid; }), v.sorts.end());
		v.hidden.erase(std::remove(v.hidden.begin(), v.hidden.end(), pid), v.hidden.end());
		if (v.groupBy == pid)      v.groupBy = UUID{};
		if (v.dateProperty == pid) v.dateProperty = UUID{};
		if (v.endProperty == pid)  v.endProperty = UUID{};
	}
	return true;
}

SelectOption* Database::addOption(const UUID& pid, std::string n, std::string color)
{
	Property* p = findProperty(pid);
	if (!p || !isSelectType(p->type)) return nullptr;
	for (SelectOption& o : p->options)
		if (o.name == n) return &o;
	SelectOption o;
	o.id    = UUID::generate();
	o.name  = std::move(n);
	o.color = std::move(color);
	p->options.push_back(std::move(o));
	return &p->options.back();
}

bool Database::removeOption(const UUID& pid, const UUID& option)
{
	Property* p = findProperty(pid);
	if (!p || !isSelectType(p->type)) return false;
	const auto it = std::find_if(p->options.begin(), p->options.end(),
	                             [&](const SelectOption& o) { return o.id == option; });
	if (it == p->options.end()) return false;
	p->options.erase(it);
	const std::string hex = uuidToHex(option);
	for (Row& r : rows)
		for (std::size_t i = 0; i < r.cells.size();)
		{
			Cell& c = r.cells[i];
			bool drop = false;
			if (c.property == pid)
			{
				ojson v = parseOrNull(c.json);
				if (v.is_string()) drop = v.get<std::string>() == hex;
				else if (v.is_array())
				{
					ojson kept = ojson::array();
					for (const ojson& e : v)
						if (!(e.is_string() && e.get<std::string>() == hex)) kept.push_back(e);
					drop = kept.empty();
					if (!drop) c.json = kept.dump();
				}
			}
			if (drop) r.cells.erase(r.cells.begin() + static_cast<std::ptrdiff_t>(i));
			else      ++i;
		}
	return true;
}

// ── Database: rows ───────────────────────────────────────────────────────────

Row* Database::addRow(std::string title, std::string author, std::int64_t nowMs)
{
	Row r;
	r.id         = UUID::generate();
	r.title      = std::move(title);
	r.createdBy  = std::move(author);
	r.createdMs  = nowMs;
	r.modifiedMs = nowMs;
	rows.push_back(std::move(r));
	return &rows.back();
}

Row* Database::findRow(const UUID& rid)
{
	for (Row& r : rows)
		if (r.id == rid) return &r;
	return nullptr;
}

const Row* Database::findRow(const UUID& rid) const
{
	return const_cast<Database*>(this)->findRow(rid);
}

bool Database::removeRow(const UUID& rid)
{
	const auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.id == rid; });
	if (it == rows.end()) return false;
	rows.erase(it);
	// A row of this database that pointed at it (a self-relation, "blocked by") loses the pointer.
	const std::string hex = uuidToHex(rid);
	for (const Property& p : properties)
	{
		if (p.type != PropertyType::Relation || p.relationDb != id) continue;
		for (Row& r : rows)
			for (std::size_t i = 0; i < r.cells.size();)
			{
				Cell& c = r.cells[i];
				bool drop = false;
				if (c.property == p.id)
				{
					ojson v = parseOrNull(c.json);
					ojson kept = ojson::array();
					if (v.is_array())
						for (const ojson& e : v)
							if (!(e.is_string() && e.get<std::string>() == hex)) kept.push_back(e);
					drop = kept.empty();
					if (!drop) c.json = kept.dump();
				}
				if (drop) r.cells.erase(r.cells.begin() + static_cast<std::ptrdiff_t>(i));
				else      ++i;
			}
	}
	return true;
}

bool Database::setTitle(const UUID& rid, std::string t, std::int64_t nowMs)
{
	Row* r = findRow(rid);
	if (!r) return false;
	r->title      = std::move(t);
	r->modifiedMs = nowMs;
	return true;
}

// ── Database: cells ──────────────────────────────────────────────────────────

const Cell* Database::cellFor(const UUID& rid, const UUID& pid) const
{
	const Row* r = findRow(rid);
	return r ? r->cell(pid) : nullptr;
}

bool Database::put(const UUID& rid, const UUID& pid, std::string json, std::int64_t nowMs,
                   std::initializer_list<PropertyType> allowed)
{
	Row* r = findRow(rid);
	const Property* p = findProperty(pid);
	if (!r || !p) return false;
	if (std::find(allowed.begin(), allowed.end(), p->type) == allowed.end()) return false;
	r->modifiedMs = nowMs;
	for (std::size_t i = 0; i < r->cells.size(); ++i)
		if (r->cells[i].property == pid)
		{
			if (json.empty()) r->cells.erase(r->cells.begin() + static_cast<std::ptrdiff_t>(i));
			else              r->cells[i].json = std::move(json);
			return true;
		}
	if (!json.empty()) r->cells.push_back({ pid, std::move(json) });
	return true;
}

bool Database::setText(const UUID& r, const UUID& p, std::string v, std::int64_t now)
{
	return put(r, p, v.empty() ? std::string{} : ojson(v).dump(), now, { PropertyType::Text, PropertyType::Url });
}

bool Database::setNumber(const UUID& r, const UUID& p, double v, std::int64_t now)
{
	if (!std::isfinite(v)) return false;
	return put(r, p, ojson(v).dump(), now, { PropertyType::Number });
}

bool Database::setChecked(const UUID& r, const UUID& p, bool v, std::int64_t now)
{
	return put(r, p, v ? "true" : "", now, { PropertyType::Checkbox });   // unchecked = no cell
}

bool Database::setDate(const UUID& r, const UUID& p, std::int64_t ms, std::int64_t now)
{
	return put(r, p, ojson(taskTimeToIso(ms)).dump(), now, { PropertyType::Date });
}

bool Database::setSelect(const UUID& r, const UUID& p, const UUID& option, std::int64_t now)
{
	const Property* prop = findProperty(p);
	if (!prop || prop->type != PropertyType::Select) return false;
	if (isNull(option)) return put(r, p, {}, now, { PropertyType::Select });
	if (!prop->findOption(option)) return false;
	return put(r, p, ojson(uuidToHex(option)).dump(), now, { PropertyType::Select });
}

bool Database::setMultiSelect(const UUID& r, const UUID& p, std::vector<UUID> options, std::int64_t now)
{
	const Property* prop = findProperty(p);
	if (!prop || prop->type != PropertyType::MultiSelect) return false;
	ojson arr = ojson::array();
	std::unordered_set<UUID> seen;
	for (const UUID& o : options)
	{
		if (!prop->findOption(o)) return false;
		if (seen.insert(o).second) arr.push_back(uuidToHex(o));
	}
	return put(r, p, arr.empty() ? std::string{} : arr.dump(), now, { PropertyType::MultiSelect });
}

bool Database::setPeople(const UUID& r, const UUID& p, std::vector<std::string> people, std::int64_t now)
{
	ojson arr = ojson::array();
	for (const std::string& n : people)
		if (!n.empty() && !arrayHas(arr, n)) arr.push_back(n);
	return put(r, p, arr.empty() ? std::string{} : arr.dump(), now, { PropertyType::Person });
}

bool Database::setRelation(const UUID& r, const UUID& p, std::vector<UUID> targets, std::int64_t now)
{
	ojson arr = ojson::array();
	std::unordered_set<UUID> seen;
	for (const UUID& t : targets)
		if (!isNull(t) && seen.insert(t).second) arr.push_back(uuidToHex(t));
	return put(r, p, arr.empty() ? std::string{} : arr.dump(), now, { PropertyType::Relation });
}

bool Database::setLinks(const UUID& r, const UUID& p, const std::vector<TaskLink>& links, std::int64_t now)
{
	ojson arr = ojson::array();
	for (const TaskLink& l : links) arr.push_back(TJ::writeLink(l));
	return put(r, p, arr.empty() ? std::string{} : arr.dump(), now, { PropertyType::EngineLink });
}

bool Database::clearCell(const UUID& r, const UUID& p, std::int64_t now)
{
	const Property* prop = findProperty(p);
	if (!prop) return false;
	return put(r, p, {}, now, { prop->type });
}

std::string Database::text(const UUID& r, const UUID& p) const
{
	const Cell* c = cellFor(r, p);
	if (!c) return {};
	const ojson v = parseOrNull(c->json);
	return v.is_string() ? v.get<std::string>() : std::string{};
}

double Database::number(const UUID& r, const UUID& p) const
{
	const Cell* c = cellFor(r, p);
	if (!c) return 0;
	const ojson v = parseOrNull(c->json);
	return v.is_number() ? v.get<double>() : 0;
}

bool Database::checked(const UUID& r, const UUID& p) const
{
	const Cell* c = cellFor(r, p);
	if (!c) return false;
	const ojson v = parseOrNull(c->json);
	return v.is_boolean() && v.get<bool>();
}

bool Database::date(const UUID& r, const UUID& p, std::int64_t& outMs) const
{
	const Cell* c = cellFor(r, p);
	return c && jsonToMs(parseOrNull(c->json), outMs);
}

UUID Database::select(const UUID& r, const UUID& p) const
{
	UUID id;
	if (const Cell* c = cellFor(r, p))
	{
		const ojson v = parseOrNull(c->json);
		if (v.is_string()) uuidFromHex(v.get<std::string>(), id);
	}
	return id;
}

static std::vector<UUID> readUuidArray(const Cell* c)
{
	std::vector<UUID> out;
	if (!c) return out;
	const ojson v = parseOrNull(c->json);
	if (!v.is_array()) return out;
	for (const ojson& e : v)
	{
		UUID id;
		if (e.is_string() && uuidFromHex(e.get<std::string>(), id)) out.push_back(id);
	}
	return out;
}

std::vector<UUID> Database::multiSelect(const UUID& r, const UUID& p) const { return readUuidArray(cellFor(r, p)); }
std::vector<UUID> Database::relation(const UUID& r, const UUID& p) const    { return readUuidArray(cellFor(r, p)); }

std::vector<std::string> Database::people(const UUID& r, const UUID& p) const
{
	std::vector<std::string> out;
	if (const Cell* c = cellFor(r, p))
	{
		const ojson v = parseOrNull(c->json);
		if (v.is_array())
			for (const ojson& e : v)
				if (e.is_string()) out.push_back(e.get<std::string>());
	}
	return out;
}

std::vector<TaskLink> Database::links(const UUID& r, const UUID& p) const
{
	std::vector<TaskLink> out;
	if (const Cell* c = cellFor(r, p))
	{
		const ojson v = parseOrNull(c->json);
		if (v.is_array())
			for (const ojson& e : v)
				if (e.is_object()) out.push_back(TJ::readLink(e));
	}
	return out;
}

bool Database::isEmpty(const UUID& r, const UUID& p) const
{
	const Cell* c = cellFor(r, p);
	return !c || jsonIsEmpty(parseOrNull(c->json));
}

// ── Database: views ──────────────────────────────────────────────────────────

View* Database::addView(std::string n, ViewKind kind)
{
	View v;
	v.id   = UUID::generate();
	v.name = std::move(n);
	v.kind = kind;
	views.push_back(std::move(v));
	return &views.back();
}

bool Database::removeView(const UUID& vid)
{
	if (views.size() <= 1) return false;
	const auto it = std::find_if(views.begin(), views.end(), [&](const View& v) { return v.id == vid; });
	if (it == views.end()) return false;
	views.erase(it);
	return true;
}

View* Database::findView(const UUID& vid)
{
	for (View& v : views)
		if (v.id == vid) return &v;
	return nullptr;
}

const View* Database::findView(const UUID& vid) const
{
	return const_cast<Database*>(this)->findView(vid);
}

std::vector<std::size_t> Database::query(const View& view) const
{
	// The cell of a row for a filter or sort: the title for the null property.
	auto valueOf = [&](const Row& r, const UUID& pid) -> ojson {
		if (isNull(pid)) return r.title;
		const Cell* c = r.cell(pid);
		return c ? parseOrNull(c->json) : ojson(nullptr);
	};

	auto matches = [&](const Row& r, const Filter& f) -> bool {
		const Property* p = isNull(f.property) ? nullptr : findProperty(f.property);
		if (!isNull(f.property) && !p) return true;   // a property that is gone filters nothing
		const PropertyType type = p ? p->type : PropertyType::Text;
		const ojson v = valueOf(r, f.property);
		const ojson operand = f.valueJson.empty() ? ojson(nullptr) : parseOrNull(f.valueJson);

		if (f.op == "is-empty")     return jsonIsEmpty(v);
		if (f.op == "is-not-empty") return !jsonIsEmpty(v);

		const bool negate = f.op == "not-equals" || f.op == "not-contains";
		const bool isEq   = f.op == "equals" || f.op == "not-equals";
		const bool isHas  = f.op == "contains" || f.op == "not-contains";
		if (isEq || isHas)
		{
			bool result = false;
			switch (type)
			{
			case PropertyType::Text:
			case PropertyType::Url:
			{
				const std::string have = v.is_string() ? v.get<std::string>() : std::string{};
				const std::string want = operand.is_string() ? operand.get<std::string>() : std::string{};
				result = isEq ? have == want : lower(have).find(lower(want)) != std::string::npos;
				break;
			}
			case PropertyType::Number:
				if (!isEq || !operand.is_number()) return true;
				result = v.is_number() && v.get<double>() == operand.get<double>();
				break;
			case PropertyType::Checkbox:
				if (!isEq || !operand.is_boolean()) return true;
				result = (v.is_boolean() && v.get<bool>()) == operand.get<bool>();
				break;
			case PropertyType::Date:
			{
				std::int64_t a = 0, b = 0;
				if (!isEq || !jsonToMs(operand, b)) return true;
				result = jsonToMs(v, a) && a == b;
				break;
			}
			case PropertyType::Select:
				if (!p) return true;
				result = v.is_string() && v.get<std::string>() == optionKey(*p, operand);
				break;
			case PropertyType::MultiSelect:
				if (!p) return true;
				result = arrayHas(v, optionKey(*p, operand));
				if (isEq) result = result && v.is_array() && v.size() == 1;
				break;
			case PropertyType::Person:
			case PropertyType::Relation:
				if (!operand.is_string()) return true;
				result = arrayHas(v, operand.get<std::string>());
				if (isEq) result = result && v.is_array() && v.size() == 1;
				break;
			default:
				return true;   // cannot evaluate: do not hide the row
			}
			return negate ? !result : result;
		}

		const bool gt = f.op == "gt", gte = f.op == "gte", lt = f.op == "lt", lte = f.op == "lte";
		if (gt || gte || lt || lte)
		{
			double a = 0, b = 0;
			if (type == PropertyType::Number)
			{
				if (!operand.is_number()) return true;
				if (!v.is_number()) return false;
				a = v.get<double>();
				b = operand.get<double>();
			}
			else if (type == PropertyType::Date)
			{
				std::int64_t x = 0, y = 0;
				if (!jsonToMs(operand, y)) return true;
				if (!jsonToMs(v, x)) return false;
				a = static_cast<double>(x);
				b = static_cast<double>(y);
			}
			else
				return true;
			return gt ? a > b : gte ? a >= b : lt ? a < b : a <= b;
		}
		return true;   // an op this build does not know
	};

	std::vector<std::size_t> idx;
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		bool ok = true;
		for (const Filter& f : view.filters)
			if (!matches(rows[i], f)) { ok = false; break; }
		if (ok) idx.push_back(i);
	}

	if (view.sorts.empty()) return idx;

	// Keys once per row and sort, not a JSON parse per comparison.
	std::vector<std::vector<SortKey>> keys(view.sorts.size(), std::vector<SortKey>(rows.size()));
	for (std::size_t s = 0; s < view.sorts.size(); ++s)
	{
		const Property* p = isNull(view.sorts[s].property) ? nullptr : findProperty(view.sorts[s].property);
		if (!isNull(view.sorts[s].property) && !p) continue;   // gone: every key stays empty
		for (const std::size_t i : idx)
			keys[s][i] = sortKeyOf(p, valueOf(rows[i], view.sorts[s].property), rows[i].title);
	}
	std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
		for (std::size_t s = 0; s < view.sorts.size(); ++s)
		{
			const SortKey& x = keys[s][a];
			const SortKey& y = keys[s][b];
			if (x.empty != y.empty) return y.empty;   // empty last, in either direction
			if (x.empty) continue;
			int c = x.num < y.num ? -1 : x.num > y.num ? 1 : x.str.compare(y.str);
			if (c == 0) continue;
			return view.sorts[s].descending ? c > 0 : c < 0;
		}
		return false;
	});
	return idx;
}

std::vector<std::size_t> Database::column(const View& view, const UUID& option) const
{
	std::vector<std::size_t> out;
	const Property* p = findProperty(view.groupBy);
	for (const std::size_t i : query(view))
	{
		UUID have = select(rows[i].id, view.groupBy);
		if (!p || !p->findOption(have)) have = UUID{};   // an option that is gone is "No value"
		if (have == option) out.push_back(i);
	}
	return out;
}

// ── Database: the document ───────────────────────────────────────────────────

Database::LoadResult Database::fromJson(std::string_view text)
{
	*this = Database{};
	LoadResult r;
	ojson j;
	int version = 1;
	if (!openDocument(text, nullptr, j, version, r)) return r;
	for (const char* key : { "properties", "views", "rows" })
		if (const auto it = j.find(key); it != j.end() && !it->is_array())
		{
			// Not a list we could add to without losing what is there.
			r.error = std::string("\"") + key + "\" is not a list";
			return r;
		}

	TJ::readUuid(j, "id", id);
	TJ::readString(j, "name", name);
	TJ::readString(j, "icon", icon);

	std::unordered_set<UUID> seen;
	if (const auto it = j.find("properties"); it != j.end())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++r.dropped; continue; }
			Property p = readProperty(e, r.dropped);
			settleId(p.id, seen);
			properties.push_back(std::move(p));
		}
	seen.clear();
	if (const auto it = j.find("views"); it != j.end())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { ++r.dropped; continue; }
			View v = readView(e, r.dropped);
			settleId(v.id, seen);
			views.push_back(std::move(v));
		}
	seen.clear();
	if (const auto it = j.find("rows"); it != j.end())
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { m_unreadableRows.push_back(e.dump()); continue; }
			Row row = readRow(e, r.dropped);
			settleId(row.id, seen);
			rows.push_back(std::move(row));
		}

	m_fileVersion = version;
	m_extraJson   = TJ::extrasOf(j, { "version", "id", "name", "icon", "properties", "views", "rows" });
	r.ok      = true;
	r.version = version;
	return r;
}

std::string Database::toJson() const
{
	ojson j = ojson::object();
	j["version"] = writeVersion();
	j["id"]      = uuidToHex(id);
	j["name"]    = name;
	if (!icon.empty()) j["icon"] = icon;
	ojson props = ojson::array();
	for (const Property& p : properties) props.push_back(writeProperty(p));
	j["properties"] = std::move(props);
	ojson vs = ojson::array();
	for (const View& v : views) vs.push_back(writeView(v));
	j["views"] = std::move(vs);
	ojson rs = ojson::array();
	if (persistRows)
	{
		for (const Row& r : rows) rs.push_back(writeRow(r));
		for (const std::string& raw : m_unreadableRows)
		{
			ojson e = ojson::parse(raw, nullptr, /*allow_exceptions=*/false);
			if (!e.is_discarded()) rs.push_back(std::move(e));
		}
	}
	j["rows"] = std::move(rs);
	TJ::appendExtras(j, m_extraJson);
	return TJ::dumpDocument(j);
}

// ── Page ─────────────────────────────────────────────────────────────────────

namespace
{

bool locate(std::vector<Block>& list, const UUID& id, std::vector<Block>*& owner, std::size_t& index)
{
	for (std::size_t i = 0; i < list.size(); ++i)
	{
		if (list[i].id == id) { owner = &list; index = i; return true; }
		if (locate(list[i].children, id, owner, index)) return true;
	}
	return false;
}

Block* findIn(std::vector<Block>& list, const UUID& id)
{
	for (Block& b : list)
	{
		if (b.id == id) return &b;
		if (Block* c = findIn(b.children, id)) return c;
	}
	return nullptr;
}

} // namespace

Block* Page::findBlock(const UUID& bid) { return findIn(blocks, bid); }

const Block* Page::findBlock(const UUID& bid) const
{
	return findIn(const_cast<std::vector<Block>&>(blocks), bid);
}

Block* Page::addBlock(BlockType type, std::string text, std::int64_t nowMs, const UUID& parentBlock)
{
	std::vector<Block>* list = &blocks;
	if (!isNull(parentBlock))
	{
		Block* p = findBlock(parentBlock);
		if (!p) return nullptr;
		list = &p->children;
	}
	Block b;
	b.id   = UUID::generate();
	b.type = type;
	b.text = std::move(text);
	list->push_back(std::move(b));
	modifiedMs = nowMs;
	return &list->back();
}

bool Page::removeBlock(const UUID& bid, std::int64_t nowMs)
{
	std::vector<Block>* owner = nullptr;
	std::size_t index = 0;
	if (!locate(blocks, bid, owner, index)) return false;
	owner->erase(owner->begin() + static_cast<std::ptrdiff_t>(index));
	modifiedMs = nowMs;
	return true;
}

bool Page::moveBlock(const UUID& bid, const UUID& newParent, std::size_t index, std::int64_t nowMs)
{
	std::vector<Block>* owner = nullptr;
	std::size_t at = 0;
	if (!locate(blocks, bid, owner, at)) return false;
	if (!isNull(newParent))
	{
		if (!findBlock(newParent)) return false;
		// Into itself or something under it would cut the block off the tree.
		if (newParent == bid || findIn((*owner)[at].children, newParent)) return false;
	}
	Block moved = std::move((*owner)[at]);
	owner->erase(owner->begin() + static_cast<std::ptrdiff_t>(at));
	std::vector<Block>& dest = isNull(newParent) ? blocks : findBlock(newParent)->children;
	dest.insert(dest.begin() + static_cast<std::ptrdiff_t>(std::min(index, dest.size())), std::move(moved));
	modifiedMs = nowMs;
	return true;
}

Page::LoadResult Page::fromJson(std::string_view text)
{
	*this = Page{};
	LoadResult r;
	ojson j;
	int version = 1;
	if (!openDocument(text, "blocks", j, version, r)) return r;

	TJ::readUuid(j, "id", id);
	TJ::readString(j, "title", title);
	TJ::readString(j, "icon", icon);
	TJ::readUuid(j, "parent", parent);
	TJ::readString(j, "createdBy", createdBy);
	TJ::readTime(j, "created", createdMs);
	TJ::readTime(j, "modified", modifiedMs);
	if (const auto it = j.find("blocks"); it != j.end())
	{
		std::unordered_set<UUID> seen;
		for (const ojson& e : *it)
		{
			if (!e.is_object()) { m_unreadableBlocks.push_back(e.dump()); continue; }
			blocks.push_back(readBlock(e, r.dropped, seen));
		}
	}
	m_fileVersion = version;
	m_extraJson   = TJ::extrasOf(j, { "version", "id", "title", "icon", "parent", "createdBy",
	                                  "created", "modified", "blocks" });
	r.ok      = true;
	r.version = version;
	return r;
}

std::string Page::toJson() const
{
	ojson j = ojson::object();
	j["version"] = writeVersion();
	j["id"]      = uuidToHex(id);
	j["title"]   = title;
	if (!icon.empty()) j["icon"] = icon;
	writeUuidIf(j, "parent", parent);
	j["createdBy"] = createdBy;
	j["created"]   = taskTimeToIso(createdMs);
	j["modified"]  = taskTimeToIso(modifiedMs);
	ojson list = ojson::array();
	for (const Block& b : blocks) list.push_back(writeBlock(b));
	for (const std::string& raw : m_unreadableBlocks)
	{
		ojson e = ojson::parse(raw, nullptr, /*allow_exceptions=*/false);
		if (!e.is_discarded()) list.push_back(std::move(e));
	}
	j["blocks"] = std::move(list);
	TJ::appendExtras(j, m_extraJson);
	return TJ::dumpDocument(j);
}

// ── The files ────────────────────────────────────────────────────────────────

std::filesystem::path plannerDir(const std::filesystem::path& projectRoot)
{
	return projectRoot / "Config" / "Planner";
}

std::filesystem::path databasePath(const std::filesystem::path& projectRoot, const UUID& id)
{
	return plannerDir(projectRoot) / "Databases" / (uuidToHex(id) + ".json");
}

std::filesystem::path pagePath(const std::filesystem::path& projectRoot, const UUID& id)
{
	return plannerDir(projectRoot) / "Pages" / (uuidToHex(id) + ".json");
}

std::vector<UUID> listDatabases(const std::filesystem::path& projectRoot)
{
	return listDir(plannerDir(projectRoot) / "Databases");
}

std::vector<UUID> listPages(const std::filesystem::path& projectRoot)
{
	return listDir(plannerDir(projectRoot) / "Pages");
}

Database::LoadResult loadDatabase(const std::filesystem::path& projectRoot, const UUID& id, Database& out)
{
	const auto reset = [&] { out = Database{}; out.id = id; };
	Database::LoadResult r = loadFile(
		databasePath(projectRoot, id), "database",
		[&](std::string_view t) { return out.fromJson(t); }, reset,
		out.m_diskHadFile, out.m_diskBytes);
	out.id = id;   // the file name is the identity, whatever the file says
	return r;
}

Page::LoadResult loadPage(const std::filesystem::path& projectRoot, const UUID& id, Page& out)
{
	const auto reset = [&] { out = Page{}; out.id = id; };
	Page::LoadResult r = loadFile(
		pagePath(projectRoot, id), "page",
		[&](std::string_view t) { return out.fromJson(t); }, reset,
		out.m_diskHadFile, out.m_diskBytes);
	out.id = id;
	return r;
}

bool saveDatabase(const std::filesystem::path& projectRoot, Database& db, std::string* error)
{
	if (isNull(db.id))
	{
		if (error) *error = "the database has no id";
		return false;
	}
	return saveFile(databasePath(projectRoot, db.id), "database", db.toJson(),
	                /*nothingToSay=*/false,
	                [](std::string_view t) { Database probe; return probe.fromJson(t); },
	                db.m_diskHadFile, db.m_diskBytes, error);
}

bool savePage(const std::filesystem::path& projectRoot, Page& page, std::string* error)
{
	if (isNull(page.id))
	{
		if (error) *error = "the page has no id";
		return false;
	}
	return saveFile(pagePath(projectRoot, page.id), "page", page.toJson(),
	                /*nothingToSay=*/page.blocks.empty() && page.title.empty() && page.m_unreadableBlocks.empty(),
	                [](std::string_view t) { Page probe; return probe.fromJson(t); },
	                page.m_diskHadFile, page.m_diskBytes, error);
}

bool deleteDatabase(const std::filesystem::path& projectRoot, const UUID& id, std::string* error)
{
	return deleteFile(databasePath(projectRoot, id), error);
}

bool deletePage(const std::filesystem::path& projectRoot, const UUID& id, std::string* error)
{
	return deleteFile(pagePath(projectRoot, id), error);
}

} // namespace HE::Planner
