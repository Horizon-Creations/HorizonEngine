#include "doctest.h"

#include "HcRename.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>   // the animation rows, asked for rather than retyped

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

// ── Renaming a widget's animation ────────────────────────────────────────────
// The fourth member kind, and the one shaped differently: a clip's name is not
// on the node, it is a VALUE in the "animation" parameter of an engine row —
// inline in the pin, or in a Const String wired to it. Everything else about the
// rename is the same problem and the same contract: rewrite what the graph can
// prove is this widget's clip, report what names it and cannot be proved, and
// leave provably-somebody-else's alone without a word.

namespace
{
constexpr const char* kCard  = "Widgets/Card.hasset";
constexpr const char* kOther = "Widgets/Other.hasset";

// A Play Animation node as the editor builds one: the engine row's parameter
// list, and the clip's name in the "animation" pin's inline default. Asking the
// registry for the parameters is the point — the rename asks it too, so a test
// that hardcoded them could pass while the two disagreed.
int animPinOf(const Node& n)
{
	const NodeSig s = signatureOf(n);
	const int dataIn0 = (int)s.execIns.size() + (int)s.execOuts.size();
	for (size_t i = 0; i < s.dataIns.size(); ++i)
		if (std::string(s.dataIns[i].name) == "animation") return dataIn0 + (int)i;
	return -1;
}

int addPlay(Graph& g, const std::string& clip,
            const char* row = "widget.playAnimation")
{
	const HE::api::ApiFn* fn = HE::api::find(row);
	REQUIRE(fn != nullptr);
	Node n;
	n.id = (int)g.nodes.size() + 1;
	n.type = NodeType::EngineCall;
	n.s = row;
	n.hasArg = true;
	for (const auto& p : fn->params)  n.params.push_back({ p.name, p.type });
	for (const auto& r : fn->results) n.results.push_back({ r.name, r.type });
	const int pin = animPinOf(n);
	REQUIRE(pin >= 0);
	if (!clip.empty()) n.pinDefaults[pin] = Value::ofString(clip);
	g.nodes.push_back(n);
	return n.id;
}

std::string playedName(const Graph& g, int nodeId)
{
	const Node* n = g.findNode(nodeId);
	if (!n) return {};
	const auto it = n->pinDefaults.find(animPinOf(*n));
	return it == n->pinDefaults.end() ? std::string{} : it->second.s;
}

HcRename::Target renameAnim(const char* from, const char* to)
{
	return { kCard, Member::Animation, from, to };
}
} // namespace

TEST_CASE("Animation rename: a widget's own Play node follows it")
{
	Graph g;
	// Target left unwired, which on this row means "the widget running me" —
	// the registry's selfDefault. That is the normal shape and it is provable.
	const int play = addPlay(g, "Fade");
	const int stop = addPlay(g, "Fade", "widget.stopAnimationClip");
	// …and one playing something else, which must not move.
	const int other = addPlay(g, "Slide");

	const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
	                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
	CHECK(p.rename.size() == 2);
	CHECK(p.unsure.empty());
	CHECK(HcRename::apply(g, p, renameAnim("Fade", "FadeIn")));
	CHECK(playedName(g, play)  == "FadeIn");
	CHECK(playedName(g, stop)  == "FadeIn");     // Stop names one too
	CHECK(playedName(g, other) == "Slide");
}

TEST_CASE("Animation rename: whose widget it is decides, and an unproven one is only reported")
{
	SUBCASE("provably another widget's clip of the same name is left alone, silently")
	{
		Graph g;
		const int obj  = add(g, NodeType::CreateObject, kOther);
		const int play = addPlay(g, "Fade");
		wireTarget(g, obj, play);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
		                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
		CHECK(p.rename.empty());
		// Not even a warning: every widget in a project is allowed a "Fade", and
		// reporting each of them would bury the lines that matter.
		CHECK(p.unsure.empty());
	}

	SUBCASE("a widget this graph was handed cannot be proved, so it is reported")
	{
		Graph g;
		// A Ref that arrives from outside: no Create Object, no Get Self, and no
		// variable declaring its class.
		const int src  = add(g, NodeType::GetVariable, "Target");
		g.findNode(src)->propType = PinType::Ref;
		const int play = addPlay(g, "Fade");
		wireTarget(g, src, play);

		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
		                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
		CHECK(p.rename.empty());
		REQUIRE(p.unsure.size() == 1);
		CHECK_FALSE(HcRename::apply(g, p, renameAnim("Fade", "FadeIn")));
		CHECK(playedName(g, play) == "Fade");    // reported, not rewritten
	}
}

