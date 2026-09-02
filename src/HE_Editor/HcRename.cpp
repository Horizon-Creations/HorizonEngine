#include "HcRename.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>   // which parameter names an animation is the registry's answer

#include <algorithm>
#include <cstring>    // strcmp — ApiParam::name is a const char*

namespace HcRename
{
namespace
{
	using NT = HorizonCode::NodeType;
	using PT = HorizonCode::PinType;

	// The unified index of a node's first DATA input, which is the Target pin on
	// every node this file looks at. Same arithmetic as HcGraphHost::pinRanges,
	// spelled out again so this half of the rename stays free of the ImGui half
	// (and can therefore be tested).
	int targetPin(const HorizonCode::Node& n)
	{
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		return (int)s.execIns.size() + (int)s.execOuts.size();
	}

	const HorizonCode::Node* wiredInto(const HorizonCode::Graph& g, int nodeId, int pin)
	{
		for (const HorizonCode::Link& l : g.links)
			if (l.dstNode == nodeId && l.dstPin == pin) return g.findNode(l.srcNode);
		return nullptr;
	}

	bool contains(const std::vector<std::string>& v, const std::string& s)
	{
		return std::find(v.begin(), v.end(), s) != v.end();
	}

	std::string line(const HorizonCode::Node& n)
	{
		std::string s = HorizonCode::nodeDisplayName(n.type);
		if (!n.s.empty()) s += " \"" + n.s + "\"";
		return s;
	}

	// ── The pins that name an animation clip ─────────────────────────────────
	// Asked of the REGISTRY, not of a list of row ids kept here: an engine row
	// takes an animation by having a String parameter called "animation", and
	// four of them do today. A list here would be a second opinion about which
	// rows those are, and the day somebody adds a fifth it would be wrong in the
	// quiet way — a rename that silently misses one node.
	//
	// Returns the unified pin indices of this node's animation parameters, and
	// (out) the widget Ref parameter's index, which is what says WHOSE animation
	// it is. Empty for every node that names none.
	std::vector<int> animationPinsOf(const HorizonCode::Node& n, int& outWidgetPin,
	                                 bool& outWidgetDefaultsToSelf)
	{
		std::vector<int> pins;
		outWidgetPin = -1;
		outWidgetDefaultsToSelf = false;
		if (n.type != NT::EngineCall) return pins;
		const HE::api::ApiFn* fn = HE::api::find(n.s);
		if (!fn) return pins;
		const HorizonCode::NodeSig s = HorizonCode::signatureOf(n);
		const int dataIn0 = (int)s.execIns.size() + (int)s.execOuts.size();
		for (size_t i = 0; i < fn->params.size() && i < s.dataIns.size(); ++i)
		{
			const HE::api::ApiParam& p = fn->params[i];
			// std::strcmp, not ==: ApiParam::name is a const char*, so == compares
			// POINTERS. It even seems to work while the linker happens to fold
			// both literals into one, which is the worst way for a bug like this
			// to behave — right on this machine, wrong on the next.
			const bool named = p.name && std::strcmp(p.name, "animation") == 0;
			if (named && p.type == PT::String)
				pins.push_back(dataIn0 + (int)i);
			// The Ref that says whose animation. `selfDefault` is the registry's
			// own word for "left empty, this is the calling instance", which is
			// exactly the proof an unwired pin needs.
			else if (p.type == PT::Ref && outWidgetPin < 0)
			{
				outWidgetPin = dataIn0 + (int)i;
				outWidgetDefaultsToSelf = p.selfDefault;
			}
		}
		return pins;
	}

