#include "doctest.h"

#include "HcRename.h"
#include <HorizonCode/HorizonCode.h>

#include <string>

// ── Which graphs a rename may touch, and which it may only report ────────────
// Renaming a member of a HorizonCode class has to reach the graphs that call it
// from outside, and it must NOT reach the ones that call something else of the
// same name. The nodes carry no target class of their own (until a picker writes
// one), so the only honest answer comes from the wire: a Ref is produced by a
// Create Object, a Cast, a typed Ref variable, a Get Self or a Get Game Instance,
// and each of those names its class.
//
// That is the whole risk of this feature in one place. A rename that is too
// eager silently rewrites a graph that was correct, which is worse than the
// breakage it set out to fix, so the tests below are mostly about what must NOT
// move.

using namespace HorizonCode;
using HcRename::Member;
using HcRename::Role;

namespace
{

constexpr const char* kEnemy  = "Classes/Enemy.hasset";
constexpr const char* kGoblin = "Classes/Goblin.hasset";
constexpr const char* kChest  = "Classes/Chest.hasset";
constexpr const char* kGI     = "GameInstance.hcode";

int add(Graph& g, NodeType t, const std::string& s = {})
{
	Node n;
	n.id   = (int)g.nodes.size() + 1;
	n.type = t;
	n.s    = s;
	g.nodes.push_back(n);
	return n.id;
}

// Wire `src`'s first data output into `dst`'s Target pin (its first data input).
void wireTarget(Graph& g, int src, int dst)
{
	auto dataIn0 = [&](int id) {
		const NodeSig s = signatureOf(*g.findNode(id));
		return (int)s.execIns.size() + (int)s.execOuts.size();
	};
	auto dataOut0 = [&](int id) {
		const NodeSig s = signatureOf(*g.findNode(id));
		return (int)s.execIns.size() + (int)s.execOuts.size() + (int)s.dataIns.size();
	};
	g.links.push_back({ src, dataOut0(src), dst, dataIn0(dst) });
}

HcRename::Target renameFn(const char* cls, const char* from, const char* to)
{
	return { cls, Member::Function, from, to };
}

// The caller of the sweep passes the class and everything deriving from it.
std::vector<std::string> keys(std::initializer_list<const char*> k)
{
	return std::vector<std::string>(k.begin(), k.end());
}

} // namespace

