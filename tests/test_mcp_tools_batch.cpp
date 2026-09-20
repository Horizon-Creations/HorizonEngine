#include "doctest.h"

#include "McpToolRegistry.h"

#include <string>
#include <vector>

// ─── Several tool calls in one request ───────────────────────────────────────
// The batch tool is a dispatcher, so the questions here are about dispatch, not
// about any tool it dispatches to: does every element run in order with its own
// arguments, does a failure in the middle leave the ones after it alone (the
// default) or stop them (stopOnError), does the result stay index-aligned in
// both cases, and does a tool's own refusal come through UNCHANGED — because a
// batch that softened `play_mode` or `locked_by_other` would be a way around
// them.
//
// EditorApplication is not in this binary and does not need to be: the tools
// under test are the two core stubs plus two registered here, one that counts
// its calls (and is marked mutating, so the log path runs) and one that refuses
// the way a guarded tool does.

using HE::Ed::McpEditorHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

struct Fixture
{
	McpToolRegistry          registry;
	std::vector<std::string> calls;   // the `tag` of every `count` call, in order

	Fixture()
	{
		HE::Ed::registerCoreTools(registry, McpEditorHooks{});

		McpTool count;
		count.name        = "count";
		count.description = "records its tag";
		count.inputSchema = json{ { "type", "object" } };
		count.mutates     = true;
		count.handler     = [this](const json& args) {
			const std::string tag = args.value("tag", std::string("-"));
			calls.push_back(tag);
			return ToolResult::ok(json{ { "tag", tag }, { "n", calls.size() } });
		};
		registry.add(std::move(count));

		// What a guarded tool answers while play-in-editor runs — the shape of
		// McpToolsMaterial's refusal, so a batch is proven to carry exactly it.
		McpTool guarded;
		guarded.name        = "guarded";
		guarded.description = "always refuses";
		guarded.inputSchema = json{ { "type", "object" } };
		guarded.mutates     = true;
		guarded.handler     = [](const json&) {
			return ToolResult::fail("play_mode", "Refused while play-in-editor runs.");
		};
		registry.add(std::move(guarded));

		HE::Ed::registerBatchTool(registry);
	}

	ToolResult batch(const json& args) const
	{
		const McpTool* t = registry.find("batch");
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	static json op(const char* tool, json args = json::object())
	{
		return json{ { "tool", tool }, { "args", std::move(args) } };
	}
};

} // namespace

TEST_CASE("batch is registered as a legal, mutating tool with a schema")
{
	Fixture f;
	const McpTool* t = f.registry.find("batch");
	REQUIRE(t != nullptr);
	CHECK(McpToolRegistry::enforceNameRule(t->name));
	CHECK(t->mutates);
	CHECK(t->inputSchema["type"] == "object");
	CHECK(t->inputSchema["properties"].contains("operations"));
	CHECK(t->inputSchema["properties"].contains("stopOnError"));
	// The contract the model reads has to state the default, because it is the
	// less obvious of the two behaviours.
	CHECK(t->description.find("does NOT stop") != std::string::npos);
	// Last in the list: after the families it dispatches to.
	CHECK(f.registry.tools().back().name == "batch");
}

TEST_CASE("Five valid operations and one unknown tool: the five run, the one is reported")
{
	Fixture f;
	const ToolResult r = f.batch(json{ { "operations", json::array({
		Fixture::op("count", { { "tag", "a" } }),
		Fixture::op("count", { { "tag", "b" } }),
		Fixture::op("no_such_tool", { { "x", 1 } }),   // the invalid one, in the middle
		Fixture::op("count", { { "tag", "c" } }),
		Fixture::op("ping",  { { "echo", "hi" } }),
		Fixture::op("count", { { "tag", "d" } }),
	}) } });

	// The batch itself succeeded — the bridge would drop the per-element
	// results otherwise (it forwards only code+message for an isError result).
	REQUIRE_FALSE(r.isError);
	const json& c = r.content;
	REQUIRE(c["results"].is_array());
	REQUIRE(c["results"].size() == 6);
	CHECK(c["total"]     == 6);
	CHECK(c["succeeded"] == 5);
	CHECK(c["failed"]    == 1);
	CHECK(c["skipped"]   == 0);
	CHECK(c["stopped"]   == false);
	CHECK(c["stoppedAt"] == -1);

	// Everything after the failure still ran, in order, with its own arguments.
	CHECK(f.calls == std::vector<std::string>{ "a", "b", "c", "d" });

	// Index-aligned slots, each naming its tool.
	for (std::size_t i = 0; i < 6; ++i)
	{
		CHECK(c["results"][i]["index"] == i);
		CHECK(c["results"][i]["tool"].is_string());
	}
	CHECK(c["results"][0]["ok"] == true);
	CHECK(c["results"][0]["result"]["tag"] == "a");
	CHECK(c["results"][4]["ok"] == true);
	CHECK(c["results"][4]["result"]["echo"] == "hi");   // args reach the tool

	// The one that failed carries a clear error in its slot and nothing else.
	const json& bad = c["results"][2];
	CHECK(bad["tool"] == "no_such_tool");
	CHECK(bad["ok"] == false);
	CHECK_FALSE(bad.contains("result"));
	CHECK_FALSE(bad.contains("skipped"));
	CHECK(bad["error"]["code"] == "unknown_tool");
	CHECK(bad["error"]["message"].get<std::string>().find("no_such_tool") != std::string::npos);
	CHECK(bad["error"]["message"].get<std::string>().find("operation 2") != std::string::npos);
}

