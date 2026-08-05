#pragma once

// ─── The editor's GitHub REST calls ──────────────────────────────────────────
// Four, and no more: create a repository (source-control setup), and — for
// Help ▸ Report Issue — identify the token's owner, upload a log as a secret
// gist, and file the issue. Everything else (push, pull, LFS) goes through git
// itself, authenticated by the credential helper the token is handed to.
// Keeping the API surface this small is what keeps the token handling
// auditable.
//
// Token rules (the same ones the whole source-control layer follows):
//   • never placed in a URL — it would land in .git/config and every error
//   • never logged — errors are built from the RESPONSE, which contains no token
//   • held only for the duration of the operation, then wiped by the caller
//
// On attachments: GitHub has no REST endpoint for attaching a file to an issue
// — the web UI's uploader is browser-session-only and rejects tokens outright.
// A secret gist is the supported way to get a whole log file onto GitHub from
// an application, which is why createGist exists here at all.

#include "SourceControl/ScCommon.h"

#include <string>

namespace HE::Sc {

struct CreatedRepo
{
	std::string cloneUrl;   // "https://github.com/user/project.git"
	std::string fullName;   // "user/project"
};

struct GitHubUser
{
	std::string login;      // "octocat"
};

struct CreatedGist
{
	std::string htmlUrl;    // "https://gist.github.com/octocat/abc123"
};

struct CreatedIssue
{
	std::string htmlUrl;    // "https://github.com/owner/repo/issues/42"
	int         number = 0;
};

class HE_SC_API GitHubApi {
public:
	// All four are blocking (one HTTPS round trip) — worker thread only.
	static bool createRepo(const std::string& token, const std::string& name,
	                       bool isPrivate, CreatedRepo& out, std::string* err = nullptr);

	// GET /user — who a token belongs to. Called before anything is posted so
	// the user can see which account is about to speak for them, and so an
	// expired token is caught before a report is composed against it.
	static bool currentUser(const std::string& token, GitHubUser& out,
	                        std::string* err = nullptr);

	// POST /gists, secret by default. `isPublic` true makes it listed and
	// searchable — a log can carry file paths and project names, so the caller
	// had better mean it.
	static bool createGist(const std::string& token, const std::string& description,
	                       const std::string& fileName, const std::string& content,
	                       bool isPublic, CreatedGist& out, std::string* err = nullptr);

	// POST /repos/{owner}/{repo}/issues. Any account with read access to a
	// public repository may file an issue; a token restricted to the user's own
	// repositories may not, and GitHub answers 404 rather than 403 for it.
	static bool createIssue(const std::string& token, const std::string& owner,
	                        const std::string& repo, const std::string& title,
	                        const std::string& body, CreatedIssue& out,
	                        std::string* err = nullptr);

	// Pure response interpretation, exposed so the error mapping is testable
	// without a network: the success code → filled result; 401/403/404/422 →
	// the message a user can act on.
	static bool parseCreateRepoResponse(int statusCode, const std::string& body,
	                                    CreatedRepo& out, std::string* err = nullptr);
	static bool parseUserResponse(int statusCode, const std::string& body,
	                              GitHubUser& out, std::string* err = nullptr);
	static bool parseCreateGistResponse(int statusCode, const std::string& body,
	                                    CreatedGist& out, std::string* err = nullptr);
	static bool parseCreateIssueResponse(int statusCode, const std::string& body,
	                                     CreatedIssue& out, std::string* err = nullptr);
};

} // namespace HE::Sc
