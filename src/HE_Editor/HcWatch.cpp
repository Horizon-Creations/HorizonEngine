#include "HcWatch.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Types/TypeRegistry.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_set>

namespace HcWatch
{
namespace
{
	using namespace HorizonCode;

	// Floats as a person reads them: 1.5, not 1.500000; 100, not 1e+02.
	std::string num(float f)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%g", (double)f);
		return buf;
	}

	std::string tuple(const float* v, int n)
	{
		std::string out = "(";
		for (int i = 0; i < n; ++i) { if (i) out += ", "; out += num(v[i]); }
		return out + ")";
	}

	// A string literal as it would be typed: quoted, with the two characters
	// that would break the line spelled out. Anything longer than a line is
	// cut — a watch row is a glance, not a document.
	std::string quoted(const std::string& s)
	{
		constexpr size_t kMax = 120;
		std::string out = "\"";
		size_t n = 0;
		for (char c : s)
		{
			if (n++ >= kMax) { out += "…"; break; }
			if (c == '\n')      out += "\\n";
			else if (c == '"')  out += "\\\"";
			else                out += c;
		}
		return out + "\"";
	}

	const char* scalarTypeName(PinType t)
	{
		switch (t)
		{
			case PinType::Float:     return "Float";
			case PinType::Double:    return "Double";
			case PinType::Bool:      return "Bool";
			case PinType::Int:       return "Int";
			case PinType::String:    return "String";
			case PinType::Vec2:      return "Vec2";
			case PinType::Color:     return "Color";
			case PinType::Vec3:      return "Vec3";
			case PinType::Vec4:      return "Vec4";
			case PinType::Ref:       return "Object";
			case PinType::Transform: return "Transform";
			case PinType::Enum:      return "Enum";
			case PinType::Struct:    return "Struct";
			default:                 return "Exec";
		}
	}

	// "Content/Enemies/Goblin.hasset" → "Goblin". The definition assets and
	// the class assets are both spelled as paths; a watch row wants the name.
	std::string stem(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
		const size_t dot = s.rfind('.');
		if (dot != std::string::npos && dot > 0) s.erase(dot);
		return s;
	}

	// A scalar's type: the definition's name for an enum or a struct, the pin
	// type's name for everything else.
	std::string scalarLabel(PinType t, const std::string& typeName)
	{
		if ((t == PinType::Enum || t == PinType::Struct) && !typeName.empty()) return stem(typeName);
		return scalarTypeName(t);
	}

	std::string formatScalar(const Value& v, const Runtime* rt, int depth);

	std::string formatItems(const std::vector<Value>& items, const Runtime* rt, int depth)
	{
		std::string out;
		for (size_t i = 0; i < items.size(); ++i)
		{
			if (i == kMaxItems) { out += ", …"; break; }
			if (i) out += ", ";
			out += formatScalar(items[i], rt, depth + 1);
		}
		return out;
	}

	std::string formatScalar(const Value& v, const Runtime* rt, int depth)
	{
		// A struct inside a struct inside a struct: the row would run off the
		// window long before the reader got there.
		if (depth > 3) return "…";
		switch (v.type)
		{
			case PinType::Float:  return num(v.f);
			case PinType::Double:
			{
				// %.15g, not num()'s %g: a watch that shows an epoch time as
				// 1.75883e+09 hides exactly the digits this type carries.
				char buf[40];
				std::snprintf(buf, sizeof(buf), "%.15g", v.d);
				return buf;
			}
			case PinType::Bool:   return v.b ? "true" : "false";
			case PinType::Int:    return std::to_string(v.i);
			case PinType::String: return quoted(v.s);
			case PinType::Vec2:   return tuple(&v.v2.x, 2);
			case PinType::Vec3:   return tuple(&v.v3.x, 3);
			case PinType::Vec4:   return tuple(&v.v4.x, 4);
			case PinType::Color:  return tuple(&v.col.x, 4);
			case PinType::Transform:
				return "pos " + tuple(&v.tpos.x, 3) + " rot " + tuple(&v.trot.x, 3) +
				       " scale " + tuple(&v.tscl.x, 3);
			case PinType::Ref:
			{
				if (v.ref == 0) return "none";
				const std::string id = "#" + std::to_string(v.ref);
				if (!rt) return id;
				if (!rt->alive(v.ref)) return id + " (destroyed)";
				const std::string cls = classLabel(rt->classKeyOf(v.ref));
				return cls.empty() ? id : cls + " " + id;
			}
			case PinType::Enum:
			{
				// The entry's name when the definition is loaded; the raw
				// number otherwise — a value is never hidden for want of a name.
				HE::EnumDef def;
				if (!v.typeName.empty() && HE::TypeRegistry::instance().getEnum(v.typeName, def))
					if (const HE::EnumEntry* e = def.findValue(v.i)) return e->name;
				return std::to_string(v.i);
			}
			case PinType::Struct:
			{
				// Fields in definition order, parallel to items (see Value::typeName).
				HE::StructDef def;
				const bool named = !v.typeName.empty() &&
				                   HE::TypeRegistry::instance().getStruct(v.typeName, def) &&
				                   def.fields.size() == v.items.size();
				std::string out = "{";
				for (size_t i = 0; i < v.items.size(); ++i)
				{
					if (i == kMaxItems) { out += ", …"; break; }
					if (i) out += ", ";
					if (named) out += def.fields[i].name + ": ";
					out += formatScalar(v.items[i], rt, depth + 1);
				}
				return out + "}";
			}
			default: return "";
		}
	}

