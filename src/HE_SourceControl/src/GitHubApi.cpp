#include "SourceControl/GitHubApi.h"

#include "ScLog.h"

#include <Net/HttpsClient.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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

// ─── Shared request plumbing ─────────────────────────────────────────────────

namespace {

std::vector<std::string> apiHeaders(const std::string& token)
{
	return {
		"Authorization: Bearer " + token,
		"Accept: application/vnd.github+json",
		"X-GitHub-Api-Version: 2022-11-28",
		"Content-Type: application/json",
		// GitHub rejects requests without a User-Agent outright, and none of the
		// three HTTPS backends sets a default one.
		"User-Agent: HorizonEngine-Editor",
	};
}

// GitHub's own "message" field, when the body is the error object it documents.
std::string apiMessage(const json& j)
{
	return j.is_object() ? j.value("message", "") : std::string();
}

// The failures every one of these calls can hit, in the user's language rather
// than HTTP's. `missingScope` names what a 403 most likely lacks.
void mapCommonError(int statusCode, const json& j, const char* missingScope,
                    std::string* err)
{
	if (!err) return;
	const std::string detail = apiMessage(j);
	switch (statusCode)
	{
	case 401:
		*err = "GitHub rejected the token — it is wrong, expired, or was revoked.";
		break;
	case 403:
		*err = detail.empty()
			? std::string("GitHub refused (403) — the token is likely missing the '") +
			  missingScope + "' scope."
			: "GitHub refused: " + detail;
		break;
	case 404:
		// GitHub answers 404 rather than 403 for a token that lacks permission,
		// precisely so it reveals nothing about what exists. Translate, or the
		// user goes hunting for a typo in a URL this code built itself.
		*err = std::string("GitHub answered 404 — the token is valid but lacks the '") +
		       missingScope + "' permission for this.";
		break;
	case 410:
		*err = "Issues are disabled for this repository.";
		break;
	case 422:
		*err = detail.empty() ? "GitHub rejected the request as invalid."
		                      : "GitHub rejected the request: " + detail;
		break;
	default:
		*err = "GitHub answered HTTP " + std::to_string(statusCode) +
		       (detail.empty() ? "" : ": " + detail);
		break;
	}
}

HE::Net::HttpsResponse apiPost(const std::string& url, const std::string& token,
                               const std::string& body, std::string* err)
{
	const HE::Net::HttpsResponse resp =
		HE::Net::httpsRequest(url, "POST", apiHeaders(token), body, 30000);
	if (!resp.ok && err)
	{
		*err = resp.error.empty() ? "could not reach api.github.com"
		                          : "could not reach GitHub: " + resp.error;
	}
	return resp;
}

} // namespace

// ─── Who the token belongs to ────────────────────────────────────────────────

bool GitHubApi::parseUserResponse(int statusCode, const std::string& body,
                                  GitHubUser& out, std::string* err)
{
	out = GitHubUser{};
	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);

	if (statusCode == 200)
	{
		if (j.is_object()) out.login = j.value("login", "");
		if (out.login.empty())
		{
			if (err) *err = "GitHub accepted the token but did not name an account";
			return false;
		}
		return true;
	}
	mapCommonError(statusCode, j, "read:user", err);
	return false;
}

bool GitHubApi::currentUser(const std::string& token, GitHubUser& out, std::string* err)
{
	out = GitHubUser{};
	if (token.empty())
	{
		if (err) *err = "a token is required";
		return false;
	}

	const HE::Net::HttpsResponse resp =
		HE::Net::httpsRequest("https://api.github.com/user", "GET", apiHeaders(token), {}, 15000);
	if (!resp.ok)
	{
		if (err) *err = resp.error.empty()
			? "could not reach api.github.com"
			: "could not reach GitHub: " + resp.error;
		return false;
	}
	return parseUserResponse(resp.statusCode, resp.body, out, err);
}

// ─── The log, as a secret gist ───────────────────────────────────────────────

bool GitHubApi::parseCreateGistResponse(int statusCode, const std::string& body,
                                        CreatedGist& out, std::string* err)
{
	out = CreatedGist{};
	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);

	if (statusCode == 201)
	{
		if (j.is_object()) out.htmlUrl = j.value("html_url", "");
		if (out.htmlUrl.empty())
		{
			if (err) *err = "GitHub created the gist but did not return its address";
			return false;
		}
		return true;
	}
	mapCommonError(statusCode, j, "gist", err);
	return false;
}

bool GitHubApi::createGist(const std::string& token, const std::string& description,
                           const std::string& fileName, const std::string& content,
                           bool isPublic, CreatedGist& out, std::string* err)
{
	out = CreatedGist{};
	if (token.empty() || fileName.empty() || content.empty())
	{
		if (err) *err = "a token, a file name and content are all required";
		return false;
	}

	const json body{
		{ "description", description },
		// "secret", in GitHub's terms: unlisted and unsearchable, but anyone
		// with the link can read it. That is the strongest a gist gets, and the
		// caller has to have told the user so.
		{ "public", isPublic },
		{ "files", { { fileName, { { "content", content } } } } },
	};

	HE_SC_INFO("Uploading %zu bytes as a %s gist", content.size(),
	           isPublic ? "public" : "secret");

	const HE::Net::HttpsResponse resp =
		apiPost("https://api.github.com/gists", token, body.dump(), err);
	if (!resp.ok) return false;
	return parseCreateGistResponse(resp.statusCode, resp.body, out, err);
}

// ─── The issue ───────────────────────────────────────────────────────────────

bool GitHubApi::parseCreateIssueResponse(int statusCode, const std::string& body,
                                         CreatedIssue& out, std::string* err)
{
	out = CreatedIssue{};
	const json j = json::parse(body, nullptr, /*allow_exceptions=*/false);

	if (statusCode == 201)
	{
		if (j.is_object())
		{
			out.htmlUrl = j.value("html_url", "");
			out.number   = j.value("number", 0);
		}
		if (out.htmlUrl.empty())
		{
			if (err) *err = "GitHub created the issue but did not return its address";
			return false;
		}
		return true;
	}
	mapCommonError(statusCode, j, "issues", err);
	return false;
}

bool GitHubApi::createIssue(const std::string& token, const std::string& owner,
                            const std::string& repo, const std::string& title,
                            const std::string& body, CreatedIssue& out, std::string* err)
{
	out = CreatedIssue{};
	if (token.empty() || owner.empty() || repo.empty() || title.empty())
	{
		if (err) *err = "a token, a repository and a title are all required";
		return false;
	}
	// owner/repo go into the request PATH. They are compile-time constants at
	// every call site today, but a path segment assembled from a string is
	// exactly the thing that stops being constant later — so check rather than
	// escape: a name outside GitHub's own alphabet is a bug, not user input.
	const auto nameIsSafe = [](const std::string& s) {
		return !s.empty() && s.find_first_not_of(
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._") ==
			std::string::npos;
	};
	if (!nameIsSafe(owner) || !nameIsSafe(repo))
	{
		if (err) *err = "invalid repository name";
		return false;
	}

	const json payload{
		{ "title", title },
		{ "body",  body  },
	};

	HE_SC_INFO("Filing an issue on %s/%s", owner.c_str(), repo.c_str());

	const HE::Net::HttpsResponse resp = apiPost(
		"https://api.github.com/repos/" + owner + "/" + repo + "/issues",
		token, payload.dump(), err);
	if (!resp.ok) return false;
	return parseCreateIssueResponse(resp.statusCode, resp.body, out, err);
}

} // namespace HE::Sc
