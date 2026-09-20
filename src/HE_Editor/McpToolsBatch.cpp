#include "McpToolCommon.h"
#include "McpToolRegistry.h"

#include <Diagnostics/Logger.h>

#include <memory>
#include <string>

// ─── Several tool calls in one request ───────────────────────────────────────
// One tool, `batch`, that takes a list of {tool, args} pairs and runs them in
// order through the same registry the bridge dispatches from. Ten entities to
// move used to be ten round trips; now they are one request and one answer.
//
// ── Partial success, stated rather than implied ──────────────────────────────
// The question this tool has to answer before it exists is what happens when
// operation 3 of 10 fails. The answer, deliberately:
//
//   • By DEFAULT the batch keeps going. Every element gets its own entry in the
//     result — index, tool name, ok, and either the tool's payload or its error
//     code and message. A failure in the middle changes nothing for the ones
//     after it. That is what "move these ten" means to a client: nine moved
//     and one refused is nine moved, not zero.
//   • With `stopOnError: true` the first failure ends the batch. The elements
//     after it are reported as `skipped`, so the result array stays index-
//     aligned with the request and a client can tell "did not run" from "ran
//     and failed" without counting.
//
// Either way the batch's OWN answer is `isError: false` once it has started.
// That is not cosmetics: the bridge forwards a failed ToolResult as code and
// message ONLY (McpBridge.cpp, tools/call) and discards the payload, so a batch
// that reported its partial failures through `isError` would report them into
// the void. `isError: true` is reserved for a request that cannot be run at
// all — no `operations` array, or more elements than the cap.
//
// ── What is NOT a whole-batch refusal ────────────────────────────────────────
// An unknown tool name, an element that is not an object, an element without a
// `tool` string — each of those is an error of ONE element and is reported in
// that element's slot, exactly like a tool that ran and refused. Refusing the
// whole request for a typo in element 7 would throw away the six that were
// fine, and the client would have to resend them. `stopOnError` sees these as
// failures too, which is the consistent reading.
//
// ── No special path past the guards ──────────────────────────────────────────
// Each element goes to `registry.find(tool)->handler(args)`, the same call the
// bridge makes for a single request. Whatever a tool checks on its own —
// play-in-editor, a peer's lock, a dirty document, the gateway's lock state —
// it checks inside a batch too, because it is the same handler with the same
// arguments. This file knows nothing about locks and must stay that way: the
// day it does, it is a second boundary.
//
// ── Not atomic, and not the plan's `Batch` ───────────────────────────────────
// docs/mcp-editor-integration-plan.md describes a gateway command `Batch` that
// applies a list of commands as one undo entry and unwinds on failure. That
// was never built and this is not it. Here every mutating element leaves its
// own undo entry and nothing is rolled back — a client that needs
// all-or-nothing has to check the result and undo itself.
//
// ── Nesting is refused ───────────────────────────────────────────────────────
// A batch inside a batch expresses nothing a flat list cannot, and a depth
// limit would just be a number to argue about. So an element naming `batch`
// is refused with `nested_batch` — and a re-entrancy flag refuses the same
// thing a second way, in case a later alias makes the name check blind.
// Handlers run on the editor's frame thread (McpToolCommon.h), so a plain bool
// is enough.
//
// The cap on the element count exists for that same thread: a 5000-element
// batch would hold the frame for as long as 5000 requests would, with no
// chance for the human to do anything in between.