	// ── The stopped run's entry ──────────────────────────────────────────────
	// SuspendedRun does not record the node the run was fired from, and the
	// event argument alone cannot say whether there was one (a run without an
	// argument carries the zero Float). So walk the exec links BACKWARDS from
	// where the run is — from the outermost steering node still open, when
	// there is one, since the stop itself may be deep inside its For Each —
	// until an entry node turns up. A chain reachable from two events is
	// ambiguous; the first entry found wins, and a wrong name on a rare graph
	// costs a mislabelled row, not a wrong value.
	//
	// The walk does NOT pass a Delay: the chain after it is resumed as a
	// FRESH run (Runner::resumeFrom — the event argument is gone), so a stop
	// behind a Delay has no argument to show, and finding the Event beyond it
	// would put the zero Value under the event's name. No entry, no row.
	const Node* findEntry(const Graph& g, int fromNode)
	{
		std::vector<int> stack{ fromNode };
		std::unordered_set<int> seen;
		while (!stack.empty())
		{
			const int cur = stack.back();
			stack.pop_back();
			if (!seen.insert(cur).second) continue;
			const Node* n = g.findNode(cur);
			if (!n) continue;
			if (n->type == NodeType::Event || n->type == NodeType::InputAction ||
			    n->type == NodeType::FunctionEntry)
				return n;
			if (n->type == NodeType::Delay) continue;
			const NodeSigCounts counts = signatureCountsOf(*n);
			for (const Link& l : g.links)
				if (l.dstNode == cur && l.dstPin < counts.execIns)   // an exec link into `cur`
					stack.push_back(l.srcNode);
		}
		return nullptr;
	}

	Row rowOf(std::string name, const Value& v, const Runtime* rt)
	{
		Row r;
		r.name  = std::move(name);
		r.type  = typeLabel(v);
		r.value = formatValue(v, rt);
		if (v.type == PinType::Ref && !v.isArray && v.ref != 0 && rt && rt->alive(v.ref)) r.ref = v.ref;
		return r;
	}

	// An unordered map as sorted rows — the list must hold still from frame
	// to frame, and a hash order does not.
	void appendSorted(std::vector<Row>& rows, const std::unordered_map<std::string, Value>& vars,
	                  const Runtime* rt)
	{
		std::map<std::string, const Value*> sorted;
		for (const auto& [name, value] : vars) sorted[name] = &value;
		for (const auto& [name, value] : sorted) rows.push_back(rowOf(name, *value, rt));
	}
}

std::string formatValue(const Value& v, const Runtime* rt)
{
	switch (v.kind())
	{
		case ContainerKind::None:
			return formatScalar(v, rt, 0);
		case ContainerKind::Array:
		case ContainerKind::Set:
			return "[" + std::to_string(v.items.size()) + "] {" + formatItems(v.items, rt, 0) + "}";
		case ContainerKind::Map:
		{
			std::string out = "[" + std::to_string(v.items.size()) + "] {";
			for (size_t i = 0; i < v.items.size(); ++i)
			{
				if (i == kMaxItems) { out += ", …"; break; }
				if (i) out += ", ";
				// Keys are parallel to items; a map read back from an older
				// asset may be short of keys, and then the value stands alone.
				if (i < v.keys.size()) out += formatScalar(v.keys[i], rt, 1) + ": ";
				out += formatScalar(v.items[i], rt, 1);
			}
			return out + "}";
		}
	}
	return {};
}

std::string typeLabel(const Value& v)
{
	const std::string elem = scalarLabel(v.type, v.typeName);
	switch (v.kind())
	{
		case ContainerKind::None:  return elem;
		case ContainerKind::Array: return "Array of " + elem;
		case ContainerKind::Set:   return "Set of " + elem;
		case ContainerKind::Map:   return "Map of " + scalarLabel(v.keyType, v.keyTypeName) + " to " + elem;
	}
	return elem;
}

std::string classLabel(const std::string& classKey)
{
	if (classKey.empty()) return {};
	// The two editor-owned graphs (HorizonCodeRuntime.cpp kGameInstanceIdentity,
	// HorizonWorld::levelScriptKey) — the same spellings HcExecTrace::tabKeyFor
	// maps to tabs.
	if (classKey == "__game_instance__") return "Game Instance";
	if (classKey.rfind("level:", 0) == 0) return "Level Script";
	return stem(classKey);
}

