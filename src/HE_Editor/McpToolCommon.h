#pragma once

// ─── What every path-taking MCP tool needs ───────────────────────────────────
// The argument readers and — the load-bearing half — the ONE confinement rule.
//
// McpToolRegistry.h promises an external client no file access. The asset tools
// and the scene tools both take paths, so that promise is only as good as
// `checkPath`, and it must be exactly one function: two copies of a boundary are
// two boundaries, and the second one is the one that quietly stops matching the
// first. This file exists because the scene tools were the second caller.
//
// Three separate refusals, because they are three different mistakes: a shape
// that is not a content-relative path at all, a path that RESOLVES outside every
// known root (which is what `..` buys), and the reserved "Engine/" namespace,
// which the Content Browser also refuses to mutate.

#include "McpToolRegistry.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class ContentManager;

namespace HE::Ed
{

// ── Small readers ────────────────────────────────────────────────────────────
// A missing or wrongly-typed argument reads as the fallback rather than
// throwing: the schema is what tells a client the shape, and a handler that
// threw on a stray null would take the bridge's frame with it.
std::string strArg (const nlohmann::json& args, const char* key);
bool        boolArg(const nlohmann::json& args, const char* key, bool fallback = false);
int         intArg (const nlohmann::json& args, const char* key, int fallback);

// `is_number`, not `is_number_float`: a client that sends 5 where the schema
// says number means 5.0, and refusing that is a refusal nobody can debug from
// the schema. `hasArg` is the separate question of whether it was sent at all —
// which for a coordinate is not the same as "sent 0".
double numArg(const nlohmann::json& args, const char* key, double fallback);
bool   hasArg(const nlohmann::json& args, const char* key);

nlohmann::json objectSchema(nlohmann::json properties, std::vector<std::string> required);
nlohmann::json stringProp(const char* what);
nlohmann::json numberProp(const char* what);

// ── Confinement ──────────────────────────────────────────────────────────────
struct PathCheck
{
	std::string rel;              // normalised, forward slashes
	std::string abs;              // lexically normal, inside a known root
	bool        engine = false;   // under the reserved "Engine/" namespace
	bool        ok = false;
	ToolResult  failure = ToolResult::ok(nlohmann::json::object());
};

// `argName` names the offending argument in the refusal, so a client reads
// which of its fields was wrong rather than which of ours.
PathCheck checkPath(ContentManager& content, const std::string& raw, bool mustExist,
                    const char* argName);

// The reserved namespace is read-only from a project's perspective — the same
// gate the Content Browser applies (`engineLocked`), and for the same reason:
// those files are the shipped engine defaults, shared by every project on this
// machine.
ToolResult failEngineReadOnly(const std::string& rel);

} // namespace HE::Ed
