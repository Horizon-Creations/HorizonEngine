#include "doctest.h"

#include "McpToolRegistry.h"
#include "CollabDocSync.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <string>
#include <vector>

// ─── Authoring HorizonCode from outside the editor ───────────────────────────
// The claim these tools make is that they never write into a graph by hand —
// every node and every variable goes through the item-level JSON HorizonCode
// itself writes, applied by the CollabDocSync adapter. A claim like that is only
// worth what a test makes of it, so the questions here are the ones that break
// the moment a handler takes a shortcut:
//
//   • does an edit come back out of the collaboration diff as a delta at all —
//     that is the whole reason for going through the adapter, and the one thing
//     a handler that wrote `g.nodes.push_back(…)` would silently lose,
//   • does a connect by pin NAME land on the pin the client meant, on the side
//     it meant it — exec pins carry no label of their own, so 'exec' has to be
//     resolved per side, and getting that wrong wires two outputs together and
//     reports success,
//   • does a refused connect leave the graph untouched, byte for byte,
//   • is a pin default keyed by the DATA-IN index rather than the unified one,
//   • does the answer say what it silently did — the wire a connect replaced,
//     the links a delete took with it.

using HE::Ed::McpHcDoc;
using HE::Ed::McpHcHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace HC = HorizonCode;

namespace {

// The editor, minus the editor: one graph, and counters for the four things the
// tools ask of the application. Nothing here has a panel or a window, which is
// the entire reason these tools take hooks instead of an EditorApplication.
struct Fixture
{
	HC::Graph   graph;
	McpHcDoc    doc;
	int         begins = 0, ends = 0, saves = 0;
	bool        playing = false;
	bool        saveWorks = true;

	McpToolRegistry registry;

	Fixture()
	{
		doc.key   = HE::Ed::kMcpDocLevelScript;
		doc.label = "Level Script";
		doc.kind  = "level";

		McpHcHooks h;
		h.documents = [this] { return std::vector<McpHcDoc>{ doc }; };
		h.resolve   = [this](const std::string& key) -> HC::Graph* {
			return key == doc.key ? &graph : nullptr;
		};
		h.beginEdit = [this](const std::string&) { ++begins; };
		h.endEdit   = [this](const std::string&) { ++ends; };
		h.save      = [this](const std::string&) { ++saves; return saveWorks; };
		h.isPlaying = [this] { return playing; };
		HE::Ed::registerHcTools(registry, std::move(h));
	}

	ToolResult call(const char* name, json args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		if (!args.contains("key")) args["key"] = doc.key;
		return t->handler(args);
	}

	// Every mutating tool takes the same first argument, so the "which document"
	// half of a call is worth writing once.
	json ok(const char* name, json args = json::object())
	{
		const ToolResult r = call(name, std::move(args));
		INFO(name << " failed: " << r.errorCode << " — " << r.errorMessage);
		REQUIRE_FALSE(r.isError);
		return r.content;
	}

	int addNode(const char* type, const char* s = "", int x = 0, int y = 0)
	{
		json args{ { "type", type }, { "position", json::array({ x, y }) } };
		if (s && s[0]) args["s"] = s;
		return ok("hc_add_node", args)["node"]["id"].get<int>();
	}
};

// The pins of a node exactly as `signatureOf` orders them — the common source
// both this file and HcGraphHost::nodePins derive from. Comparing the tool's
// answer against HcGraphHost directly is not possible here (that translation
// unit is ImGui and is not in this binary), and would be the wrong comparison
// anyway: the invariant is that both agree with the signature, not with each
// other.
std::vector<std::string> signaturePinLabels(const HC::Node& n)
{
	const HC::NodeSig s = HC::signatureOf(n);
	std::vector<std::string> out;
	for (const auto& p : s.execIns)  out.push_back(p.name ? p.name : "");
	for (const auto& p : s.execOuts) out.push_back(p.name ? p.name : "");
	for (const auto& p : s.dataIns)  out.push_back(p.name ? p.name : "");
	for (const auto& p : s.dataOuts) out.push_back(p.name ? p.name : "");
	return out;
}

// The first engine-API row that takes at least one parameter and runs on exec —
// picked from the registry rather than named, so a renamed row does not turn
// this file red for a reason that has nothing to do with MCP.
const HE::api::ApiFn* someExecApiRow()
{
	for (const HE::api::ApiFn& fn : HE::api::registry())
		if (fn.isExec && !fn.params.empty()) return &fn;
	return nullptr;
}

} // namespace

