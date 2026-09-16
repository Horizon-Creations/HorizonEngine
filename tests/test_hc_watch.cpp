#include "doctest.h"
#include "../src/HE_Editor/HcWatch.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Types/TypeRegistry.h>

#include <set>
#include <string>
#include <vector>

using namespace HorizonCode;

// ── The watch window's rows ──────────────────────────────────────────────────
// The interpreter's side (what a SuspendedRun carries) is covered in
// test_horizoncode_runtime.cpp; this is the editor module that turns a stopped
// run into named, typed, formatted rows — and the one spelling of a Value.

namespace
{
	// Print: execIn 0 / execOut 1 / Text dataIn 2; ConstString dataOut 0.
	int printNode(Graph& g, const std::string& text)
	{
		Node cs; cs.type = NodeType::ConstString; cs.s = text;
		const int c = g.addNode(cs);
		Node pr; pr.type = NodeType::Print;
		const int p = g.addNode(pr);
		REQUIRE(g.connect(c, 0, p, 2));
		return p;
	}
	void chain(Graph& g, int from, int pin, int to) { REQUIRE(g.connect(from, pin, to, 0)); }

	// A runtime that stops at every node in `bps`, like the editor's would.
	struct Rig
	{
		Runtime       rt;
		std::set<int> bps;
		Rig()
		{
			rt.setBreakPredicate([this](InstanceId, const std::string&, size_t, int node)
			{ return bps.count(node) != 0; });
		}
	};

	const HcWatch::Section* sectionOf(const HcWatch::Snapshot& s, HcWatch::Section::Kind k,
	                                  size_t nth = 0)
	{
		for (const HcWatch::Section& sec : s.sections)
			if (sec.kind == k && nth-- == 0) return &sec;
		return nullptr;
	}
	const HcWatch::Row* rowOf(const HcWatch::Section& sec, const std::string& name)
	{
		for (const HcWatch::Row& r : sec.rows) if (r.name == name) return &r;
		return nullptr;
	}

	// Go(Int) → Call F(21) → Set out = y → Print "after".
	// F: Entry x → Set l = x (l: local, default 5) → Print "inner" → Return y = x.
	struct Fixture
	{
		Graph g;
		int e = 0, fcId = 0, feId = 0, setL = 0, inner = 0, setOut = 0, after = 0;
		Fixture()
		{
			{ Variable v; v.name = "out";  v.type = PinType::Int;    g.variables.push_back(v); }
			{ Variable v; v.name = "name"; v.type = PinType::String; v.s = "hero"; g.variables.push_back(v); }
			{ Variable v; v.name = "aim";  v.type = PinType::Vec3;   v.f[0] = 1; v.f[1] = 2.5f; v.f[2] = -3;
			  g.variables.push_back(v); }
			Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F";
			fe.params = { { "x", PinType::Int } }; fe.results = { { "y", PinType::Int } };
			feId = g.addNode(fe);
			{ Variable l; l.name = "l"; l.type = PinType::Int; l.f[0] = 5; l.scope = feId; g.variables.push_back(l); }
			Node sl; sl.type = NodeType::SetVariable; sl.s = "l"; sl.propType = PinType::Int; setL = g.addNode(sl);
			inner = printNode(g, "inner");
			Node fr; fr.type = NodeType::FunctionReturn; fr.s = "F"; const int frId = g.addNode(fr);
			Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.hasArg = true; ev.propType = PinType::Int;
			e = g.addNode(ev);
			Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 21; const int c = g.addNode(ci);
			Node fc; fc.type = NodeType::FunctionCall; fc.s = "F"; fcId = g.addNode(fc);
			Node so; so.type = NodeType::SetVariable; so.s = "out"; so.propType = PinType::Int; setOut = g.addNode(so);
			after = printNode(g, "after");
			syncFunctionSignatures(g);
			chain(g, feId, 0, setL); chain(g, setL, 1, inner); chain(g, inner, 1, frId);
			REQUIRE(g.connect(feId, 1, setL, 2));      // entry.x → Set l Value
			REQUIRE(g.connect(feId, 1, frId, 1));      // entry.x → return.y
			chain(g, e, 0, fcId);
			REQUIRE(g.connect(c, 0, fcId, 2));         // 21 → x
			chain(g, fcId, 1, setOut); chain(g, setOut, 1, after);
			REQUIRE(g.connect(fcId, 3, setOut, 2));    // call.y → Set out Value
		}
	};
}