TEST_CASE("A tool's own refusal comes through a batch unchanged")
{
	// No special path: the guarded tool refuses inside a batch with the same
	// code and message it would give a single call, and the batch goes on.
	Fixture f;
	const ToolResult single = f.registry.find("guarded")->handler(json::object());
	REQUIRE(single.isError);

	const ToolResult r = f.batch(json{ { "operations", json::array({
		Fixture::op("count",   { { "tag", "a" } }),
		Fixture::op("guarded"),
		Fixture::op("count",   { { "tag", "b" } }),
	}) } });
	REQUIRE_FALSE(r.isError);
	const json& c = r.content;
	CHECK(c["succeeded"] == 2);
	CHECK(c["failed"]    == 1);
	CHECK(c["results"][1]["ok"] == false);
	CHECK(c["results"][1]["error"]["code"]    == single.errorCode);
	CHECK(c["results"][1]["error"]["message"] == single.errorMessage);
	CHECK(f.calls == std::vector<std::string>{ "a", "b" });
}

TEST_CASE("stopOnError stops at the first failure and marks the rest skipped")
{
	Fixture f;
	const ToolResult r = f.batch(json{
		{ "stopOnError", true },
		{ "operations", json::array({
			Fixture::op("count", { { "tag", "a" } }),
			Fixture::op("count", { { "tag", "b" } }),
			Fixture::op("guarded"),                     // index 2 fails
			Fixture::op("count", { { "tag", "c" } }),   // must not run
			Fixture::op("no_such_tool"),                // must not even be looked up
			Fixture::op("count", { { "tag", "d" } }),
		}) },
	});

	// Still not an error of the batch: the two that ran are in the answer.
	REQUIRE_FALSE(r.isError);
	const json& c = r.content;
	REQUIRE(c["results"].size() == 6);
	CHECK(c["total"]     == 6);
	CHECK(c["succeeded"] == 2);
	CHECK(c["failed"]    == 1);
	CHECK(c["skipped"]   == 3);
	CHECK(c["stopped"]   == true);
	CHECK(c["stoppedAt"] == 2);
	CHECK(f.calls == std::vector<std::string>{ "a", "b" });

	CHECK(c["results"][0]["ok"] == true);
	CHECK(c["results"][1]["ok"] == true);
	CHECK(c["results"][2]["ok"] == false);
	CHECK(c["results"][2]["error"]["code"] == "play_mode");
	for (std::size_t i = 3; i < 6; ++i)
	{
		// "Did not run" is distinguishable from "ran and failed": no error, a
		// skipped mark, and the slot still says which tool it would have been.
		CHECK(c["results"][i]["index"] == i);
		CHECK(c["results"][i]["ok"] == false);
		CHECK(c["results"][i]["skipped"] == true);
		CHECK_FALSE(c["results"][i].contains("error"));
		CHECK_FALSE(c["results"][i].contains("result"));
	}
	CHECK(c["results"][3]["tool"] == "count");
	CHECK(c["results"][4]["tool"] == "no_such_tool");
}

TEST_CASE("stopOnError=false is the default and an explicit false reads the same")
{
	Fixture f;
	const ToolResult r = f.batch(json{
		{ "stopOnError", false },
		{ "operations", json::array({
			Fixture::op("guarded"),
			Fixture::op("count", { { "tag", "after" } }),
		}) },
	});
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["stopped"] == false);
	CHECK(r.content["succeeded"] == 1);
	CHECK(f.calls == std::vector<std::string>{ "after" });
}