TEST_CASE("Animation rename: a shared Const String is one string, so it is counted not judged")
{
	SUBCASE("feeding only animation pins of ours, it is rewritten once")
	{
		Graph g;
		const int lit  = add(g, NodeType::ConstString, "Fade");
		const int play = addPlay(g, {});
		const int stop = addPlay(g, {}, "widget.stopAnimationClip");
		g.links.push_back({ lit, 0, play, animPinOf(*g.findNode(play)) });
		g.links.push_back({ lit, 0, stop, animPinOf(*g.findNode(stop)) });

		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
		                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
		REQUIRE(p.rename.size() == 1);           // the literal, once, not twice
		CHECK(p.unsure.empty());
		CHECK(HcRename::apply(g, p, renameAnim("Fade", "FadeIn")));
		CHECK(g.findNode(lit)->s == "FadeIn");
	}

	SUBCASE("shared with something else, it is reported and left standing")
	{
		Graph g;
		const int lit  = add(g, NodeType::ConstString, "Fade");
		const int play = addPlay(g, {});
		g.links.push_back({ lit, 0, play, animPinOf(*g.findNode(play)) });
		// The same words, used as words: a label somewhere on the widget. There
		// is one string, so rewriting it to fix the graph would change what the
		// screen says.
		const int text = add(g, NodeType::SetProperty, "Text");
		g.findNode(text)->propType = PinType::String;
		g.findNode(text)->elem = 4;
		g.links.push_back({ lit, 0, text, 2 });

		const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
		                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
		CHECK(p.rename.empty());
		REQUIRE(p.unsure.size() == 1);
		CHECK_FALSE(HcRename::apply(g, p, renameAnim("Fade", "FadeIn")));
		CHECK(g.findNode(lit)->s == "Fade");
	}
}

TEST_CASE("Animation rename: a name this graph cannot read is not evidence of anything")
{
	// The clip name arrives through a variable. It might be "Fade" at runtime and
	// it might not; either way nothing here can tell, and a rename that guessed
	// would be the eager kind this whole file exists to prevent.
	Graph g;
	const int var  = add(g, NodeType::GetVariable, "Which");
	g.findNode(var)->propType = PinType::String;
	const int play = addPlay(g, {});
	g.links.push_back({ var, 0, play, animPinOf(*g.findNode(play)) });

	const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
	                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
	CHECK(p.rename.empty());
	CHECK(p.unsure.empty());
}

// The parameter is found by NAME, and ApiParam::name is a const char* — so the
// comparison has to be a string one. It was == for a while, which compares
// pointers, and it worked for exactly as long as the linker folded both literals
// into the same address. This pins the answer to the value instead.
TEST_CASE("Animation rename: the animation parameter is found by its name, not by its address")
{
	Graph g;
	const int play = addPlay(g, "Fade");
	const int pin  = animPinOf(*g.findNode(play));
	REQUIRE(pin >= 0);
	const HE::api::ApiFn* fn = HE::api::find("widget.playAnimation");
	REQUIRE(fn != nullptr);
	// Whatever index the row's parameters end up in, the one the rename writes to
	// is the one CALLED "animation" — the test asks the registry the same way.
	const NodeSig s = signatureOf(*g.findNode(play));
	const int dataIn0 = (int)s.execIns.size() + (int)s.execOuts.size();
	CHECK(std::string(fn->params[(size_t)(pin - dataIn0)].name) == "animation");

	const HcRename::Plan p = HcRename::planGraph(g, Role::Declares, keys({ kCard }),
	                                             kCard, kGI, renameAnim("Fade", "FadeIn"));
	REQUIRE(p.rename.size() == 1);
	CHECK(HcRename::apply(g, p, renameAnim("Fade", "FadeIn")));
	CHECK(playedName(g, play) == "FadeIn");
}