TEST_CASE("HcWatch: nothing stopped, nothing to show")
{
	Runtime rt;
	const HcWatch::Snapshot s = HcWatch::build(rt);
	CHECK_FALSE(s.valid);
	CHECK(s.sections.empty());
	CHECK(s.runCount == 0);
	CHECK(HcWatch::variableRows(rt, 999).empty());
}

TEST_CASE("HcWatch: a stop inside a function shows the event argument, the frame with its local, and the variables")
{
	Fixture f;
	Rig rig;
	rig.bps = { f.inner };
	ClassIdentity cls; cls.key = "Content/Enemies/Goblin.hasset";
	const InstanceId id = rig.rt.add(f.g, {}, cls);
	rig.rt.fireEvent(id, "Go", 0, Value::ofInt(7));
	REQUIRE(rig.rt.isSuspended());

	const HcWatch::Snapshot s = HcWatch::build(rig.rt);
	REQUIRE(s.valid);
	CHECK(s.instance == id);
	CHECK(s.classKey == "Content/Enemies/Goblin.hasset");
	CHECK(s.nodeId == f.inner);
	CHECK(s.nodeLabel == "Print");
	CHECK(s.runCount == 1);

	// Reading order: the argument, the frame, the variables. No outputs yet —
	// the call has not returned, and nothing before it produced any.
	REQUIRE(s.sections.size() == 3);
	CHECK(s.sections[0].kind == HcWatch::Section::Kind::EventArg);
	CHECK(s.sections[1].kind == HcWatch::Section::Kind::Frame);
	CHECK(s.sections[2].kind == HcWatch::Section::Kind::Variables);

	// The event argument: found by walking back from the open Call frame to
	// the Event, named after the event's data-out.
	const HcWatch::Section& arg = s.sections[0];
	CHECK(arg.title == "Event Go");
	REQUIRE(arg.rows.size() == 1);
	CHECK(arg.rows[0].type  == "Int");
	CHECK(arg.rows[0].value == "7");

	// The frame: the argument by its declared name, then the local — which
	// the Set before the stop already overwrote with x.
	const HcWatch::Section& frame = s.sections[1];
	CHECK(frame.title == "Function F");
	REQUIRE(frame.rows.size() == 2);
	CHECK(frame.rows[0].name  == "x");
	CHECK(frame.rows[0].type  == "Int");
	CHECK(frame.rows[0].value == "21");
	CHECK(frame.rows[1].name  == "l");
	CHECK(frame.rows[1].value == "21");

	// The instance's variables, sorted by name, each spelled by its type.
	const HcWatch::Section& vars = s.sections[2];
	REQUIRE(vars.rows.size() == 3);
	CHECK(vars.rows[0].name == "aim");  CHECK(vars.rows[0].type == "Vec3");   CHECK(vars.rows[0].value == "(1, 2.5, -3)");
	CHECK(vars.rows[1].name == "name"); CHECK(vars.rows[1].type == "String"); CHECK(vars.rows[1].value == "\"hero\"");
	CHECK(vars.rows[2].name == "out");  CHECK(vars.rows[2].type == "Int");    CHECK(vars.rows[2].value == "0");
	// The local is not among them.
	CHECK(rowOf(vars, "l") == nullptr);

	// The live view of the same instance says the same.
	const std::vector<HcWatch::Row> live = HcWatch::variableRows(rig.rt, id);
	REQUIRE(live.size() == 3);
	CHECK(live[1].value == "\"hero\"");

	// Continue: the run ends, the snapshot is empty again.
	rig.rt.debugContinue();
	CHECK_FALSE(HcWatch::build(rig.rt).valid);
	CHECK(rig.rt.getVariable(id, "out").i == 21);
}