TEST_CASE("HorizonCode rename: a call is followed only when the graph can prove its target")
{
	SUBCASE("Create Object names the class, so the call is ours")
	{
		Graph g;
		const int obj  = add(g, NodeType::CreateObject, kEnemy);
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, obj, call);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		REQUIRE(p.rename.size() == 1);
		CHECK(p.unsure.empty());
		CHECK(HcRename::apply(g, p, renameFn(kEnemy, "Fire", "Ignite")));
		CHECK(g.findNode(call)->s == "Ignite");
	}

	SUBCASE("a call on a DIFFERENT class with the same function name is left alone")
	{
		// The case that makes renaming by name alone unusable: Chest also has a
		// "Fire" and this graph calls that one.
		Graph g;
		const int obj  = add(g, NodeType::CreateObject, kChest);
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, obj, call);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.empty());
		CHECK(p.unsure.empty());   // provably NOT ours: not even worth reporting
		CHECK_FALSE(HcRename::apply(g, p, renameFn(kEnemy, "Fire", "Ignite")));
		CHECK(g.findNode(call)->s == "Fire");
	}

	SUBCASE("an unwired Target is reported, never rewritten")
	{
		Graph g;
		const int call = add(g, NodeType::CallExternal, "Fire");

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.empty());
		REQUIRE(p.unsure.size() == 1);
		CHECK(p.unsure[0].node == call);
		CHECK(g.findNode(call)->s == "Fire");
	}

	SUBCASE("a Ref variable carries its class")
	{
		Graph g;
		Variable v; v.name = "Boss"; v.type = PinType::Ref; v.className = kEnemy;
		g.variables.push_back(v);
		const int get  = add(g, NodeType::GetVariable, "Boss");
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, get, call);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.size() == 1);
	}

	SUBCASE("an untyped Ref variable proves nothing")
	{
		Graph g;
		Variable v; v.name = "Thing"; v.type = PinType::Ref;   // no className
		g.variables.push_back(v);
		const int get  = add(g, NodeType::GetVariable, "Thing");
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, get, call);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.empty());
		CHECK(p.unsure.size() == 1);
	}

	SUBCASE("a call on a class DERIVING from the renamed one follows too")
	{
		// Goblin derives from Enemy, so "Fire" on a Goblin IS Enemy's function.
		// The sweep hands the descendants in; the planner just has to honour them.
		Graph g;
		const int obj  = add(g, NodeType::CreateObject, kGoblin);
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, obj, call);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy, kGoblin }),
		                                             "Level.hescene", kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.size() == 1);
	}

	SUBCASE("Get Self and Get Game Instance resolve to the keys they are given")
	{
		Graph g;
		const int self = add(g, NodeType::GetSelf);
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, self, call);
		const int gi     = add(g, NodeType::GetGameInstance);
		const int giCall = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, gi, giCall);

		const HcRename::Plan own = HcRename::planGraph(g, Role::Other, keys({ kEnemy }),
		                                              kEnemy, kGI, renameFn(kEnemy, "Fire", "Ignite"));
		REQUIRE(own.rename.size() == 1);
		CHECK(own.rename[0].node == call);       // the self-call, not the GameInstance one

		const HcRename::Plan onGi = HcRename::planGraph(g, Role::Other, keys({ kGI }),
		                                               kEnemy, kGI, renameFn(kGI, "Fire", "Ignite"));
		REQUIRE(onGi.rename.size() == 1);
		CHECK(onGi.rename[0].node == giCall);
	}

	SUBCASE("a Cast to an engine class proves nothing, a Cast to a class asset does")
	{
		Graph g;
		const int cast = add(g, NodeType::Cast, kEnemy);
		const int call = add(g, NodeType::CallExternal, "Fire");
		wireTarget(g, cast, call);
		CHECK(HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene", kGI,
		                          renameFn(kEnemy, "Fire", "Ignite")).rename.size() == 1);

		g.findNode(cast)->s = "PlayerCharacter";   // an engine taxonomy row
		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene",
		                                             kGI, renameFn(kEnemy, "Fire", "Ignite"));
		CHECK(p.rename.empty());
		CHECK(p.unsure.size() == 1);
	}

	SUBCASE("a recorded target class beats the wire")
	{
		// What the picker writes. It also covers the Target coming from somewhere
		// the walk cannot read (an engine call handing back a Ref).
		Graph g;
		const int call = add(g, NodeType::CallExternal, "Fire");
		g.findNode(call)->className = kEnemy;

		CHECK(HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene", kGI,
		                          renameFn(kEnemy, "Fire", "Ignite")).rename.size() == 1);
	}
}

TEST_CASE("HorizonCode rename: variables and events reach in through their own nodes")
{
	SUBCASE("Get and Set External follow a public variable")
	{
		Graph g;
		const int obj = add(g, NodeType::CreateObject, kEnemy);
		const int get = add(g, NodeType::GetExternal, "Health");
		const int set = add(g, NodeType::SetExternal, "Health");
		wireTarget(g, obj, get);
		wireTarget(g, obj, set);
		const int call = add(g, NodeType::CallExternal, "Health");   // a FUNCTION of that name
		wireTarget(g, obj, call);

		const HcRename::Target t{ kEnemy, Member::Variable, "Health", "Hitpoints" };
		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene", kGI, t);
		REQUIRE(p.rename.size() == 2);       // the two variable nodes
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findNode(get)->s  == "Hitpoints");
		CHECK(g.findNode(set)->s  == "Hitpoints");
		CHECK(g.findNode(call)->s == "Health");   // a same-named function is not a variable
	}

	SUBCASE("Emit Event follows, and Bind Event drags this graph's handler with it")
	{
		// Bind Event names both ends at once: when the Target fires "Died", THIS
		// graph's own "Event Died" node runs. Renaming one without the other is
		// what would quietly unhook the handler.
		Graph g;
		const int obj  = add(g, NodeType::CreateObject, kEnemy);
		const int bind = add(g, NodeType::BindEvent, "Died");
		wireTarget(g, obj, bind);
		const int handler = add(g, NodeType::Event, "Died");

		const HcRename::Target t{ kEnemy, Member::Event, "Died", "Fell" };
		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene", kGI, t);
		REQUIRE(p.rename.size() == 2);
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findNode(bind)->s    == "Fell");
		CHECK(g.findNode(handler)->s == "Fell");
	}

	SUBCASE("a subscriber with an event of its own by that name is blocked, not guessed at")
	{
		Graph g;
		EventDecl own; own.name = "Died";      // this graph raises a "Died" of its own
		g.events.push_back(own);
		const int obj  = add(g, NodeType::CreateObject, kEnemy);
		const int bind = add(g, NodeType::BindEvent, "Died");
		wireTarget(g, obj, bind);
		const int handler = add(g, NodeType::Event, "Died");

		const HcRename::Target t{ kEnemy, Member::Event, "Died", "Fell" };
		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }), "Level.hescene", kGI, t);
		CHECK(p.rename.empty());               // nothing in this graph is touched
		CHECK(p.blocked.size() == 1);
		CHECK(p.blockedWhy.size() > 0);
		CHECK_FALSE(HcRename::apply(g, p, t));
		CHECK(g.findNode(bind)->s    == "Died");
		CHECK(g.findNode(handler)->s == "Died");
	}
}

