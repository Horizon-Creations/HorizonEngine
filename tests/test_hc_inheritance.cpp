// HorizonCode class inheritance: a class asset's base may be ANOTHER class, and
// the chain is resolved by flattening — the ancestors' graphs merged into one,
// nearest-wins. What matters here is that the merge really behaves like C++
// inheritance (an override replaces, and only the override runs) and that the
// ancestry survives into the runtime identity, because that is what makes a
// Cast to a parent class succeed.
#include "doctest.h"
#include <HorizonCode/HcClassResolve.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <algorithm>   // sort / adjacent_find — libc++ pulls these in transitively, MSVC does not
#include <map>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	// A tiny in-memory project: key → (graph JSON, baseClass). Standing in for the
	// ContentManager, which is exactly why resolveClass takes a loader.
	struct Project
	{
		struct Entry { std::string json, base; };
		std::map<std::string, Entry> classes;

		void add(const std::string& key, const Graph& g, const std::string& base = {})
		{ classes[key] = { toJson(g), base }; }

		ClassLoader loader() const
		{
			return [this](const std::string& key, std::string& json, std::string& base)
			{
				const auto it = classes.find(key);
				if (it == classes.end()) return false;
				json = it->second.json;
				base = it->second.base;
				return true;
			};
		}
	};

	// Event(name) → SetVariable(var, ConstInt marker).
	void addEventWriting(Graph& g, const std::string& event, const std::string& var,
	                     int marker, bool overridable = false)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.overridable = overridable;
		const int e = g.addNode(std::move(ev));
		Node k; k.type = NodeType::ConstInt; k.f[0] = (float)marker;
		const int c = g.addNode(std::move(k));
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Int;
		const int s = g.addNode(std::move(sv));
		REQUIRE(g.connect(e, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}

	void addVar(Graph& g, const std::string& name, int def)
	{
		Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
		g.variables.push_back(v);
	}

	// FunctionEntry(name) → SetVariable(var, ConstInt marker). Its body sits in
	// the function's own sub-graph, which is what makes a replaced function's
	// body removable exactly.
	void addFunctionWriting(Graph& g, const std::string& fn, const std::string& var,
	                        int marker, bool overridable = false)
	{
		Node fe; fe.type = NodeType::FunctionEntry; fe.s = fn; fe.access = 0;
		fe.overridable = overridable;
		const int f = g.addNode(std::move(fe));
		Node k; k.type = NodeType::ConstInt; k.f[0] = (float)marker; k.subgraph = f;
		const int c = g.addNode(std::move(k));
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Int;
		sv.subgraph = f;
		const int s = g.addNode(std::move(sv));
		REQUIRE(g.connect(f, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}
}

TEST_CASE("a derived class inherits its ancestors' members")
{
	Project p;
	{
		Graph enemy;
		addVar(enemy, "hp", 100);
		addVar(enemy, "seen", 0);
		addEventWriting(enemy, "Construct", "seen", 1);
		addFunctionWriting(enemy, "TakeDamage", "hp", 50);
		p.add("Content/Enemy.hasset", enemy, "Entity");
	}
	{
		Graph goblin;                      // adds nothing of its own but a marker
		addVar(goblin, "mine", 0);
		addEventWriting(goblin, "BeginPlay", "mine", 7);
		p.add("Content/Goblin.hasset", goblin, "Content/Enemy.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/Goblin.hasset", p.loader());
	REQUIRE(rc.ok);
	// The chain is nearest-first and stops at the ENGINE row, which is what every
	// "is this an Entity / a player" question downstream reads.
	REQUIRE(rc.chain.size() == 1);
	CHECK(rc.chain[0] == "Content/Enemy.hasset");
	CHECK(rc.engineBase == "Entity");

	// Enemy's variables and members are simply THERE, in one graph.
	CHECK(rc.graph.findVariable("hp") != nullptr);
	CHECK(rc.graph.findVariable("mine") != nullptr);
	auto has = [&](NodeType t, const char* name)
	{
		for (const Node& n : rc.graph.nodes) if (n.type == t && n.s == name) return true;
		return false;
	};
	CHECK(has(NodeType::Event, "Construct"));          // from Enemy
	CHECK(has(NodeType::Event, "BeginPlay"));          // Goblin's own
	CHECK(has(NodeType::FunctionEntry, "TakeDamage")); // from Enemy

	// And it really runs: the inherited handler and function work on a Goblin.
	Runtime rt;
	const InstanceId g = rt.add(rc.graph, {}, { "Content/Goblin.hasset", rc.engineBase, rc.chain });
	rt.fireConstruct(g);
	CHECK(rt.getVariable(g, "seen").i == 1);
	CHECK(rt.callFunction(g, "TakeDamage"));
	CHECK(rt.getVariable(g, "hp").i == 50);
}

TEST_CASE("an override replaces the base's version — only one of them runs")
{
	Project p;
	{
		Graph base;
		addVar(base, "who", 0);
		addEventWriting(base, "Construct", "who", 1, /*overridable=*/true);
		addFunctionWriting(base, "Speak", "who", 1, /*overridable=*/true);
		p.add("Content/Base.hasset", base, "Object");
	}
	{
		Graph child;
		addEventWriting(child, "Construct", "who", 2);   // the override
		addFunctionWriting(child, "Speak", "who", 2);
		p.add("Content/Child.hasset", child, "Content/Base.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/Child.hasset", p.loader());
	REQUIRE(rc.ok);

	// Exactly ONE handler and ONE function survive per name. Two Event nodes of
	// the same name would BOTH fire, which is the precise failure this replaces.
	int events = 0, fns = 0;
	for (const Node& n : rc.graph.nodes)
	{
		if (n.type == NodeType::Event         && n.s == "Construct") ++events;
		if (n.type == NodeType::FunctionEntry && n.s == "Speak")     ++fns;
	}
	CHECK(events == 1);
	CHECK(fns == 1);

	Runtime rt;
	const InstanceId inst = rt.add(rc.graph, {}, { "Content/Child.hasset", rc.engineBase, rc.chain });
	rt.fireConstruct(inst);
	CHECK(rt.getVariable(inst, "who").i == 2);          // the child's, not the base's
	REQUIRE(rt.callFunction(inst, "Speak"));
	CHECK(rt.getVariable(inst, "who").i == 2);
}

TEST_CASE("a derived variable shadows the base's declaration")
{
	Project p;
	{
		Graph base; addVar(base, "hp", 100);
		p.add("Content/B.hasset", base, "Object");
	}
	{
		Graph child; addVar(child, "hp", 5);
		p.add("Content/C.hasset", child, "Content/B.hasset");
	}
	const ResolvedClass rc = resolveClass("Content/C.hasset", p.loader());
	REQUIRE(rc.ok);
	// One name, one slot in the instance store — the nearest declaration's value.
	int count = 0;
	for (const Variable& v : rc.graph.variables) if (v.name == "hp") ++count;
	CHECK(count == 1);

	Runtime rt;
	const InstanceId inst = rt.add(rc.graph);
	CHECK(rt.getVariable(inst, "hp").i == 5);
}

TEST_CASE("Cast reaches every class in the chain, and nothing outside it")
{
	Project p;
	{
		Graph g; p.add("Content/Enemy.hasset", g, "Entity");
	}
	{
		Graph g; p.add("Content/Goblin.hasset", g, "Content/Enemy.hasset");
	}
	{
		Graph g; p.add("Content/Chest.hasset", g, "Entity");
	}

	const ResolvedClass goblin = resolveClass("Content/Goblin.hasset", p.loader());
	REQUIRE(goblin.ok);
	Runtime rt;
	const InstanceId inst = rt.add(goblin.graph, {},
	                               { "Content/Goblin.hasset", goblin.engineBase, goblin.chain });

	CHECK(rt.instanceIsA(inst, "Content/Goblin.hasset"));   // itself
	CHECK(rt.instanceIsA(inst, "Content/Enemy.hasset"));    // its parent — the whole point
	CHECK(rt.instanceIsA(inst, "Entity"));                  // the engine row it lands on
	CHECK(rt.instanceIsA(inst, "Object"));
	// A sibling is not an ancestor, and neither is a class further down.
	CHECK_FALSE(rt.instanceIsA(inst, "Content/Chest.hasset"));
	CHECK_FALSE(rt.instanceIsA(inst, "PlayerCharacter"));

	// The other direction: an Enemy is NOT a Goblin.
	const ResolvedClass enemy = resolveClass("Content/Enemy.hasset", p.loader());
	const InstanceId plain = rt.add(enemy.graph, {},
	                                { "Content/Enemy.hasset", enemy.engineBase, enemy.chain });
	CHECK(rt.instanceIsA(plain, "Content/Enemy.hasset"));
	CHECK_FALSE(rt.instanceIsA(plain, "Content/Goblin.hasset"));
}

TEST_CASE("only members marked overridable are offered as overrides")
{
	Project p;
	{
		Graph base;
		addVar(base, "x", 0);
		addEventWriting(base, "Construct", "x", 1, /*overridable=*/true);
		addEventWriting(base, "Destruct",  "x", 2, /*overridable=*/false);
		addFunctionWriting(base, "Open",  "x", 3, /*overridable=*/true);
		addFunctionWriting(base, "Inner", "x", 4, /*overridable=*/false);
		p.add("Content/Door.hasset", base, "Object");
	}
	{
		Graph child; p.add("Content/BigDoor.hasset", child, "Content/Door.hasset");
	}

	const auto members = overridableMembers("Content/BigDoor.hasset", p.loader());
	auto has = [&](NodeType t, const char* n)
	{
		for (const auto& m : members) if (m.kind == t && m.name == n) return true;
		return false;
	};
	CHECK(has(NodeType::Event, "Construct"));
	CHECK(has(NodeType::FunctionEntry, "Open"));
	// Opt-in, like `virtual`: what the base did not mark is not on offer.
	CHECK_FALSE(has(NodeType::Event, "Destruct"));
	CHECK_FALSE(has(NodeType::FunctionEntry, "Inner"));
	for (const auto& m : members) CHECK(m.fromClass == "Content/Door.hasset");
}

TEST_CASE("a nearer ancestor's override is the one offered, and the deeper chain still resolves")
{
	Project p;
	{
		Graph a;
		addVar(a, "x", 0);
		addEventWriting(a, "Construct", "x", 1, /*overridable=*/true);
		p.add("Content/A.hasset", a, "Entity");
	}
	{
		Graph b;
		addEventWriting(b, "Construct", "x", 2, /*overridable=*/true);   // overrides A's
		p.add("Content/B.hasset", b, "Content/A.hasset");
	}
	{
		Graph c; p.add("Content/C.hasset", c, "Content/B.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/C.hasset", p.loader());
	REQUIRE(rc.ok);
	REQUIRE(rc.chain.size() == 2);
	CHECK(rc.chain[0] == "Content/B.hasset");   // nearest first
	CHECK(rc.chain[1] == "Content/A.hasset");
	CHECK(rc.engineBase == "Entity");

	Runtime rt;
	const InstanceId inst = rt.add(rc.graph, {}, { "Content/C.hasset", rc.engineBase, rc.chain });
	rt.fireConstruct(inst);
	CHECK(rt.getVariable(inst, "x").i == 2);    // B's version, not A's

	// The menu offers it ONCE, attributed to the nearest declaration.
	const auto members = overridableMembers("Content/C.hasset", p.loader());
	int construct = 0;
	for (const auto& m : members)
		if (m.kind == NodeType::Event && m.name == "Construct")
		{ ++construct; CHECK(m.fromClass == "Content/B.hasset"); }
	CHECK(construct == 1);
}

TEST_CASE("an inheritance cycle stops instead of hanging")
{
	Project p;
	{
		Graph a; addVar(a, "x", 1);
		p.add("Content/A.hasset", a, "Content/B.hasset");
	}
	{
		Graph b; addVar(b, "y", 2);
		p.add("Content/B.hasset", b, "Content/A.hasset");
	}
	// Resolving either end has to terminate and keep what it did resolve — a
	// half-resolved class still runs its own logic, which beats hanging.
	const ResolvedClass rc = resolveClass("Content/A.hasset", p.loader());
	REQUIRE(rc.ok);
	CHECK(rc.graph.findVariable("x") != nullptr);
	CHECK(rc.graph.findVariable("y") != nullptr);
}

TEST_CASE("a missing base class stops the chain without losing the class itself")
{
	Project p;
	{
		Graph g; addVar(g, "mine", 3);
		p.add("Content/Orphan.hasset", g, "Content/Gone.hasset");
	}
	const ResolvedClass rc = resolveClass("Content/Orphan.hasset", p.loader());
	REQUIRE(rc.ok);
	CHECK(rc.graph.findVariable("mine") != nullptr);
	CHECK(rc.chain.empty());
	// A class whose OWN asset is missing resolves to nothing at all.
	CHECK_FALSE(resolveClass("Content/Nope.hasset", p.loader()).ok);
}

TEST_CASE("merging keeps both sides' wiring intact")
{
	// Node ids collide between two independently authored graphs; the merge has
	// to renumber one side and carry its links along, or wires land on the wrong
	// nodes without anything failing.
	Project p;
	{
		Graph base;
		addVar(base, "a", 0);
		addEventWriting(base, "Construct", "a", 11);
		p.add("Content/P.hasset", base, "Object");
	}
	{
		Graph child;
		addVar(child, "b", 0);
		addEventWriting(child, "BeginPlay", "b", 22);
		p.add("Content/Q.hasset", child, "Content/P.hasset");
	}
	const ResolvedClass rc = resolveClass("Content/Q.hasset", p.loader());
	REQUIRE(rc.ok);

	// Every link still names two nodes that exist, and every node id is unique.
	std::vector<int> ids;
	for (const Node& n : rc.graph.nodes) ids.push_back(n.id);
	std::sort(ids.begin(), ids.end());
	CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
	for (const Link& l : rc.graph.links)
	{
		CHECK(rc.graph.findNode(l.srcNode) != nullptr);
		CHECK(rc.graph.findNode(l.dstNode) != nullptr);
	}

	Runtime rt;
	const InstanceId inst = rt.add(rc.graph, {}, { "Content/Q.hasset", rc.engineBase, rc.chain });
	rt.fireConstruct(inst);
	rt.fireBeginPlay(inst);
	CHECK(rt.getVariable(inst, "a").i == 11);   // the base's chain still wired
	CHECK(rt.getVariable(inst, "b").i == 22);   // and the child's
}

// ── The instance hierarchy (the levels the runtime actually executes) ────────
//
// Everything above asserts the MERGED graph, which is what the editor reads to
// show a class's whole member surface. What RUNS is one graph level per class,
// kept apart — so an override is resolved by asking the levels leaf-first
// rather than by having deleted the base's version at load time. These are the
// cases where the difference is observable.

TEST_CASE("levels are kept apart, root first, and the instance runs them as one object")
{
	Project p;
	{
		Graph enemy;
		addVar(enemy, "hp", 100);
		addVar(enemy, "seen", 0);
		addEventWriting(enemy, "Construct", "seen", 1);
		addFunctionWriting(enemy, "TakeDamage", "hp", 50);
		p.add("Content/Enemy.hasset", enemy, "Entity");
	}
	{
		Graph goblin;
		addVar(goblin, "mine", 0);
		addEventWriting(goblin, "BeginPlay", "mine", 7);
		p.add("Content/Goblin.hasset", goblin, "Content/Enemy.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/Goblin.hasset", p.loader());
	REQUIRE(rc.ok);
	// Two classes, two levels, root first — NOT merged into one.
	REQUIRE(rc.levels.size() == 2);
	CHECK(rc.levels[0].findVariable("hp")   != nullptr);   // Enemy
	CHECK(rc.levels[0].findVariable("mine") == nullptr);
	CHECK(rc.levels[1].findVariable("mine") != nullptr);   // Goblin

	Runtime rt;
	const InstanceId g = rt.addLevels(rc.levels, {}, { "Content/Goblin.hasset", rc.engineBase, rc.chain });

	// One object: an inherited handler and an inherited function both run on it,
	// and they share the one variable store.
	rt.fireConstruct(g);
	CHECK(rt.getVariable(g, "seen").i == 1);      // Enemy's Construct
	rt.fireBeginPlay(g);
	CHECK(rt.getVariable(g, "mine").i == 7);      // Goblin's own BeginPlay
	REQUIRE(rt.callFunction(g, "TakeDamage"));    // declared two levels up
	CHECK(rt.getVariable(g, "hp").i == 50);
	// And it is one object, not two: Get Self is a single reference.
	CHECK(rt.alive(g));
}

TEST_CASE("an override is resolved leaf-first — the base's version is not also run")
{
	Project p;
	{
		Graph base;
		addVar(base, "who", 0);
		addVar(base, "baseRan", 0);
		// The base writes a SECOND marker, so "the base also ran" would be
		// visible even if both wrote `who`.
		addEventWriting(base, "Construct", "baseRan", 1, /*overridable=*/true);
		addFunctionWriting(base, "Speak", "who", 1, /*overridable=*/true);
		p.add("Content/Base.hasset", base, "Object");
	}
	{
		Graph child;
		addEventWriting(child, "Construct", "who", 2);
		addFunctionWriting(child, "Speak", "who", 2);
		p.add("Content/Child.hasset", child, "Content/Base.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/Child.hasset", p.loader());
	REQUIRE(rc.ok);
	// BOTH declarations still exist — nothing was deleted at load time. The
	// override is a dispatch decision now, which is the whole point of levels.
	REQUIRE(rc.levels.size() == 2);
	auto declares = [](const Graph& g, NodeType t, const char* n)
	{
		for (const Node& node : g.nodes) if (node.type == t && node.s == n) return true;
		return false;
	};
	CHECK(declares(rc.levels[0], NodeType::Event, "Construct"));
	CHECK(declares(rc.levels[1], NodeType::Event, "Construct"));

	Runtime rt;
	const InstanceId inst = rt.addLevels(rc.levels, {}, { "Content/Child.hasset", rc.engineBase, rc.chain });
	rt.fireConstruct(inst);
	CHECK(rt.getVariable(inst, "who").i == 2);       // the child's ran
	CHECK(rt.getVariable(inst, "baseRan").i == 0);   // and the base's did NOT

	REQUIRE(rt.callFunction(inst, "Speak"));
	CHECK(rt.getVariable(inst, "who").i == 2);
}

TEST_CASE("a Do Once in the base and one in the child are two different Do Onces")
{
	// The trap that made per-level state necessary: a node id is unique inside
	// its own graph and nowhere else, so both graphs own a node 7. One shared
	// state map would have the child's Do Once arrive already fired.
	Project p;
	auto onceGraph = [](const char* event, const char* var, int marker)
	{
		Graph g;
		Variable v; v.name = var; v.type = PinType::Int; g.variables.push_back(v);
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(std::move(ev));
		Node once; once.type = NodeType::DoOnce;
		const int o = g.addNode(std::move(once));
		Node k; k.type = NodeType::ConstInt; k.f[0] = (float)marker;
		const int c = g.addNode(std::move(k));
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Int;
		const int s = g.addNode(std::move(sv));
		REQUIRE(g.connect(e, 0, o, 0));    // Event → Do Once
		REQUIRE(g.connect(o, 1, s, 0));    // Do Once "Then" → Set
		REQUIRE(g.connect(c, 0, s, 2));
		return g;
	};
	p.add("Content/B.hasset", onceGraph("Ping", "fromBase", 1), "Object");
	{
		// The child handles a DIFFERENT event, so both Do Onces can fire.
		Graph child = onceGraph("Pong", "fromChild", 2);
		p.add("Content/C.hasset", child, "Content/B.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/C.hasset", p.loader());
	REQUIRE(rc.ok);
	REQUIRE(rc.levels.size() == 2);

	Runtime rt;
	const InstanceId inst = rt.addLevels(rc.levels, {}, { "Content/C.hasset", rc.engineBase, rc.chain });
	rt.fireEvent(inst, "Ping");
	rt.fireEvent(inst, "Pong");
	// Both got through: their Do Onces do not share state despite sharing ids.
	CHECK(rt.getVariable(inst, "fromBase").i == 1);
	CHECK(rt.getVariable(inst, "fromChild").i == 2);

	// And each is still a Do ONCE: a second Ping changes nothing.
	rt.setVariable(inst, "fromBase", Value::ofInt(0));
	rt.fireEvent(inst, "Ping");
	CHECK(rt.getVariable(inst, "fromBase").i == 0);
}

TEST_CASE("a base class's private function stays private to it")
{
	// C++'s rule, and the reason the escalation asks for public members only.
	Project p;
	{
		Graph base;
		addVar(base, "x", 0);
		addFunctionWriting(base, "Open",   "x", 1, /*overridable=*/false);
		addFunctionWriting(base, "Hidden", "x", 2);
		// Make Hidden private.
		for (Node& n : base.nodes)
			if (n.type == NodeType::FunctionEntry && n.s == "Hidden") n.access = 1;
		p.add("Content/Door.hasset", base, "Object");
	}
	p.add("Content/BigDoor.hasset", Graph{}, "Content/Door.hasset");

	const ResolvedClass rc = resolveClass("Content/BigDoor.hasset", p.loader());
	REQUIRE(rc.ok);
	Runtime rt;
	const InstanceId inst = rt.addLevels(rc.levels, {},
	                                     { "Content/BigDoor.hasset", rc.engineBase, rc.chain });

	// A public inherited function is reachable…
	REQUIRE(rt.callFunction(inst, "Open"));
	CHECK(rt.getVariable(inst, "x").i == 1);
	// …and a private one is not, when the call crosses a class boundary.
	CHECK_FALSE(rt.callFunction(inst, "Hidden", /*requirePublic=*/true));
	CHECK(rt.getVariable(inst, "x").i == 1);
}

// ── What a derived class inherits BY NAME ────────────────────────────────────
// The editor asks for this: it lists the ancestors' members, offers them in the
// add menus, and refuses a new variable whose name the chain already uses.
TEST_CASE("inherited variables are reported nearest-first, once per name")
{
	Project p;
	{
		Graph root;
		addVar(root, "hp", 100);
		addVar(root, "tag", 1);
		p.add("Content/Actor.hasset", root, "Entity");
	}
	{
		Graph mid;
		addVar(mid, "hp", 250);          // redeclared: the nearer one must win
		addVar(mid, "armor", 5);
		p.add("Content/Enemy.hasset", mid, "Content/Actor.hasset");
	}
	{
		Graph leaf;
		addVar(leaf, "mine", 0);
		// A function-local is not an instance variable and cannot collide with
		// one, so it must not be reported.
		Variable loc; loc.name = "temp"; loc.type = PinType::Int; loc.scope = 42;
		leaf.variables.push_back(loc);
		p.add("Content/Goblin.hasset", leaf, "Content/Enemy.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/Goblin.hasset", p.loader());
	REQUIRE(rc.ok);
	const std::vector<InheritedVariable> iv = inheritedVariables(rc);

	// Three names, and the class's own "mine" is not among them.
	std::vector<std::string> names;
	for (const auto& e : iv) names.push_back(e.var.name);
	std::sort(names.begin(), names.end());
	CHECK(names == std::vector<std::string>{ "armor", "hp", "tag" });

	auto entry = [&](const char* n) -> const InheritedVariable*
	{
		for (const auto& e : iv) if (e.var.name == n) return &e;
		return nullptr;
	};
	// Nearest declaration wins, and it is attributed to the class that made it.
	REQUIRE(entry("hp"));
	CHECK(entry("hp")->var.f[0] == doctest::Approx(250.0f));
	CHECK(entry("hp")->fromClass == "Content/Enemy.hasset");
	REQUIRE(entry("tag"));
	CHECK(entry("tag")->fromClass == "Content/Actor.hasset");
}

TEST_CASE("a base class's PRIVATE variable is inherited as a reserved name")
{
	// It cannot be read from the derived class, but the derived class must not
	// declare one of the same name either: there is a single variable store per
	// instance, so a second declaration would not shadow the base's state, it
	// would write over it. Reporting it is how the editor can refuse the name.
	Project p;
	{
		Graph base;
		addVar(base, "secret", 3);
		base.variables.back().access = 1;   // private
		p.add("Content/Door.hasset", base, "Object");
	}
	p.add("Content/BigDoor.hasset", Graph{}, "Content/Door.hasset");

	const ResolvedClass rc = resolveClass("Content/BigDoor.hasset", p.loader());
	REQUIRE(rc.ok);
	const std::vector<InheritedVariable> iv = inheritedVariables(rc);
	REQUIRE(iv.size() == 1);
	CHECK(iv[0].var.name == "secret");
	CHECK(iv[0].var.access == 1);          // …and the editor can tell it apart

	// The lookup the editor's name check runs, over a graph carrying the list.
	Graph child;
	for (const auto& e : iv) child.inherited.push_back(e.var);
	CHECK(child.findVariable("secret") == nullptr);             // not this class's
	CHECK(child.findVariableOrInherited("secret") != nullptr);  // but the name is taken
}

TEST_CASE("only PUBLIC inherited functions are offered")
{
	Project p;
	{
		Graph base;
		addVar(base, "x", 0);
		addFunctionWriting(base, "Open",   "x", 1);
		addFunctionWriting(base, "Hidden", "x", 2);
		addFunctionWriting(base, "Close",  "x", 3);
		for (Node& n : base.nodes)
			if (n.type == NodeType::FunctionEntry && n.s == "Hidden") n.access = 1;
		p.add("Content/Door.hasset", base, "Object");
	}
	{
		Graph leaf;
		addVar(leaf, "y", 0);
		addFunctionWriting(leaf, "Close", "y", 9);   // overrides the base's
		p.add("Content/BigDoor.hasset", leaf, "Content/Door.hasset");
	}

	const ResolvedClass rc = resolveClass("Content/BigDoor.hasset", p.loader());
	REQUIRE(rc.ok);
	std::vector<std::string> fns;
	for (const auto& f : inheritedFunctions(rc)) fns.push_back(f.proto.s);
	std::sort(fns.begin(), fns.end());
	// "Hidden" is private. "Close" IS reported here — the editor is what drops
	// it, because the class declares its own and that is the entry a call binds
	// to; keeping the filter there means this function stays a plain answer to
	// "what does the chain declare".
	CHECK(fns == std::vector<std::string>{ "Close", "Open" });

	// Nothing is inherited when there is nothing above the class.
	const ResolvedClass root = resolveClass("Content/Door.hasset", p.loader());
	REQUIRE(root.ok);
	CHECK(inheritedFunctions(root).empty());
	CHECK(inheritedVariables(root).empty());
}