	// What name a pin actually carries: its inline default, or a Const String
	// wired to it. Anything else wired in (a variable, a joined string) is a name
	// nobody can read from here, and `outReadable` says so — a rename must not
	// pretend to have looked at it.
	std::string nameAtPin(const HorizonCode::Graph& g, const HorizonCode::Node& n, int pin,
	                      const HorizonCode::Node** outConst, bool& outReadable)
	{
		if (outConst) *outConst = nullptr;
		outReadable = true;
		if (const HorizonCode::Node* src = wiredInto(g, n.id, pin))
		{
			if (src->type != NT::ConstString) { outReadable = false; return {}; }
			if (outConst) *outConst = src;
			return src->s;
		}
		const auto it = n.pinDefaults.find(pin);
		return it == n.pinDefaults.end() ? std::string{} : it->second.s;
	}

	// Which node types carry the name of THIS kind of member on another instance.
	bool reachesIn(Member m, NT t)
	{
		switch (m)
		{
			case Member::Function: return t == NT::CallExternal;
			case Member::Variable: return t == NT::GetExternal || t == NT::SetExternal;
			case Member::Event:    return t == NT::BindEvent   || t == NT::EmitEvent;
			// An animation is never named on the node — see the Member comment.
			// Its own branch does the walking.
			case Member::Animation: return false;
		}
		return false;
	}