TEST_CASE("A batch inside a batch is refused by name, and the rest still runs")
{
	Fixture f;
	const ToolResult r = f.batch(json{ { "operations", json::array({
		Fixture::op("count", { { "tag", "a" } }),
		Fixture::op("batch", { { "operations", json::array({ Fixture::op("count", { { "tag", "inner" } }) }) } }),
		Fixture::op("count", { { "tag", "b" } }),
	}) } });
	REQUIRE_FALSE(r.isError);
	const json& c = r.content;
	CHECK(c["results"][1]["ok"] == false);
	CHECK(c["results"][1]["error"]["code"] == "nested_batch");
	// The inner list never ran — no "inner" among the calls.
	CHECK(f.calls == std::vector<std::string>{ "a", "b" });
	CHECK(c["succeeded"] == 2);
	CHECK(c["failed"] == 1);
}

TEST_CASE("Re-entry is refused even when the name check is bypassed")
{
	// The second layer: a tool that itself calls batch (what an alias would be)
	// finds the flag set and is refused, rather than recursing.
	Fixture f;
	McpTool alias;
	alias.name        = "alias_batch";
	alias.description = "calls batch";
	alias.inputSchema = json{ { "type", "object" } };
	alias.handler     = [&f](const json& args) { return f.batch(args); };
	f.registry.add(std::move(alias));

	const ToolResult r = f.batch(json{ { "operations", json::array({
		Fixture::op("alias_batch", { { "operations", json::array({ Fixture::op("count", { { "tag", "inner" } }) }) } }),
		Fixture::op("count", { { "tag", "outer" } }),
	}) } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["results"][0]["ok"] == false);
	CHECK(r.content["results"][0]["error"]["code"] == "nested_batch");
	CHECK(f.calls == std::vector<std::string>{ "outer" });

	// And the flag is cleared afterwards: a following batch runs normally.
	const ToolResult again = f.batch(json{ { "operations", json::array({
		Fixture::op("count", { { "tag", "later" } }) }) } });
	REQUIRE_FALSE(again.isError);
	CHECK(again.content["succeeded"] == 1);
}

TEST_CASE("Malformed elements fail in their slot; a malformed envelope fails the batch")
{
	Fixture f;

	SUBCASE("element shapes")
	{
		const ToolResult r = f.batch(json{ { "operations", json::array({
			42,                                              // not an object
			json{ { "args", json::object() } },              // no tool
			json{ { "tool", "count" }, { "args", "nope" } }, // args not an object
			json{ { "tool", "count" } },                     // no args at all: fine
		}) } });
		REQUIRE_FALSE(r.isError);
		const json& c = r.content;
		REQUIRE(c["results"].size() == 4);
		CHECK(c["results"][0]["error"]["code"] == "invalid_operation");
		CHECK(c["results"][1]["error"]["code"] == "invalid_operation");
		CHECK(c["results"][2]["error"]["code"] == "invalid_operation");
		CHECK(c["results"][3]["ok"] == true);
		CHECK(c["results"][3]["result"]["tag"] == "-");   // missing args = empty object
		CHECK(c["succeeded"] == 1);
		CHECK(c["failed"] == 3);
		CHECK(f.calls == std::vector<std::string>{ "-" });
	}

	SUBCASE("no operations array")
	{
		CHECK(f.batch(json::object()).isError);
		CHECK(f.batch(json::object()).errorCode == "invalid_params");
		CHECK(f.batch(json{ { "operations", "count" } }).errorCode == "invalid_params");
		CHECK(f.calls.empty());
	}

	SUBCASE("empty list is a valid, empty batch")
	{
		const ToolResult r = f.batch(json{ { "operations", json::array() } });
		REQUIRE_FALSE(r.isError);
		CHECK(r.content["results"].empty());
		CHECK(r.content["total"] == 0);
	}

	SUBCASE("over the cap nothing runs")
	{
		json ops = json::array();
		for (int i = 0; i < 101; ++i) ops.push_back(Fixture::op("count", { { "tag", "x" } }));
		const ToolResult r = f.batch(json{ { "operations", ops } });
		CHECK(r.isError);
		CHECK(r.errorCode == "too_many_operations");
		CHECK(f.calls.empty());

		ops.erase(ops.size() - 1);
		const ToolResult ok = f.batch(json{ { "operations", ops } });
		CHECK_FALSE(ok.isError);
		CHECK(ok.content["succeeded"] == 100);
	}
}