TEST_CASE("HorizonCode rename: the declaring class and an overriding one carry their own")
{
	SUBCASE("the class itself renames its declaration and everything using it")
	{
		Graph g;
		const int entry = add(g, NodeType::FunctionEntry,  "Fire");
		const int call  = add(g, NodeType::FunctionCall,   "Fire");
		const int ret   = add(g, NodeType::FunctionReturn, "Fire");

		const HcRename::Target t = renameFn(kEnemy, "Fire", "Ignite");
		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kEnemy }), kEnemy, kGI, t);
		REQUIRE(p.rename.size() == 3);
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findNode(entry)->s == "Ignite");
		CHECK(g.findNode(call)->s  == "Ignite");
		CHECK(g.findNode(ret)->s   == "Ignite");
	}

	SUBCASE("a derived class's override follows the base, or it stops overriding anything")
	{
		Graph g;
		const int entry = add(g, NodeType::FunctionEntry, "Fire");   // Goblin's override
		const HcRename::Target t = renameFn(kEnemy, "Fire", "Ignite");
		const HcRename::Plan p = HcRename::planGraph(g, Role::Overrides, keys({ kEnemy, kGoblin }), kGoblin, kGI, t);
		REQUIRE(p.rename.size() == 1);
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findNode(entry)->s == "Ignite");
	}

	SUBCASE("an unrelated graph's own function of the same name stays put")
	{
		Graph g;
		const int entry = add(g, NodeType::FunctionEntry, "Fire");
		const HcRename::Target t = renameFn(kEnemy, "Fire", "Ignite");
		const HcRename::Plan p = HcRename::planGraph(g, Role::Other, keys({ kEnemy }), kChest, kGI, t);
		CHECK(p.rename.empty());
		CHECK(g.findNode(entry)->s == "Fire");
	}

	SUBCASE("a renamed variable takes its declaration and its Get/Set nodes")
	{
		Graph g;
		Variable v; v.name = "Health"; v.type = PinType::Float;
		g.variables.push_back(v);
		const int get = add(g, NodeType::GetVariable, "Health");
		const int set = add(g, NodeType::SetVariable, "Health");

		const HcRename::Target t{ kEnemy, Member::Variable, "Health", "Hitpoints" };
		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kEnemy }), kEnemy, kGI, t);
		REQUIRE(p.rename.size() == 3);
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findVariable("Hitpoints") != nullptr);
		CHECK(g.findVariable("Health") == nullptr);
		CHECK(g.findNode(get)->s == "Hitpoints");
		CHECK(g.findNode(set)->s == "Hitpoints");
	}

	SUBCASE("a renamed event takes its declaration, its handlers and its emitters")
	{
		Graph g;
		EventDecl d; d.name = "Died";
		g.events.push_back(d);
		const int handler = add(g, NodeType::Event,     "Died");
		const int emit    = add(g, NodeType::EmitEvent, "Died");

		const HcRename::Target t{ kEnemy, Member::Event, "Died", "Fell" };
		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kEnemy }), kEnemy, kGI, t);
		REQUIRE(p.rename.size() == 3);
		CHECK(HcRename::apply(g, p, t));
		CHECK(g.findEvent("Fell") != nullptr);
		CHECK(g.findEvent("Died") == nullptr);
		CHECK(g.findNode(handler)->s == "Fell");
		CHECK(g.findNode(emit)->s    == "Fell");
	}
}

TEST_CASE("HorizonCode rename: a picked target class survives the file")
{
	Graph g;
	Node n; n.id = 1; n.type = NodeType::CallExternal; n.s = "Fire"; n.className = kEnemy;
	g.nodes.push_back(n);

	Graph back;
	REQUIRE(fromJson(toJson(g), back));
	REQUIRE(back.nodes.size() == 1);
	CHECK(back.nodes[0].className == kEnemy);

	// And a node that never picked one adds nothing to the file, so a graph
	// authored before the pickers round-trips byte for byte.
	Graph plain;
	Node p; p.id = 1; p.type = NodeType::CallExternal; p.s = "Fire";
	plain.nodes.push_back(p);
	CHECK(toJson(plain).find("className") == std::string::npos);
}