TEST_CASE("HcWatch: a stop after a call shows the call's results under Outputs, and the Set already landed")
{
	Fixture f;
	Rig rig;
	rig.bps = { f.after };
	const InstanceId id = rig.rt.add(f.g);
	rig.rt.fireEvent(id, "Go", 0, Value::ofInt(7));
	REQUIRE(rig.rt.isSuspended());

	const HcWatch::Snapshot s = HcWatch::build(rig.rt);
	REQUIRE(s.valid);
	CHECK(s.nodeId == f.after);
	// The frame is gone (the function returned); the argument, the variables
	// and the outputs remain.
	CHECK(sectionOf(s, HcWatch::Section::Kind::Frame) == nullptr);
	REQUIRE(sectionOf(s, HcWatch::Section::Kind::EventArg) != nullptr);
	CHECK(sectionOf(s, HcWatch::Section::Kind::EventArg)->rows[0].value == "7");

	const HcWatch::Section* vars = sectionOf(s, HcWatch::Section::Kind::Variables);
	REQUIRE(vars != nullptr);
	REQUIRE(rowOf(*vars, "out") != nullptr);
	CHECK(rowOf(*vars, "out")->value == "21");

	const HcWatch::Section* outs = sectionOf(s, HcWatch::Section::Kind::Outputs);
	REQUIRE(outs != nullptr);
	REQUIRE(outs->rows.size() == 1);
	CHECK(outs->rows[0].name  == "Call Function F > y");
	CHECK(outs->rows[0].type  == "Int");
	CHECK(outs->rows[0].value == "21");
}

TEST_CASE("HcWatch: two stopped runs — the index picks, runCount counts")
{
	// Two instances of the same class, both stopped at the same node.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p = printNode(g, "p");
	chain(g, e, 0, p);
	{ Variable v; v.name = "n"; v.type = PinType::Int; g.variables.push_back(v); }

	Rig rig;
	rig.bps = { p };
	const InstanceId a = rig.rt.add(g), b = rig.rt.add(g);
	rig.rt.setVariable(a, "n", Value::ofInt(1));
	rig.rt.setVariable(b, "n", Value::ofInt(2));
	rig.rt.fireEvent(a, "Go");
	rig.rt.fireEvent(b, "Go");
	REQUIRE(rig.rt.suspendedSites().size() == 2);

	const HcWatch::Snapshot first = HcWatch::build(rig.rt, 0), second = HcWatch::build(rig.rt, 1);
	REQUIRE(first.valid); REQUIRE(second.valid);
	CHECK(first.runCount == 2);
	CHECK(first.instance == a);
	CHECK(second.instance == b);
	CHECK(rowOf(*sectionOf(first,  HcWatch::Section::Kind::Variables), "n")->value == "1");
	CHECK(rowOf(*sectionOf(second, HcWatch::Section::Kind::Variables), "n")->value == "2");
	// An event without an argument has no argument section.
	CHECK(sectionOf(first, HcWatch::Section::Kind::EventArg) == nullptr);
	CHECK_FALSE(HcWatch::build(rig.rt, 2).valid);
	rig.rt.debugAbort();
}