TEST_CASE("hc tools: every tool is registered under a name a client may call")
{
	Fixture f;
	for (const char* name : { "hc_documents", "hc_get", "hc_node_types", "hc_add_node",
	                          "hc_set_node", "hc_remove_node", "hc_connect",
	                          "hc_disconnect", "hc_set_pin_default", "hc_add_variable",
	                          "hc_set_variable", "hc_remove_variable", "hc_save" })
	{
		const McpTool* t = f.registry.find(name);
		INFO("missing tool: " << name);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK_FALSE(t->description.empty());
		CHECK(t->inputSchema.is_object());
	}
	// Reading tools are not logged and not refused during play; mutating ones
	// are both. Getting this backwards is invisible until someone edits a graph
	// in play mode and loses it.
	CHECK_FALSE(f.registry.find("hc_get")->mutates);
	CHECK_FALSE(f.registry.find("hc_documents")->mutates);
	CHECK(f.registry.find("hc_add_node")->mutates);
	CHECK(f.registry.find("hc_connect")->mutates);
	CHECK(f.registry.find("hc_save")->mutates);
}

TEST_CASE("hc tools: an unknown document is not_found, and play mode refuses a write")
{
	Fixture f;

	const ToolResult unknown = f.call("hc_get", json{ { "key", "no/such/thing.hasset" } });
	CHECK(unknown.isError);
	CHECK(unknown.errorCode == "not_found");

	f.playing = true;
	const ToolResult refused = f.call("hc_add_node", json{ { "type", "Branch" } });
	CHECK(refused.isError);
	CHECK(refused.errorCode == "play_mode");
	CHECK(f.graph.nodes.empty());
	// Reading is untouched: a client that cannot look while the human plays
	// cannot prepare the edit it will make when play stops.
	CHECK_FALSE(f.call("hc_get").isError);
}

TEST_CASE("hc tools: add a node, and the answer carries the pins nothing else can supply")
{
	Fixture f;
	const json added = f.ok("hc_add_node", json{
		{ "type", "Branch" }, { "position", json::array({ 120, 40 }) } });

	REQUIRE(f.graph.nodes.size() == 1);
	const HC::Node& n = f.graph.nodes[0];
	CHECK(n.type == HC::NodeType::Branch);
	CHECK(n.x == doctest::Approx(120.0f));
	CHECK(added["node"]["id"].get<int>() == n.id);
	CHECK(f.begins == 1);
	CHECK(f.ends == 1);

	// Parity with the signature, which is where every other reader of a node's
	// pins gets them too.
	const std::vector<std::string> want = signaturePinLabels(n);
	const json& pins = added["node"]["pins"];
	REQUIRE(pins.size() == want.size());
	for (std::size_t i = 0; i < want.size(); ++i)
	{
		CHECK(pins[i]["label"].get<std::string>() == want[i]);
		CHECK(pins[i]["index"].get<int>() == static_cast<int>(i));
	}

	// The id has to be minted out of the graph's own counter, or the next node
	// added by a human collides with it.
	CHECK(f.graph.nextId > n.id);
}

