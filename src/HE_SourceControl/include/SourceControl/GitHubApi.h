#pragma once

// ─── The editor's GitHub REST calls ──────────────────────────────────────────
// Five, and no more: create a repository (source-control setup), list the
// token owner's repositories (clone an existing one as a project), and — for
// Help ▸ Report Issue — identify the token's owner, upload a log as a secret
// gist, and file the issue. Everything else (clone, push, pull, LFS) goes
// through git itself, authenticated by the credential helper the token is
// handed to.
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
#include <vector>

namespace HE::Sc {

struct CreatedRepo
{
	std::string cloneUrl;   // "https://github.com/user/project.git"
	std::string fullName;   // "user/project"
};

// One entry of GET /user/repos — what the clone picker shows and needs.
struct RepoListEntry
{
	std::string name;           // "project"
	std::string fullName;       // "user/project"
	std::string cloneUrl;       // "https://github.com/user/project.git" — never carries a token
	std::string defaultBranch;  // "main"; empty for a repository with no commits yet
	std::string updatedAt;      // ISO 8601 UTC, "2026-09-30T12:00:00Z" — sorts as a string
	bool        isPrivate = false;
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
	// Page size of listRepos — GitHub's maximum for /user/repos — and the page
	// cap that keeps a server ignoring `page` from looping forever (10 000
	// repositories; hitting it is logged).
	static constexpr int kListReposPerPage  = 100;
	static constexpr int kListReposMaxPages = 100;

	// All of these are blocking (one HTTPS round trip; listRepos one per page
	// of 100) — worker thread only.
	static bool createRepo(const std::string& token, const std::string& name,
	                       bool isPrivate, CreatedRepo& out, std::string* err = nullptr);

	// GET /user/repos — every repository the token can see as its owner,
	// collaborator or organisation member, most recently updated first. Pages
	// are walked with the `page` parameter until one comes back short (the
	// HTTPS layer does not surface the Link header). On any failure `out` is
	// left empty rather than holding a silently truncated list.
	static bool listRepos(const std::string& token, std::vector<RepoListEntry>& out,
	                      std::string* err = nullptr);

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
	// One page of /user/repos. `out` is replaced with that page's entries;
	// entries without a clone URL or name are skipped, not fatal. A 200 whose
	// body is not an array is a failure. `rawCount` receives the array's own
	// length, skipped entries included — that, not out.size(), is what tells
	// the pager whether the page was full.
	static bool parseListReposResponse(int statusCode, const std::string& body,
	                                   std::vector<RepoListEntry>& out,
	                                   std::string* err = nullptr,
	                                   int* rawCount = nullptr);
	static bool parseUserResponse(int statusCode, const std::string& body,
	                              GitHubUser& out, std::string* err = nullptr);
	static bool parseCreateGistResponse(int statusCode, const std::string& body,
	                                    CreatedGist& out, std::string* err = nullptr);
	static bool parseCreateIssueResponse(int statusCode, const std::string& body,
	                                     CreatedIssue& out, std::string* err = nullptr);
};

} // namespace HE::Sc