namespace HE::Ed
{

using nlohmann::json;

namespace
{

constexpr int         kMaxOperations = 100;
constexpr const char* kBatchName     = "batch";

json operationSchema()
{
	return objectSchema(
		json{
			{ "tool", stringProp("Name of a registered tool, exactly as listed by "
			                     "tools/list. 'batch' itself is refused.") },
			{ "args", json{ { "type", "object" },
			                { "description", "The arguments for that tool, in its own "
			                                 "schema. Omitted = no arguments." } } },
		},
		{ "tool" });
}

// One slot of the result array. `ok`, the index and the name are always there;
// `result` on success, `error` on failure, `skipped` when a stop came first.
json slotFor(std::size_t index, const std::string& tool)
{
	return json{
		{ "index", index },
		{ "tool",  tool },
		{ "ok",    false },
	};
}

json failed(json slot, const std::string& code, const std::string& message)
{
	slot["error"] = json{ { "code", code }, { "message", message } };
	return slot;
}

} // namespace

void registerBatchTool(McpToolRegistry& registry)
{
	McpTool batch;
	batch.name        = kBatchName;
	batch.description =
		"Run several tool calls in one request, in order. Each operation is "
		"{tool, args} and is dispatched exactly as a single call would be, so "
		"every tool's own refusals (play mode, a peer's lock, an unknown entity) "
		"apply unchanged. The answer has one entry per operation, index-aligned: "
		"{index, tool, ok, result} or {index, tool, ok:false, error:{code,message}}. "
		"By default a failure does NOT stop the batch — the remaining operations "
		"still run and every outcome is reported; set stopOnError=true to stop at "
		"the first failure, after which the rest are reported as skipped. An "
		"unknown tool name or a malformed operation is a failure of that one "
		"operation, not of the batch. The batch itself only errors when it cannot "
		"run at all (no operations array, or more than " +
		std::to_string(kMaxOperations) + " operations). Not atomic: nothing is "
		"rolled back, and each mutating operation leaves its own undo entry. "
		"'batch' inside a batch is refused.";
	batch.inputSchema = objectSchema(
		json{
			{ "operations", json{
				{ "type", "array" },
				{ "items", operationSchema() },
				{ "maxItems", kMaxOperations },
				{ "description", "The calls to make, in order. At most " +
				                 std::to_string(kMaxOperations) + "." } } },
			{ "stopOnError", json{
				{ "type", "boolean" },
				{ "description", "true = stop at the first failing operation and "
				                 "report the rest as skipped. Default false: run "
				                 "every operation and report each outcome." } } },
		},
		{ "operations" });
	// It can run anything, including every mutating tool, so it is logged as
	// one — and each mutating element is logged below on its own, because
	// "MCP: batch" alone does not answer who moved what.
	batch.mutates = true;

	auto running = std::make_shared<bool>(false);
	batch.handler = [&registry, running](const json& args) -> ToolResult {
		if (*running)
			return ToolResult::fail("nested_batch",
			                        "'batch' cannot be called from inside a batch. "
			                        "Put the operations in the outer list instead.");

		if (!args.is_object() || !args.contains("operations") ||
		    !args["operations"].is_array())
			return ToolResult::fail("invalid_params",
			                        "'operations' must be an array of {tool, args}.");

		const json& ops = args["operations"];
		if (ops.size() > static_cast<std::size_t>(kMaxOperations))
			return ToolResult::fail("too_many_operations",
			                        "A batch holds at most " + std::to_string(kMaxOperations) +
			                            " operations, this one has " +
			                            std::to_string(ops.size()) +
			                            ". Split it into several requests.");

		const bool stopOnError = boolArg(args, "stopOnError", false);

		*running = true;
		struct Reset
		{
			bool& flag;
			~Reset() { flag = false; }
		} reset{ *running };

		json results = json::array();
		int  succeeded = 0, failedCount = 0, skipped = 0;
		bool stopped   = false;
		int  stoppedAt = -1;

		for (std::size_t i = 0; i < ops.size(); ++i)
		{
			const json& op = ops[i];
			const std::string tool = op.is_object() ? strArg(op, "tool") : std::string();

			if (stopped)
			{
				json slot = slotFor(i, tool);
				slot["skipped"] = true;
				results.push_back(std::move(slot));
				++skipped;
				continue;
			}

			// The element-level refusals: reported in the slot, never as a
			// refusal of the batch, because the element after this one may be
			// perfectly fine.
			json failure;
			bool isFailure = true;
			if (!op.is_object())
				failure = failed(slotFor(i, tool), "invalid_operation",
				                 "operation " + std::to_string(i) +
				                     " is not an object; expected {tool, args}");
			else if (tool.empty())
				failure = failed(slotFor(i, tool), "invalid_operation",
				                 "operation " + std::to_string(i) +
				                     " has no 'tool' string");
			else if (tool == kBatchName)
				failure = failed(slotFor(i, tool), "nested_batch",
				                 "operation " + std::to_string(i) +
				                     " is 'batch' — a batch cannot contain a batch; "
				                     "put its operations in this list instead");
			else if (op.contains("args") && !op["args"].is_null() && !op["args"].is_object())
				failure = failed(slotFor(i, tool), "invalid_operation",
				                 "operation " + std::to_string(i) +
				                     ": 'args' must be an object");
			else
				isFailure = false;

			if (!isFailure)
			{
				const McpTool* target = registry.find(tool);
				if (!target)
				{
					failure = failed(slotFor(i, tool), "unknown_tool",
					                 "operation " + std::to_string(i) +
					                     ": no such tool '" + tool + "'");
					isFailure = true;
				}
				else
				{
					// Missing `args` reads as an empty object — the same reading
					// the bridge gives a missing `arguments`.
					const json subArgs = (op.contains("args") && op["args"].is_object())
					                         ? op["args"] : json::object();
					const ToolResult r = target->handler(subArgs);
					json slot = slotFor(i, tool);
					if (r.isError)
					{
						failure   = failed(std::move(slot), r.errorCode, r.errorMessage);
						isFailure = true;
					}
					else
					{
						slot["ok"]     = true;
						slot["result"] = r.content;
						results.push_back(std::move(slot));
						++succeeded;
						if (target->mutates)
							HE_LOG_INFO(Editor, "MCP: batch[%zu] %s", i, tool.c_str());
					}
				}
			}

			if (isFailure)
			{
				results.push_back(std::move(failure));
				++failedCount;
				if (stopOnError)
				{
					stopped   = true;
					stoppedAt = static_cast<int>(i);
				}
			}
		}

		return ToolResult::ok(json{
			{ "results",   std::move(results) },
			{ "total",     ops.size() },
			{ "succeeded", succeeded },
			{ "failed",    failedCount },
			{ "skipped",   skipped },
			{ "stopped",   stopped },
			{ "stoppedAt", stoppedAt },   // -1 = the batch ran to the end
		});
	};
	registry.add(std::move(batch));
}

} // namespace HE::Ed
