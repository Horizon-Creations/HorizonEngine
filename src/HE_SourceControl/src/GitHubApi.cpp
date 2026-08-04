#include "SourceControl/GitHubApi.h"

#include "ScLog.h"

#include <Net/HttpsClient.h>

#include <nlohmann/json.hpp>

namespace HE::Sc {

using nlohmann::json;

bool GitHubApi::parseCreateRepoResponse(int statusCode, const std::string& body,
                                        CreatedRepo& out, std::string* err)
{
	out = CreatedRepo{};

	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);

	if (statusCode == 201)
	{
		if (!j.is_object() || !j.contains("clone_url"))
		{
			if (err) *err = "GitHub answered success but without a repository URL";
			return false;
		}
		out.cloneUrl = j.value("clone_url", "");
		out.fullName = j.value("full_name", "");
		return !out.cloneUrl.empty();
	}

	// The actionable cases, in the user's language rather than HTTP's. The
	// response body never contains the token, so quoting GitHub's own message
	// is safe — and it names things (like a missing scope) this code cannot see.
	std::string detail;
	if (j.is_object()) detail = j.value("message", "");

	if (err)
	{
		switch (statusCode)
		{
		case 401:
			*err = "GitHub rejected the token — it is wrong, expired, or was revoked.";
			break;
		case 403:
			*err = detail.empty()
				? "GitHub refused (403) — the token is likely missing the 'repo' scope."
				: "GitHub refused: " + detail;
			break;
		case 404:
			// GitHub's API answers 404 (not 403) for a fine-grained token that
			// lacks the permission, precisely so it reveals nothing. Translate.
			*err = "GitHub answered 404 — with a valid URL this means the token lacks "
			       "permission to create repositories.";
			break;
		case 422:
			*err = detail.empty()
				? "GitHub could not create the repository — a repository with this "
				  "name probably already exists."
				: "GitHub could not create the repository: " + detail;
			break;
		default:
			*err = "GitHub answered HTTP " + std::to_string(statusCode) +
			       (detail.empty() ? "" : ": " + detail);
			break;
		}
	}
	return false;
}

bool GitHubApi::createRepo(const std::string& token, const std::string& name,
                           bool isPrivate, CreatedRepo& out, std::string* err)
{
	out = CreatedRepo{};
	if (token.empty() || name.empty())
	{
		if (err) *err = "a token and a repository name are both required";
		return false;
	}

	const json body{
		{ "name",     name },
		{ "private",  isPrivate },
		// Never auto-initialise: the local repository already has commits, and a
		// remote-created README would make the very first push a merge.
		{ "auto_init", false },
	};

	const std::vector<std::string> headers = {
		"Authorization: Bearer " + token,
		"Accept: application/vnd.github+json",
		"X-GitHub-Api-Version: 2022-11-28",
		"Content-Type: application/json",
		// GitHub rejects requests without a User-Agent outright, and none of the
		// three HTTPS backends sets a default one.
		"User-Agent: HorizonEngine-Editor",
	};

	HE_SC_INFO("Creating GitHub repository \"%s\" (%s)", name.c_str(),
	           isPrivate ? "private" : "public");

	const HE::Net::HttpsResponse resp = HE::Net::httpsRequest(
		"https://api.github.com/user/repos", "POST", headers, body.dump(), 30000);

	if (!resp.ok)
	{
		if (err) *err = resp.error.empty()
			? "could not reach api.github.com"
			: "could not reach GitHub: " + resp.error;
		return false;
	}
	return parseCreateRepoResponse(resp.statusCode, resp.body, out, err);
}

} // namespace HE::Sc