std::string nodeLabel(const Graph& g, int nodeId)
{
	const Node* n = g.findNode(nodeId);
	if (!n) return "Node " + std::to_string(nodeId);
	std::string label = nodeDisplayName(n->type);
	// What the node names, for the types where `s` IS the name a reader knows
	// it by. A string literal's `s` is its payload and a Print's is nothing;
	// an engine call's is its registry id, which is close enough to a name.
	switch (n->type)
	{
		case NodeType::Event: case NodeType::FunctionEntry: case NodeType::FunctionCall:
		case NodeType::GetVariable: case NodeType::SetVariable:
		case NodeType::GetProperty: case NodeType::SetProperty:
		case NodeType::GetExternal: case NodeType::SetExternal:
		case NodeType::InputAction: case NodeType::EngineCall:
			if (!n->s.empty()) label += " " + n->s;
			break;
		default: break;
	}
	return label;
}

std::vector<Row> variableRows(const Runtime& rt, uint32_t instance)
{
	std::vector<Row> rows;
	if (!rt.alive(instance)) return rows;
	appendSorted(rows, rt.variablesSnapshot(instance), &rt);
	return rows;
}

Snapshot build(const Runtime& rt, size_t runIndex)
{
	Snapshot s;
	const SuspendedRun* run = rt.suspendedRun(runIndex);
	if (!run) return s;
	s.valid    = true;
	s.instance = run->instance;
	s.classKey = run->classKey;
	s.level    = run->level;
	s.nodeId   = run->nodeId;
	s.runCount = rt.suspendedSites().size();
	const Graph& g = rt.graphAt(run->instance, run->level);
	s.nodeLabel = nodeLabel(g, run->nodeId);

	// ── The event argument ──
	// Only when the run was fired from an event (or an axis action) that
	// carries one. A run started by callFunction enters at a FunctionEntry,
	// whose arguments are in the first call frame below.
	{
		const int from = run->outer.empty() ? run->nodeId : run->outer.back().nodeId;
		if (const Node* entry = findEntry(g, from))
			if (entry->type != NodeType::FunctionEntry && entry->hasArg)
			{
				Section sec;
				sec.kind  = Section::Kind::EventArg;
				sec.title = nodeLabel(g, entry->id);
				PinDesc pin;
				const std::string name = dataPinDescOf(*entry, false, 0, pin) ? pin.name : "Argument";
				sec.rows.push_back(rowOf(name, run->eventArg, &rt));
				s.sections.push_back(std::move(sec));
			}
	}

	// ── Call frames, innermost first ──
	// The stack holds the innermost LAST; the reader wants the function the
	// stop is in at the top, like any debugger's stack. Arguments in
	// declaration order (they are the function's signature), locals by name.
	for (size_t k = run->callStack.size(); k-- > 0; )
	{
		const CallFrame& frame = run->callStack[k];
		Section sec;
		sec.kind = Section::Kind::Frame;
		const Node* entry = g.findNode(frame.fnEntryId);
		sec.title = entry ? "Function " + entry->s : "Function (node " + std::to_string(frame.fnEntryId) + ")";
		for (size_t a = 0; a < frame.args.size(); ++a)
		{
			const std::string name = (entry && a < entry->params.size() && !entry->params[a].name.empty())
			                       ? entry->params[a].name : "Input " + std::to_string(a + 1);
			sec.rows.push_back(rowOf(name, frame.args[a], &rt));
		}
		appendSorted(sec.rows, frame.locals, &rt);
		s.sections.push_back(std::move(sec));
	}

	// ── The instance's variables ──
	{
		Section sec;
		sec.kind  = Section::Kind::Variables;
		sec.title = "Variables";
		appendSorted(sec.rows, rt.variablesSnapshot(run->instance), &rt);
		s.sections.push_back(std::move(sec));
	}

	// ── Outputs the run produced before the stop ──
	// The exec-output cache: a Create Widget's id, a Create Object's reference,
	// a function call's results — what a data read below the stop will find.
	// Node order (id) so the list holds still; one row per data-out that has
	// a value, named "<node> ▸ <pin>".
	if (!run->execOutputs.empty())
	{
		Section sec;
		sec.kind  = Section::Kind::Outputs;
		sec.title = "Outputs so far";
		std::map<int, const std::vector<Value>*> sorted;
		for (const auto& [nodeId, values] : run->execOutputs) sorted[nodeId] = &values;
		for (const auto& [nodeId, values] : sorted)
		{
			const Node* n = g.findNode(nodeId);
			const std::string label = nodeLabel(g, nodeId);
			for (size_t i = 0; i < values->size(); ++i)
			{
				PinDesc pin;
				const std::string pinName = (n && dataPinDescOf(*n, false, (int)i, pin))
				                          ? pin.name : "Out " + std::to_string(i + 1);
				sec.rows.push_back(rowOf(label + " > " + pinName, (*values)[i], &rt));
			}
		}
		if (!sec.rows.empty()) s.sections.push_back(std::move(sec));
	}
	return s;
}

} // namespace HcWatch