	// The same node twice in one list is one line too many in the dialog, and the
	// declaration half and the Bind Event half can both reach the same Event node.
	void dedupe(std::vector<Hit>& hits)
	{
		std::vector<Hit> out;
		for (const Hit& h : hits)
		{
			const bool seen = std::any_of(out.begin(), out.end(), [&](const Hit& o) {
				return h.node ? o.node == h.node : (o.node == 0 && o.decl == h.decl); });
			if (!seen) out.push_back(h);
		}
		hits = std::move(out);
	}
} // namespace

std::string targetClassOf(const HorizonCode::Graph& g, const HorizonCode::Node& n,
                          const std::string& selfKey, const std::string& giKey)
{
	// What the node itself records wins: the pickers write it, and it survives a
	// Target wire being re-routed through something this walk cannot read.
	if (!n.className.empty()) return n.className;

	const HorizonCode::Node* src = wiredInto(g, n.id, targetPin(n));
	return src ? classOfRefSource(g, *src, selfKey, giKey) : std::string();
}

std::string classOfRefSource(const HorizonCode::Graph& g, const HorizonCode::Node& source,
                             const std::string& selfKey, const std::string& giKey)
{
	const HorizonCode::Node* src = &source;
	switch (src->type)
	{
		case NT::GetSelf:         return selfKey;
		case NT::GetGameInstance: return giKey;
		case NT::CreateObject:
		case NT::CreateWidget:    return src->s;
		// A Cast to an ENGINE class says nothing about a HorizonCode member: the
		// taxonomy rows have no graph and declare no functions of their own.
		case NT::Cast:
			return (src->s.empty() || HorizonCode::findEngineClass(src->s)) ? std::string() : src->s;
		case NT::GetVariable:
		case NT::SetVariable:     // the set node passes its value through
		{
			const HorizonCode::Variable* v = g.findVariableOrInherited(src->s);
			return (v && v->type == PT::Ref) ? v->className : std::string();
		}
		case NT::ForEach:         // element of an object array (class adopted on connect)
			return src->propType == PT::Ref ? src->s : std::string();
		default: return {};
	}
}

Plan planGraph(const HorizonCode::Graph& g, Role role,
               const std::vector<std::string>& targetKeys,
               const std::string& graphKey, const std::string& giKey,
               const Target& t)
{
	Plan p;
	if (t.oldName.empty() || t.newName.empty() || t.oldName == t.newName) return p;

	// ── An animation clip, which is named in a pin rather than on a node ─────
	// Same contract as everything below it: rename what this graph can PROVE is
	// the renamed widget's clip, report what names it but cannot be proved, and
	// touch nothing else.
	if (t.member == Member::Animation)
	{
		// A Const String may feed more than one pin, and there is only ONE string
		// to rewrite. So each literal is counted rather than judged: how many
		// wires leave it, and how many of those land on a pin we just proved is
		// ours. The same literal wired into a Set Text is a label on the screen,
		// and rewriting it to fix a graph would be a rename changing words.
		struct LitUse { const HorizonCode::Node* node; int ours = 0; };
		std::vector<LitUse> lits;
		auto litUse = [&](const HorizonCode::Node* n) -> LitUse& {
			for (LitUse& u : lits) if (u.node == n) return u;
			lits.push_back({ n, 0 });
			return lits.back();
		};

		for (const HorizonCode::Node& n : g.nodes)
		{
			int widgetPin = -1;
			bool defaultsToSelf = false;
			const std::vector<int> pins = animationPinsOf(n, widgetPin, defaultsToSelf);
			if (pins.empty()) continue;

			// Whose animation is it? An unwired Target on a row that stands in
			// for itself IS this graph's class; otherwise ask where the Ref came
			// from, the same question every other member kind asks.
			std::string key;
			if (widgetPin >= 0)
			{
				if (const HorizonCode::Node* src = wiredInto(g, n.id, widgetPin))
					key = classOfRefSource(g, *src, graphKey, giKey);
				else if (defaultsToSelf)
					key = graphKey;
			}
			const bool proven = !key.empty();
			const bool ours   = proven && contains(targetKeys, key);

			for (const int pin : pins)
			{
				const HorizonCode::Node* lit = nullptr;
				bool readable = true;
				const std::string named = nameAtPin(g, n, pin, &lit, readable);
				// A name arriving through a variable is a name this cannot read.
				// Silence is right: it is not evidence of the old name.
				if (!readable || named != t.oldName) continue;
				if (lit) { LitUse& u = litUse(lit); if (ours) ++u.ours; continue; }
				if (ours) p.rename.push_back({ n.id, {}, line(n) });
				// Provably somebody ELSE's widget: not ours, and reporting it
				// would bury the real warnings under every same-named clip in
				// the project. Only the unprovable ones are worth a line.
				else if (!proven) p.unsure.push_back({ n.id, {}, line(n) });
			}
		}

		for (const LitUse& u : lits)
		{
			if (u.ours == 0) continue;          // names it, but never for us
			int outLinks = 0;
			for (const HorizonCode::Link& l : g.links) if (l.srcNode == u.node->id) ++outLinks;
			// Every wire out of it is one of ours, so the string means one thing
			// and can be rewritten. Otherwise it is shared with something this
			// rename has no business touching, and that is worth saying out loud.
			if (u.ours == outLinks) p.rename.push_back({ u.node->id, {}, line(*u.node) });
			else                    p.unsure.push_back({ u.node->id, {}, line(*u.node) });
		}

		dedupe(p.rename);
		dedupe(p.unsure);
		// The declaration is the widget's own clip list, not anything in a graph,
		// so there is no Role::Declares half to run.
		return p;
	}

	// ── What reaches in from here ────────────────────────────────────────────
	bool boundHere = false;
	for (const HorizonCode::Node& n : g.nodes)
	{
		if (!reachesIn(t.member, n.type) || n.s != t.oldName) continue;
		const std::string key = targetClassOf(g, n, graphKey, giKey);
		if (key.empty())
		{
			// Names it, but the Target could be anything. Renaming on the strength
			// of the name alone is how a rename breaks a graph that was correct, so
			// this is reported and left standing.
			p.unsure.push_back({ n.id, {}, line(n) });
		}
		else if (contains(targetKeys, key))
		{
			p.rename.push_back({ n.id, {}, line(n) });
			if (n.type == NT::BindEvent) boundHere = true;
		}
		// Else it resolved to some OTHER class: provably not ours, and reporting it
		// would bury the real warnings under every same-named member in the project.
	}

	// A Bind Event uses ONE name for both ends: when the Target fires event X,
	// THIS graph's own "Event X" node runs. So a proven bind has to drag the local
	// handler along — and that is only safe while X means one thing here.
	//
	// Only for a graph that does NOT declare the event itself. The class being
	// renamed may well bind its own event on another instance of itself, and there
	// the local handler belongs to the declaration below, which renames it anyway.
	if (boundHere && role == Role::Other)
	{
		const bool ownsOld = g.findEvent(t.oldName) != nullptr;
		const bool ownsNew = g.findEvent(t.newName) != nullptr;
		if (ownsOld || ownsNew)
		{
			p.blocked   = p.rename;
			p.rename.clear();
			p.blockedWhy = ownsOld
				? "this graph declares an event of the same name, so its handler could be either one"
				: "this graph already has an event called \"" + t.newName + "\"";
		}
		else
			for (const HorizonCode::Node& n : g.nodes)
				if (n.type == NT::Event && n.s == t.oldName)
					p.rename.push_back({ n.id, {}, line(n) });
	}

	// ── The declaration living here, and everything in this graph using it ───
	// Role::Declares is the class itself; Role::Overrides is a class deriving from
	// it, whose override has to follow the base or it stops overriding anything.
	// Both look the same from here: a local declaration of that name.
	if (role != Role::Other)
	{
		switch (t.member)
		{
			case Member::Function:
				for (const HorizonCode::Node& n : g.nodes)
					if ((n.type == NT::FunctionEntry || n.type == NT::FunctionCall ||
					     n.type == NT::FunctionReturn) && n.s == t.oldName)
						p.rename.push_back({ n.id, {}, line(n) });
				break;
			case Member::Variable:
				if (g.findVariable(t.oldName))
					p.rename.push_back({ 0, t.oldName, "Variable \"" + t.oldName + "\"" });
				for (const HorizonCode::Node& n : g.nodes)
					if ((n.type == NT::GetVariable || n.type == NT::SetVariable) && n.s == t.oldName)
						p.rename.push_back({ n.id, {}, line(n) });
				break;
			case Member::Event:
				if (g.findEvent(t.oldName))
					p.rename.push_back({ 0, t.oldName, "Event \"" + t.oldName + "\"" });
				for (const HorizonCode::Node& n : g.nodes)
					if ((n.type == NT::Event || n.type == NT::EmitEvent) && n.s == t.oldName)
						p.rename.push_back({ n.id, {}, line(n) });
				break;
		}
	}

	dedupe(p.rename);
	dedupe(p.unsure);
	return p;
}

bool apply(HorizonCode::Graph& g, const Plan& p, const Target& t)
{
	bool changed = false;
	// An animation's name lives in a PIN, so the write is a different one — a
	// Const String gets its `s`, an engine call gets its default. Which pins
	// those are is worked out again here rather than carried in the Hit: the plan
	// stays a list of nodes, and re-asking cannot disagree with itself.
	if (t.member == Member::Animation)
	{
		for (const Hit& h : p.rename)
		{
			HorizonCode::Node* n = h.node ? g.findNode(h.node) : nullptr;
			if (!n) continue;
			if (n->type == NT::ConstString)
			{
				if (n->s == t.oldName) { n->s = t.newName; changed = true; }
				continue;
			}
			int widgetPin = -1;
			bool defaultsToSelf = false;
			for (const int pin : animationPinsOf(*n, widgetPin, defaultsToSelf))
			{
				const auto it = n->pinDefaults.find(pin);
				if (it != n->pinDefaults.end() && it->second.s == t.oldName)
				{ it->second.s = t.newName; changed = true; }
			}
		}
		return changed;
	}
	for (const Hit& h : p.rename)
	{
		if (h.node)
		{
			HorizonCode::Node* n = g.findNode(h.node);
			if (n && n->s != t.newName) { n->s = t.newName; changed = true; }
		}
		else if (!h.decl.empty() && t.member == Member::Variable)
		{
			if (HorizonCode::Variable* v = g.findVariable(h.decl)) { v->name = t.newName; changed = true; }
		}
		else if (!h.decl.empty() && t.member == Member::Event)
		{
			if (HorizonCode::EventDecl* e = g.findEvent(h.decl)) { e->name = t.newName; changed = true; }
		}
	}
	return changed;
}

} // namespace HcRename