TEST_CASE("hc tools: an unknown node type is refused, and an excluded one too")
{
	Fixture f;
	const ToolResult bad = f.call("hc_add_node", json{ { "type", "Teleporter" } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "invalid_payload");
	CHECK(f.graph.nodes.empty());

	// A frontend that hides a type from its add menu must not get it through
	// this door either — the restriction would otherwise be cosmetic.
	f.doc.excludedNodeTypes = { HC::nodeDisplayName(HC::NodeType::Delay) };
	const ToolResult hidden = f.call("hc_add_node", json{
		{ "type", HC::nodeDisplayName(HC::NodeType::Delay) } });
	CHECK(hidden.isError);
	CHECK(hidden.errorCode == "refused_by_policy");
	CHECK(f.graph.nodes.empty());

	// And hc_node_types does not offer it, so a well-behaved client never asks.
	const json types = f.ok("hc_node_types");
	for (const json& t : types["nodeTypes"])
		CHECK(t["type"].get<std::string>() != HC::nodeDisplayName(HC::NodeType::Delay));
}

TEST_CASE("hc tools: an Engine Call node mirrors the registry row, and a bad id is refused")
{
	const HE::api::ApiFn* row = someExecApiRow();
	REQUIRE(row != nullptr);

	Fixture f;
	const json added = f.ok("hc_add_node", json{
		{ "type", HC::nodeDisplayName(HC::NodeType::EngineCall) }, { "s", row->id } });

	REQUIRE(f.graph.nodes.size() == 1);
	const HC::Node& n = f.graph.nodes[0];
	CHECK(n.s == row->id);
	// `hasArg` IS the descriptor's isExec (see the NodeType::EngineCall comment):
	// get it wrong and the node has no exec pins at all.
	CHECK(n.hasArg == row->isExec);
	REQUIRE(n.params.size() == row->params.size());
	for (std::size_t i = 0; i < n.params.size(); ++i)
	{
		CHECK(n.params[i].name == std::string(row->params[i].name));
		CHECK(n.params[i].type == row->params[i].type);
	}
	CHECK(added["node"]["pins"].size() == signaturePinLabels(n).size());

	const ToolResult bad = f.call("hc_add_node", json{
		{ "type", HC::nodeDisplayName(HC::NodeType::EngineCall) },
		{ "s", "no.such.registry.row" } });
	CHECK(bad.isError);
	CHECK(bad.errorCode == "invalid_payload");
	CHECK(f.graph.nodes.size() == 1);

	// A document whose frontend only allows another group refuses the row.
	f.doc.apiGroups = { "definitely_not_a_group" };
	const ToolResult outside = f.call("hc_add_node", json{
		{ "type", HC::nodeDisplayName(HC::NodeType::EngineCall) }, { "s", row->id } });
	CHECK(outside.isError);
	CHECK(outside.errorCode == "refused_by_policy");
	CHECK(f.graph.nodes.size() == 1);
}

TEST_CASE("hc tools: connect by pin label resolves on the side the end needs")
{
	Fixture f;
	const int ev = f.addNode("Event", "OnLevelLoaded");
	const int br = f.addNode("Branch");

	// Exec pins carry no label at all (signatureOf gives them ""), so 'exec' is
	// the word that stands in for "the one exec pin on this side" — and it means
	// the Event's OUT on one end and the Branch's IN on the other. A resolver
	// that searched one flat list would wire the wrong pair and report success.
	const json r = f.ok("hc_connect", json{
		{ "srcNode", ev }, { "srcPin", "exec" },
		{ "dstNode", br }, { "dstPin", "exec" } });

	REQUIRE(f.graph.links.size() == 1);
	const HC::Link& l = f.graph.links[0];
	CHECK(l.srcNode == ev);
	CHECK(l.dstNode == br);
	CHECK(r["viaConversion"].get<bool>() == false);
	CHECK(r["link"]["srcPin"].get<int>() == l.srcPin);

	// The source's "exec" has to be an OUTPUT of the Event node.
	const HC::Node* evn = f.graph.findNode(ev);
	REQUIRE(evn != nullptr);
	const HC::NodeSig es = HC::signatureOf(*evn);
	CHECK(l.srcPin >= static_cast<int>(es.execIns.size()));
	CHECK(l.srcPin <  static_cast<int>(es.execIns.size() + es.execOuts.size()));

	// Naming an output where an input belongs is a refusal, not a guess.
	const ToolResult wrongWay = f.call("hc_connect", json{
		{ "srcNode", br }, { "srcPin", 0 },        // Branch's exec IN
		{ "dstNode", ev }, { "dstPin", 0 } });
	CHECK(wrongWay.isError);
	CHECK(wrongWay.errorCode == "invalid_payload");
	CHECK(f.graph.links.size() == 1);
}

TEST_CASE("hc tools: a connect that cannot be made leaves the graph exactly as it was")
{
	Fixture f;
	const int a = f.addNode("Print");
	const int b = f.addNode("Print");
	const std::string before = HC::toJson(f.graph);

	// Two exec INPUTS. There is no conversion node for that, and there must not
	// be a half-built one left behind either.
	const ToolResult r = f.call("hc_connect", json{
		{ "srcNode", a }, { "srcPin", 0 },
		{ "dstNode", b }, { "dstPin", 0 } });
	CHECK(r.isError);
	CHECK(HC::toJson(f.graph) == before);
}

TEST_CASE("hc tools: a type mismatch is bridged by the same conversion node the canvas spawns")
{
	Fixture f;
	const int num = f.addNode("Float");          // ConstFloat: one Float data-out
	const int cat = f.addNode("Concat");         // two String data-ins

	const HC::Node* n = f.graph.findNode(num);
	const HC::Node* c = f.graph.findNode(cat);
	REQUIRE(n != nullptr);
	REQUIRE(c != nullptr);
	const HC::NodeSig ns = HC::signatureOf(*n);
	const HC::NodeSig cs = HC::signatureOf(*c);
	REQUIRE(ns.dataOuts.size() >= 1);
	REQUIRE(cs.dataIns.size() >= 1);
	const int srcPin = static_cast<int>(ns.execIns.size() + ns.execOuts.size() +
	                                    ns.dataIns.size());
	const int dstPin = static_cast<int>(cs.execIns.size() + cs.execOuts.size());

	const json r = f.ok("hc_connect", json{
		{ "srcNode", num }, { "srcPin", srcPin },
		{ "dstNode", cat }, { "dstPin", dstPin } });

	CHECK(r["viaConversion"].get<bool>());
	// The spawned node is NAMED, not left for the client to find in the next
	// hc_get — a graph that grew a node nobody was told about is a graph the
	// client's model of it no longer matches.
	REQUIRE(r["spawnedNodes"].size() == 1);
	const int convId = r["spawnedNodes"][0]["id"].get<int>();
	CHECK(f.graph.findNode(convId) != nullptr);
	CHECK(f.graph.links.size() == 2);

	// With the fallback switched off the same call is a plain refusal.
	Fixture g;
	const int n2 = g.addNode("Float");
	const int c2 = g.addNode("Concat");
	const std::string before = HC::toJson(g.graph);
	const ToolResult refused = g.call("hc_connect", json{
		{ "srcNode", n2 }, { "srcPin", srcPin },
		{ "dstNode", c2 }, { "dstPin", dstPin },
		{ "allowConversion", false } });
	CHECK(refused.isError);
	CHECK(refused.errorCode == "failed");
	CHECK(HC::toJson(g.graph) == before);
}

TEST_CASE("hc tools: replacing an occupied exec output reports the wire it took away")
{
	Fixture f;
	const int ev  = f.addNode("Event", "OnLevelLoaded");
	const int br1 = f.addNode("Branch");
	const int br2 = f.addNode("Branch");

	// It is the SOURCE side that gets replaced for exec: an exec output is a
	// single "what runs next" pointer, while an exec input takes as many wires
	// as want to arrive at it (Graph::connect, and the note beside it).
	f.ok("hc_connect", json{ { "srcNode", ev }, { "srcPin", "exec" },
	                         { "dstNode", br1 }, { "dstPin", "exec" } });
	const json second = f.ok("hc_connect", json{
		{ "srcNode", ev }, { "srcPin", "exec" },
		{ "dstNode", br2 }, { "dstPin", "exec" } });

	// The canvas is silent about this because you watch the old wire vanish. A
	// client cannot watch anything.
	REQUIRE(second["replacedLinks"].size() == 1);
	CHECK(second["replacedLinks"][0]["dstNode"].get<int>() == br1);
	REQUIRE(f.graph.links.size() == 1);
	CHECK(f.graph.links[0].dstNode == br2);

	// Two wires INTO one exec input, on the other hand, both survive — replacing
	// there would silently delete a branch of the graph.
	f.ok("hc_connect", json{ { "srcNode", br1 }, { "srcPin", "True" },
	                         { "dstNode", br2 }, { "dstPin", "exec" } });
	CHECK(f.graph.links.size() == 2);
}

TEST_CASE("hc tools: disconnect names both ends, and removing a node takes its links")
{
	Fixture f;
	const int ev = f.addNode("Event", "OnLevelLoaded");
	const int br = f.addNode("Branch");
	f.ok("hc_connect", json{ { "srcNode", ev }, { "srcPin", "exec" },
	                         { "dstNode", br }, { "dstPin", "exec" } });
	const int srcPin = f.graph.links[0].srcPin;
	const int dstPin = f.graph.links[0].dstPin;

	// A wire that is not there is not_found rather than a silent success.
	const ToolResult missing = f.call("hc_disconnect", json{
		{ "srcNode", ev }, { "srcPin", srcPin },
		{ "dstNode", br }, { "dstPin", dstPin + 1 } });
	CHECK(missing.isError);

	f.ok("hc_disconnect", json{ { "srcNode", ev }, { "srcPin", srcPin },
	                            { "dstNode", br }, { "dstPin", dstPin } });
	CHECK(f.graph.links.empty());

	f.ok("hc_connect", json{ { "srcNode", ev }, { "srcPin", "exec" },
	                         { "dstNode", br }, { "dstPin", "exec" } });
	const json removed = f.ok("hc_remove_node", json{ { "id", br } });
	CHECK(f.graph.findNode(br) == nullptr);
	CHECK(f.graph.links.empty());
	REQUIRE(removed["removedLinks"].size() == 1);
	CHECK(removed["removedLinks"][0]["dstNode"].get<int>() == br);

	CHECK(f.call("hc_remove_node", json{ { "id", 9999 } }).errorCode == "not_found");
}

TEST_CASE("hc tools: a pin default is keyed by the data-in index, not the unified one")
{
	Fixture f;
	// Branch has an exec IN before its Condition, so the two indices differ —
	// which is the whole point. A handler that stored the unified index would
	// write a default the interpreter never reads.
	const int br = f.addNode("Branch");
	const HC::Node* n = f.graph.findNode(br);
	REQUIRE(n != nullptr);
	const HC::NodeSig s = HC::signatureOf(*n);
	const int dataIn0 = static_cast<int>(s.execIns.size() + s.execOuts.size());
	REQUIRE(dataIn0 > 0);
	REQUIRE(s.dataIns.size() >= 1);

	const json r = f.ok("hc_set_pin_default", json{
		{ "node", br }, { "pin", dataIn0 }, { "value", true } });
	CHECK(r["dataIndex"].get<int>() == 0);

	const HC::Node* after = f.graph.findNode(br);
	REQUIRE(after != nullptr);
	const auto it = after->pinDefaults.find(0);
	REQUIRE(it != after->pinDefaults.end());
	CHECK(it->second.b == true);
	CHECK(after->pinDefaults.count(dataIn0) == 0);

	// It survives the item JSON — the payload is the only thing that travels, so
	// a field the serializer drops is a field the peer never sees.
	HC::Node roundTrip;
	REQUIRE(HC::nodeFromJson(HC::nodeToJson(*after), roundTrip));
	CHECK(roundTrip.pinDefaults.count(0) == 1);

	// An exec pin has no value to default.
	CHECK(f.call("hc_set_pin_default", json{
		{ "node", br }, { "pin", 0 }, { "value", true } }).isError);

	// A wired pin ignores its default, so writing one there is a lie the client
	// cannot detect. Refused instead.
	const int cond = f.addNode("Bool");
	const HC::Node* cn = f.graph.findNode(cond);
	REQUIRE(cn != nullptr);
	const HC::NodeSig cs = HC::signatureOf(*cn);
	const int condOut = static_cast<int>(cs.execIns.size() + cs.execOuts.size() +
	                                     cs.dataIns.size());
	f.ok("hc_connect", json{ { "srcNode", cond }, { "srcPin", condOut },
	                         { "dstNode", br },   { "dstPin", dataIn0 } });
	const ToolResult wired = f.call("hc_set_pin_default", json{
		{ "node", br }, { "pin", dataIn0 }, { "value", false } });
	CHECK(wired.isError);
	CHECK(wired.errorCode == "failed");

	// Clearing takes it away again.
	f.ok("hc_disconnect", json{ { "srcNode", cond }, { "srcPin", condOut },
	                            { "dstNode", br },   { "dstPin", dataIn0 } });
	f.ok("hc_set_pin_default", json{ { "node", br }, { "pin", dataIn0 },
	                                 { "value", nullptr } });
	CHECK(f.graph.findNode(br)->pinDefaults.empty());
}

TEST_CASE("hc tools: a pin default has to match the pin's type")
{
	Fixture f;
	const int br = f.addNode("Branch");
	const HC::NodeSig s = HC::signatureOf(*f.graph.findNode(br));
	const int cond = static_cast<int>(s.execIns.size() + s.execOuts.size());

	const ToolResult wrong = f.call("hc_set_pin_default", json{
		{ "node", br }, { "pin", cond }, { "value", "yes please" } });
	CHECK(wrong.isError);
	CHECK(wrong.errorCode == "invalid_payload");
	CHECK(f.graph.findNode(br)->pinDefaults.empty());
}

TEST_CASE("hc tools: variables are added, replaced, removed — and named by name")
{
	Fixture f;
	f.ok("hc_add_variable", json{ { "name", "Score" }, { "type", "Int" } });
	REQUIRE(f.graph.variables.size() == 1);
	CHECK(f.graph.variables[0].type == HC::PinType::Int);

	// The name is the identity everywhere in the graph, so a second one under
	// the same name is a refusal rather than a silent overwrite.
	const ToolResult again = f.call("hc_add_variable", json{
		{ "name", "Score" }, { "type", "Float" } });
	CHECK(again.isError);
	CHECK(f.graph.variables.size() == 1);
	CHECK(f.graph.variables[0].type == HC::PinType::Int);

	// hc_set_variable takes the object hc_get returns, so the round trip has to
	// work on the real payload rather than on a hand-built one.
	json v = json::parse(HC::variableToJson(f.graph.variables[0]));
	v["access"] = 1;
	f.ok("hc_set_variable", json{ { "variable", v } });
	CHECK(f.graph.variables[0].access == 1);

	CHECK(f.call("hc_set_variable", json{ { "variable",
		json{ { "name", "Nope" }, { "type", 1 } } } }).errorCode == "not_found");

	// A Get Variable node that named it is LEFT standing, and reported.
	const int get = f.addNode("Get Variable", "Score");
	const json removed = f.ok("hc_remove_variable", json{ { "name", "Score" } });
	CHECK(f.graph.variables.empty());
	CHECK(f.graph.findNode(get) != nullptr);
	REQUIRE(removed["orphanedNodes"].size() == 1);
	CHECK(removed["orphanedNodes"][0].get<int>() == get);
}

TEST_CASE("hc tools: hc_set_node upserts, and drops links into pins that are gone")
{
	Fixture f;
	const int seq = f.addNode("Sequence");
	const int ev  = f.addNode("Event", "OnLevelLoaded");
	f.ok("hc_connect", json{ { "srcNode", ev },  { "srcPin", "exec" },
	                         { "dstNode", seq }, { "dstPin", "exec" } });

	json node = f.ok("hc_get", json{ { "nodeIds", json::array({ seq }) } })["nodes"][0];
	// The derived member has to be tolerated: the shape a client reads back is
	// the shape it will send, and a tool that rejected its own output would be
	// unusable.
	REQUIRE(node.contains("pins"));
	node["pos"] = json::array({ 400, 250 });
	f.ok("hc_set_node", json{ { "node", node } });
	CHECK(f.graph.findNode(seq)->x == doctest::Approx(400.0f));
	CHECK(f.graph.links.size() == 1);

	// Turn it into a node with fewer pins. Nothing on the collaboration path
	// prunes the links that lost their pin (a peer sends the node and its link
	// deltas together); a client sends one object, so this has to.
	const HC::NodeSig seqSig = HC::signatureOf(*f.graph.findNode(seq));
	json shrunk = node;
	shrunk["type"] = "Print";
	shrunk.erase("pins");
	const json r = f.ok("hc_set_node", json{ { "node", shrunk } });
	CHECK(f.graph.findNode(seq)->type == HC::NodeType::Print);
	const HC::NodeSig printSig = HC::signatureOf(*f.graph.findNode(seq));
	// Only meaningful if Print really is the narrower of the two.
	if (printSig.execOuts.size() < seqSig.execOuts.size())
		CHECK(r["droppedLinks"].size() + f.graph.links.size() == 1);
	for (const HC::Link& l : f.graph.links)
		CHECK(l.dstPin < static_cast<int>(printSig.execIns.size() + printSig.execOuts.size() +
		                                  printSig.dataIns.size() + printSig.dataOuts.size()));
}

TEST_CASE("hc tools: an edit comes out of the collaboration diff as exactly one delta")
{
	// The reason every mutation goes through the CollabDocSync adapter at all:
	// in a live session the panel's own mirror diff has to see the change and
	// publish it. A handler that wrote into the graph by hand would still pass
	// every other test in this file and lose exactly this.
	Fixture f;
	f.addNode("Event", "OnLevelLoaded");

	CollabDocSync::DocMirror mirror;
	const auto adapter = CollabDocSync::forHorizonCodeGraph(f.graph);
	CollabDocSync::seed(*adapter, mirror, CollabDocSync::Scope::Primary);

	std::vector<HE::Net::CollabSession::DocDelta> deltas;
	CollabDocSync::diffInto(*adapter, mirror, CollabDocSync::Scope::Primary, deltas);
	CHECK(deltas.empty());   // seeded: nothing has happened yet

	const int br = f.addNode("Branch");
	CollabDocSync::diffInto(*adapter, mirror, CollabDocSync::Scope::Primary, deltas);
	REQUIRE(deltas.size() == 1);
	CHECK(deltas[0].itemId == br);

	// And a change INSIDE a node is a delta too — the pin default has to travel
	// in the item payload or the peer never learns the constant.
	deltas.clear();
	const HC::NodeSig s = HC::signatureOf(*f.graph.findNode(br));
	const int cond = static_cast<int>(s.execIns.size() + s.execOuts.size());
	f.ok("hc_set_pin_default", json{ { "node", br }, { "pin", cond }, { "value", true } });
	CollabDocSync::diffInto(*adapter, mirror, CollabDocSync::Scope::Primary, deltas);
	REQUIRE(deltas.size() == 1);
	CHECK(deltas[0].itemId == br);
}

TEST_CASE("hc tools: hc_get reports the graph the way the file stores it")
{
	Fixture f;
	const int ev = f.addNode("Event", "OnLevelLoaded", 10, 20);
	const int br = f.addNode("Branch", "", 200, 20);
	f.ok("hc_connect", json{ { "srcNode", ev }, { "srcPin", "exec" },
	                         { "dstNode", br }, { "dstPin", "exec" } });
	f.ok("hc_add_variable", json{ { "name", "Alive" }, { "type", "Bool" } });

	const json g = f.ok("hc_get");
	CHECK(g["key"].get<std::string>() == HE::Ed::kMcpDocLevelScript);
	CHECK(g["nextId"].get<int>() == f.graph.nextId);
	REQUIRE(g["nodes"].size() == 2);
	REQUIRE(g["links"].size() == 1);
	REQUIRE(g["variables"].size() == 1);
	CHECK(g["variables"][0]["name"].get<std::string>() == "Alive");
	CHECK(g["truncated"].get<int>() == 0);

	// The node object is the ITEM JSON, not a second description of it.
	json fromTool = g["nodes"][0];
	fromTool.erase("pins");
	CHECK(fromTool == json::parse(HC::nodeToJson(*f.graph.findNode(ev))));

	// Filters narrow rather than page.
	CHECK(f.ok("hc_get", json{ { "nodeIds", json::array({ br }) } })["nodes"].size() == 1);
	const json clipped = f.ok("hc_get", json{ { "limit", 1 } });
	CHECK(clipped["nodes"].size() == 1);
	CHECK(clipped["truncated"].get<int>() == 1);
	CHECK_FALSE(f.ok("hc_get", json{ { "includePins", false } })["nodes"][0].contains("pins"));
}

TEST_CASE("hc tools: hc_save goes through the editor and reports a refusal as one")
{
	Fixture f;
	f.addNode("Branch");
	CHECK_FALSE(f.call("hc_save").isError);
	CHECK(f.saves == 1);

	f.saveWorks = false;
	const ToolResult r = f.call("hc_save");
	CHECK(r.isError);
	CHECK(r.errorCode == "save_failed");
}

TEST_CASE("hc tools: hc_documents is the only place a key comes from")
{
	Fixture f;
	const json docs = f.ok("hc_documents");
	REQUIRE(docs["documents"].size() == 1);
	CHECK(docs["documents"][0]["key"].get<std::string>() == HE::Ed::kMcpDocLevelScript);
	CHECK(docs["documents"][0]["kind"].get<std::string>() == "level");
	// The restrictions are reported rather than hidden: a client refused an
	// Engine Call otherwise has no way to learn that this document restricts
	// them at all.
	CHECK(docs["documents"][0]["apiGroups"].is_array());
	CHECK(docs["documents"][0]["excludedNodeTypes"].is_array());
}
