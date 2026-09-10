#include "McpToolCommon.h"

#include <ContentManager/ContentManager.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace HE::Ed
{

using nlohmann::json;

std::string strArg(const json& args, const char* key)
{
	if (!args.is_object()) return {};
	const auto it = args.find(key);
	return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool boolArg(const json& args, const char* key, bool fallback)
{
	if (!args.is_object()) return fallback;
	const auto it = args.find(key);
	return (it != args.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

int intArg(const json& args, const char* key, int fallback)
{
	if (!args.is_object()) return fallback;
	const auto it = args.find(key);
	return (it != args.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

double numArg(const json& args, const char* key, double fallback)
{
	if (!args.is_object()) return fallback;
	const auto it = args.find(key);
	return (it != args.end() && it->is_number()) ? it->get<double>() : fallback;
}

bool hasArg(const json& args, const char* key)
{
	return args.is_object() && args.find(key) != args.end() && !args[key].is_null();
}

json objectSchema(json properties, std::vector<std::string> required)
{
	json s{
		{ "type",       "object" },
		{ "properties", std::move(properties) },
		{ "additionalProperties", false },
	};
	if (!required.empty()) s["required"] = required;
	return s;
}

json stringProp(const char* what)
{
	return json{ { "type", "string" }, { "description", what } };
}

json numberProp(const char* what)
{
	return json{ { "type", "number" }, { "description", what } };
}

namespace
{

ToolResult failPath(const std::string& why)
{
	return ToolResult::fail("invalid_path", why);
}

// Is `child` inside `root`? Both already lexically normal and absolute.
bool isUnder(const std::filesystem::path& child, const std::filesystem::path& root)
{
	if (root.empty()) return false;
	auto c = child.begin();
	auto r = root.begin();
	for (; r != root.end(); ++r, ++c)
	{
		if (c == child.end()) return false;
		// The trailing empty element a path ending in a separator carries.
		if (r->empty() && ++decltype(r)(r) == root.end()) break;
		if (*c != *r) return false;
	}
	return true;
}

} // namespace

PathCheck checkPath(ContentManager& content, const std::string& raw, bool mustExist,
                    const char* argName)
{
	PathCheck p;
	if (raw.empty())
	{
		p.failure = failPath(std::string("'") + argName + "' is required: a "
		                     "content-relative path such as 'Materials/Rock.hasset' "
		                     "or 'Levels/Main.hescene'.");
		return p;
	}

	// Windows separators arrive from clients that assemble paths with the host
	// OS in mind; stored references only ever use '/'.
	std::string rel = raw;
	std::replace(rel.begin(), rel.end(), '\\', '/');

	// An absolute path is refused as ONE, before anything is stripped off it.
	// Quietly dropping the leading slash would be the tempting leniency and it is
	// the wrong one twice over: '/Materials/Rock.hasset' (a client that meant the
	// content root) and '/Users/someone/.ssh/id_rsa' are the same string after
	// stripping, and the second one would come back as a plain "not found" — a
	// refusal that reads like "try a different absolute path".
	if (!rel.empty() && (rel.front() == '/' || (rel.size() >= 2 && rel[1] == ':')))
	{
		p.failure = failPath("'" + raw + "' is an absolute path. These tools address "
		                     "assets content-relative to the open project, e.g. "
		                     "'Materials/Rock.hasset' — drop the leading '/' if that "
		                     "is what was meant.");
		return p;
	}

	while (!rel.empty() && rel.back() == '/') rel.pop_back();
	if (rel.empty())
	{
		p.failure = failPath(std::string("'") + argName + "' is the content root "
		                     "itself, which is not an asset.");
		return p;
	}
	for (const auto& part : std::filesystem::path(rel))
	{
		if (part == "..")
		{
			p.failure = failPath("'" + rel + "' walks out of the content root with "
			                     "'..'. These tools reach the open project's content "
			                     "and nothing else on this machine.");
			return p;
		}
	}

	if (content.contentRoot().empty())
		return { {}, {}, false, false,
		         ToolResult::fail("no_project", "No project is open in the editor, so "
		                          "there is no content root to address. Call scene_info "
		                          "first.") };

	const std::string absRaw = content.resolveAbsolutePath(rel);
	if (absRaw.empty())
	{
		p.failure = failPath("'" + rel + "' does not resolve to anything under the "
		                     "project's content root.");
		return p;
	}

	std::error_code ec;
	std::filesystem::path abs = std::filesystem::path(absRaw).lexically_normal();

	// The check that actually keeps the promise: resolution is symbolic, so a
	// path can come back looking fine and still point outside. Both roots count —
	// "Engine/…" legitimately resolves into the shared engine content.
	const std::filesystem::path contentRoot =
		std::filesystem::path(content.contentRoot()).lexically_normal();
	const std::filesystem::path engineRoot =
		content.engineContentRoot().empty()
			? std::filesystem::path{}
			: std::filesystem::path(content.engineContentRoot()).lexically_normal();
	if (!isUnder(abs, contentRoot) && !(engineRoot.empty() ? false : isUnder(abs, engineRoot)))
	{
		p.failure = failPath("'" + rel + "' resolves outside the project's content "
		                     "root. These tools reach the open project's content and "
		                     "nothing else on this machine.");
		return p;
	}

	if (mustExist && !std::filesystem::exists(abs, ec))
	{
		p.failure = ToolResult::fail("not_found",
			"No file at '" + rel + "'. Use asset_list to see what is there; a path "
			"from another project or an earlier session does not carry over.");
		return p;
	}

	p.rel    = rel;
	p.abs    = abs.string();
	p.engine = rel.rfind("Engine/", 0) == 0;
	p.ok     = true;
	return p;
}

ToolResult failEngineReadOnly(const std::string& rel)
{
	return ToolResult::fail("read_only",
		"'" + rel + "' is in the reserved 'Engine/' namespace — the engine's shipped "
		"default content, shared by every project on this machine. The editor does not "
		"let a human create, delete or move anything there either. Save a copy under "
		"the project's own content instead.");
}

} // namespace HE::Ed
