#include "HcExtract.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace HcExtract
{
namespace
{
	using HorizonCode::ContainerKind;
	using HorizonCode::ExtractFailure;
	using HorizonCode::ExtractMapEntry;
	using HorizonCode::PinType;
	using HorizonCode::Variable;

	std::string typeWord(PinType t, ContainerKind k, const std::string& typeName)
	{
		static const char* kNames[] = { "Exec", "Float", "Bool", "Int", "String", "Vec2", "Color",
		                                "Object", "Transform", "Enum", "Struct", "Vec3", "Vec4",
		                                "Double" };
		const size_t i = (size_t)t;
		std::string w = i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
		if ((t == PinType::Enum || t == PinType::Struct) && !typeName.empty())
			w += " " + structLabel(typeName);
		switch (k)
		{
			case ContainerKind::Array: return w + "[]";
			case ContainerKind::Set:   return "Set of " + w;
			case ContainerKind::Map:   return "Map of " + w;
			default:                   return w;
		}
	}

	// What an unmapped member carries, as the table's grey "default …".
	std::string defaultWord(const HE::StructField& f)
	{
		if (f.kind() != ContainerKind::None) return "default (empty)";
		const HorizonCode::Value v = HE::TypeRegistry::instance().makeFieldDefault(f);
		char buf[64] = {};
		switch (f.type)
		{
			case PinType::Float:  std::snprintf(buf, sizeof(buf), "%g", (double)v.f); break;
			case PinType::Double: std::snprintf(buf, sizeof(buf), "%g", v.d); break;
			case PinType::Int:    std::snprintf(buf, sizeof(buf), "%d", v.i); break;
			case PinType::Bool:   return std::string("default ") + (v.b ? "true" : "false");
			case PinType::String: return "default \"" + v.s + "\"";
			case PinType::Enum:   return "default " + (f.defaultValue.s.empty() ? std::string("(first)")
			                                                                : f.defaultValue.s);
			case PinType::Ref:    return "default (none)";
			default:              return "default";
		}
		return std::string("default ") + buf;
	}

	ExtractMapEntry* entryFor(HorizonCode::ExtractSpec& s, const std::string& member)
	{
		for (ExtractMapEntry& m : s.map)
			if (m.member == member) return &m;
		return nullptr;
	}

	const ClassVar* findVar(const std::vector<ClassVar>& vars, const std::string& name)
	{
		for (const ClassVar& cv : vars)
			if (cv.decl.name == name) return &cv;
		return nullptr;
	}

	std::string lower(std::string s)
	{
		for (char& c : s) c = (char)std::tolower((unsigned char)c);
		return s;
	}
}

std::vector<ClassVar> classVariables(const HorizonCode::Graph& g)
{
	std::vector<ClassVar> out;
	for (const Variable& v : g.variables)
		if (v.scope == 0) out.push_back({ v, false });
	// Graph::inherited is nearest-first and never shadowed by this class's own
	// (the panel refuses a name a base already uses), but stay safe.
	for (const Variable& v : g.inherited)
		if (v.scope == 0 && !findVar(out, v.name)) out.push_back({ v, true });
	return out;
}

std::string fitOf(const HE::StructField& f, const Variable& v)
{
	std::string why;
	HorizonCode::pullShapesCompatible(v.type, v.kind(), v.keyType, v.typeName,
	                                  f.type, f.kind(), f.keyType, f.typeName, &why);
	return why;
}

std::string selfFit(const HE::StructField& f)
{
	if (f.type == PinType::Ref && f.kind() == ContainerKind::None) return {};
	return "Self is an object reference, " + typeText(f) + " is not";
}

std::string typeText(const HE::StructField& f) { return typeWord(f.type, f.kind(), f.typeName); }

std::string structLabel(const std::string& path)
{
	std::string stem = path;
	if (const size_t s = stem.find_last_of("/\\"); s != std::string::npos) stem.erase(0, s + 1);
	if (const size_t d = stem.rfind('.'); d != std::string::npos) stem.erase(d);
	return stem;
}

std::vector<Row> rows(const HorizonCode::Graph& g, const HE::StructDef& def)
{
	std::vector<Row> out;
	const std::vector<ClassVar> vars = classVariables(g);
	std::vector<bool> used(g.extract.map.size(), false);
	for (size_t i = 0; i < def.fields.size(); ++i)
	{
		const HE::StructField& f = def.fields[i];
		Row r;
		r.member = f.name;
		r.type = typeText(f);
		r.defaultText = defaultWord(f);
		r.field = (int)i;
		// The entry for this field: under its live name, or a former one (a
		// struct renamed since the class was last saved).
		for (size_t e = 0; e < g.extract.map.size(); ++e)
		{
			const ExtractMapEntry& m = g.extract.map[e];
			if (used[e]) continue;
			if (def.findField(m.member) != &f) continue;
			used[e] = true;
			r.var = m.var;
			ExtractFailure why = ExtractFailure::None;
			std::string detail;
			if (m.var == HorizonCode::kExtractSelf)
			{
				if (!selfFit(f).empty()) why = ExtractFailure::SelfNotRef;
			}
			else if (const ClassVar* cv = findVar(vars, m.var); !cv)
				why = ExtractFailure::NoSuchVariable;
			else if (detail = fitOf(f, cv->decl); !detail.empty())
				why = ExtractFailure::TypeMismatch;
			if (why != ExtractFailure::None)
				r.error = HorizonCode::extractFailureText(why, f.name, m.var, detail);
			break;
		}
		out.push_back(std::move(r));
	}
	for (size_t e = 0; e < g.extract.map.size(); ++e)
	{
		if (used[e]) continue;
		const ExtractMapEntry& m = g.extract.map[e];
		Row r;
		r.member = m.member;
		r.var = m.var;
		r.removed = true;
		r.error = HorizonCode::extractFailureText(ExtractFailure::NoSuchMember, m.member, m.var);
		out.push_back(std::move(r));
	}
	return out;
}

bool assign(HorizonCode::Graph& g, const std::string& member, const std::string& var)
{
	if (member.empty()) return false;
	HorizonCode::ExtractSpec& s = g.extract;
	if (var.empty())
	{
		const size_t before = s.map.size();
		s.map.erase(std::remove_if(s.map.begin(), s.map.end(),
		                           [&](const ExtractMapEntry& m) { return m.member == member; }),
		            s.map.end());
		return s.map.size() != before;
	}
	if (ExtractMapEntry* m = entryFor(s, member))
	{
		if (m->var == var) return false;
		m->var = var;
		return true;
	}
	s.map.push_back({ member, var });
	return true;
}

int autoMapByName(HorizonCode::Graph& g, const HE::StructDef& def)
{
	const std::vector<ClassVar> vars = classVariables(g);
	int filled = 0;
	for (const HE::StructField& f : def.fields)
	{
		bool mapped = false;
		for (const ExtractMapEntry& m : g.extract.map)
			if (def.findField(m.member) == &f) { mapped = true; break; }
		if (mapped) continue;

		const ClassVar* pick = nullptr;
		if (const ClassVar* exact = findVar(vars, f.name); exact && fitOf(f, exact->decl).empty())
			pick = exact;
		if (!pick)
			for (const ClassVar& cv : vars)
				if (lower(cv.decl.name) == lower(f.name) && fitOf(f, cv.decl).empty())
				{ pick = &cv; break; }
		if (pick)
		{
			g.extract.map.push_back({ f.name, pick->decl.name });
			++filled;
			continue;
		}
		// A Ref member that names the sender: nothing else could mean it.
		const std::string ln = lower(f.name);
		if (selfFit(f).empty() && (ln == "self" || ln == "owner" || ln == "who" || ln == "source"))
		{
			g.extract.map.push_back({ f.name, HorizonCode::kExtractSelf });
			++filled;
		}
	}
	return filled;
}

bool chooseStruct(HorizonCode::Graph& g, const HE::StructDef& def)
{
	if (def.assetPath.empty()) return false;
	if (g.extract.structPath == def.assetPath) return false;
	g.extract.structPath = def.assetPath;
	g.extract.map.clear();
	autoMapByName(g, def);
	return true;
}

bool clear(HorizonCode::Graph& g)
{
	if (g.extract.empty() && g.extract.map.empty()) return false;
	g.extract = {};
	return true;
}

bool clearMap(HorizonCode::Graph& g)
{
	if (g.extract.map.empty()) return false;
	g.extract.map.clear();
	return true;
}

HE::StructDef structFromVariables(const HorizonCode::Graph& g, const std::vector<std::string>& vars,
                                  const std::string& name, const std::string& assetPath)
{
	HE::StructDef def;
	def.name = name;
	def.assetPath = assetPath;
	const std::vector<ClassVar> all = classVariables(g);
	for (const std::string& n : vars)
	{
		const ClassVar* cv = findVar(all, n);
		if (!cv || cv->decl.type == PinType::Exec) continue;
		bool dup = false;
		for (const HE::StructField& f : def.fields) dup = dup || f.name == n;
		if (dup) continue;
		const Variable& v = cv->decl;
		HE::StructField f;
		f.name = v.name;
		f.type = v.type;
		f.isArray = v.isArray;
		f.container = v.container;
		f.keyType = v.keyType;
		f.keyTypeName = v.keyTypeName;
		f.typeName = v.typeName;
		// The variable's default becomes the field's: what a listener reads when
		// a row is later cleared is then what the class started with. An enum
		// field stores its default by ENTRY NAME (TypeRegistry's JSON), a
		// nested struct none (its own definition supplies it).
		if (v.type == PinType::Enum && v.kind() == ContainerKind::None)
			f.defaultValue = HorizonCode::Value::ofString(v.s);
		else if (v.type != PinType::Struct || v.kind() != ContainerKind::None)
			f.defaultValue = HorizonCode::variableDefaultValue(v);
		def.fields.push_back(std::move(f));
	}
	return def;
}

bool isValidStructName(const std::string& name)
{
	if (name.empty() || name.size() > 64) return false;
	if (!(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
	for (char c : name)
		if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
	return true;
}

std::vector<std::string> extractedTo(const HorizonCode::Graph& g, const std::string& var)
{
	std::vector<std::string> out;
	for (const ExtractMapEntry& m : g.extract.map)
		if (m.var == var) out.push_back(m.member);
	return out;
}

std::string describe(const HorizonCode::Graph& g, const std::string& var)
{
	if (g.extract.empty()) return {};
	const std::vector<std::string> members = extractedTo(g, var);
	if (members.empty()) return {};
	const std::string s = structLabel(g.extract.structPath);
	std::string out = "Extracted to ";
	for (size_t i = 0; i < members.size(); ++i)
		out += (i ? ", " : "") + s + "." + members[i];
	return out;
}

int renameVariable(HorizonCode::Graph& g, const std::string& from, const std::string& to)
{
	if (from.empty() || to.empty() || from == to) return 0;
	int n = 0;
	for (ExtractMapEntry& m : g.extract.map)
		if (m.var == from) { m.var = to; ++n; }
	return n;
}

bool onDestroyedArg(const HorizonCode::Graph& listener, std::string& structPath)
{
	if (const HorizonCode::EventDecl* d = listener.findEvent(HorizonCode::kOnDestroyed))
	{
		structPath = d->hasArg && d->argType == PinType::Struct ? d->typeName : std::string();
		return true;
	}
	for (const HorizonCode::Node& n : listener.nodes)
		if (n.type == HorizonCode::NodeType::Event && n.s == HorizonCode::kOnDestroyed)
		{
			structPath = n.hasArg && n.propType == PinType::Struct ? n.typeName : std::string();
			return true;
		}
	return false;
}

Listener createOnDestroyedEvent(HorizonCode::Graph& listener, const std::string& structPath,
                                std::string* what)
{
	auto say = [&](std::string s) { if (what) *what = std::move(s); };
	const std::string want = structPath.empty() ? std::string("no data")
	                                            : structLabel(structPath);
	std::string have;
	if (onDestroyedArg(listener, have) && have != structPath)
	{
		// One OnDestroyed per class (design §3.6): a second with another type
		// cannot exist, and silently retyping the first would cut its wires.
		say("this class already has OnDestroyed (" +
		    (have.empty() ? std::string("no data") : structLabel(have)) + "), not (" + want + ")");
		return Listener::WrongType;
	}

	bool declared = listener.findEvent(HorizonCode::kOnDestroyed) != nullptr;
	if (!declared)
	{
		HorizonCode::EventDecl d;
		d.name = HorizonCode::kOnDestroyed;
		d.hasArg = !structPath.empty();
		d.argType = d.hasArg ? PinType::Struct : PinType::Float;
		d.typeName = structPath;
		listener.events.push_back(std::move(d));
	}
	bool handler = false;
	for (const HorizonCode::Node& n : listener.nodes)
		if (n.type == HorizonCode::NodeType::Event && n.s == HorizonCode::kOnDestroyed && n.subgraph == 0)
			handler = true;
	if (handler)
	{
		say(declared ? "OnDestroyed (" + want + ") is already there"
		             : "declared OnDestroyed (" + want + ")");
		return declared ? Listener::AlreadyThere : Listener::Created;
	}
	HorizonCode::Node ev;
	ev.type = HorizonCode::NodeType::Event;
	ev.s = HorizonCode::kOnDestroyed;
	ev.hasArg = !structPath.empty();
	ev.propType = ev.hasArg ? PinType::Struct : PinType::Float;
	ev.typeName = structPath;
	// Below everything already on the event graph, so it never lands on top
	// of a node somebody placed.
	float y = 40.0f;
	for (const HorizonCode::Node& n : listener.nodes)
		if (n.subgraph == 0) y = std::max(y, n.y + 160.0f);
	ev.x = 40.0f; ev.y = y;
	listener.addNode(std::move(ev));
	say(declared ? "added the OnDestroyed (" + want + ") handler"
	             : "created OnDestroyed (" + want + ")");
	return declared ? Listener::HandlerAdded : Listener::Created;
}
}