TEST_CASE("HcWatch: one spelling per Value")
{
	CHECK(HcWatch::formatValue(Value::ofFloat(1.5f))    == "1.5");
	CHECK(HcWatch::formatValue(Value::ofFloat(100.0f))  == "100");
	CHECK(HcWatch::formatValue(Value::ofBool(true))     == "true");
	CHECK(HcWatch::formatValue(Value::ofInt(-4))        == "-4");
	CHECK(HcWatch::formatValue(Value::ofString("a\"b\nc")) == "\"a\\\"b\\nc\"");
	CHECK(HcWatch::formatValue(Value::ofVec2({ 1, 2 }))  == "(1, 2)");
	CHECK(HcWatch::formatValue(Value::ofVec4({ 1, 2, 3, 4 })) == "(1, 2, 3, 4)");
	CHECK(HcWatch::formatValue(Value::ofColor({ 1, 0.5f, 0, 1 })) == "(1, 0.5, 0, 1)");
	CHECK(HcWatch::formatValue(Value::ofTransform({ 1, 2, 3 }, { 0, 90, 0 }, { 1, 1, 1 }))
	      == "pos (1, 2, 3) rot (0, 90, 0) scale (1, 1, 1)");
	CHECK(HcWatch::typeLabel(Value::ofRef(0)) == "Object");

	// References: none, bare (no runtime), a live one by its class, a dead one.
	CHECK(HcWatch::formatValue(Value::ofRef(0))  == "none");
	CHECK(HcWatch::formatValue(Value::ofRef(12)) == "#12");
	{
		Runtime rt;
		Graph g;
		ClassIdentity cls; cls.key = "Content/Enemies/Goblin.hasset";
		const InstanceId id = rt.add(g, {}, cls);
		CHECK(HcWatch::formatValue(Value::ofRef(id), &rt) == "Goblin #" + std::to_string(id));
		CHECK(HcWatch::formatValue(Value::ofRef(id + 100), &rt) == "#" + std::to_string(id + 100) + " (destroyed)");
	}
	CHECK(HcWatch::classLabel("__game_instance__") == "Game Instance");
	CHECK(HcWatch::classLabel("level:0123abcd")    == "Level Script");
	CHECK(HcWatch::classLabel("UI/Menu.hasset")    == "Menu");
	CHECK(HcWatch::classLabel("").empty());

	// Containers: the count, then the elements — cut after kMaxItems.
	{
		Value arr = Value::ofArray(PinType::Int);
		for (int i = 1; i <= 3; ++i) arr.items.push_back(Value::ofInt(i));
		CHECK(HcWatch::formatValue(arr) == "[3] {1, 2, 3}");
		CHECK(HcWatch::typeLabel(arr)   == "Array of Int");
		Value big = Value::ofArray(PinType::Int);
		for (int i = 0; i < 20; ++i) big.items.push_back(Value::ofInt(i));
		CHECK(HcWatch::formatValue(big) == "[20] {0, 1, 2, 3, 4, 5, 6, 7, …}");
		Value set = Value::ofSet(PinType::String);
		set.items.push_back(Value::ofString("a"));
		CHECK(HcWatch::formatValue(set) == "[1] {\"a\"}");
		CHECK(HcWatch::typeLabel(set)   == "Set of String");
		Value map = Value::ofMap(PinType::String, PinType::Float);
		map.keys.push_back(Value::ofString("hp")); map.items.push_back(Value::ofFloat(10));
		map.keys.push_back(Value::ofString("mp")); map.items.push_back(Value::ofFloat(2.5f));
		CHECK(HcWatch::formatValue(map) == "[2] {\"hp\": 10, \"mp\": 2.5}");
		CHECK(HcWatch::typeLabel(map)   == "Map of String to Float");
	}

	// Enums by entry name, structs by field name — when the definition is
	// registered; the raw shape otherwise.
	{
		HE::EnumDef ed; ed.name = "State"; ed.assetPath = "Types/State.hasset";
		ed.entries = { { "Idle", 0 }, { "Run", 1 } };
		HE::TypeRegistry::instance().registerEnum(ed);
		HE::StructDef sd; sd.name = "Stats"; sd.assetPath = "Types/Stats.hasset";
		HE::StructField hp; hp.name = "hp"; hp.type = PinType::Int;
		HE::StructField st; st.name = "state"; st.type = PinType::Enum; st.typeName = "Types/State.hasset";
		sd.fields = { hp, st };
		HE::TypeRegistry::instance().registerStruct(sd);

		Value en; en.type = PinType::Enum; en.typeName = "Types/State.hasset"; en.i = 1;
		CHECK(HcWatch::formatValue(en) == "Run");
		CHECK(HcWatch::typeLabel(en)   == "State");
		en.i = 7;
		CHECK(HcWatch::formatValue(en) == "7");                 // no such entry: the number

		Value s; s.type = PinType::Struct; s.typeName = "Types/Stats.hasset";
		s.items.push_back(Value::ofInt(10));
		s.items.push_back(en); s.items.back().i = 0;
		CHECK(HcWatch::formatValue(s) == "{hp: 10, state: Idle}");
		CHECK(HcWatch::typeLabel(s)   == "Stats");

		HE::TypeRegistry::instance().removeType("Types/Stats.hasset");
		HE::TypeRegistry::instance().removeType("Types/State.hasset");
		CHECK(HcWatch::formatValue(s)  == "{10, 0}");            // unregistered: positional
		CHECK(HcWatch::formatValue(en) == "7");
	}
}
