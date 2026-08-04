#pragma once

// ─── Creating a repository on GitHub ─────────────────────────────────────────
// One REST call: POST /user/repos with a personal access token. Deliberately
// the ONLY thing the editor does against the GitHub API — everything after
// (push, pull, LFS) goes through git itself, authenticated by the credential
// helper this token is handed to. Keeping the API surface this small is what
// keeps the token handling auditable.
//
// Token rules (the same ones the whole source-control layer follows):
//   • never placed in a URL — it would land in .git/config and every error
//   • never logged — errors are built from the RESPONSE, which contains no token
//   • held only for the duration of the operation, then wiped by the caller

#include "SourceControl/ScCommon.h"

#include <string>

namespace HE::Sc {

struct CreatedRepo
{
	std::string cloneUrl;   // "https://github.com/user/project.git"
	std::string fullName;   // "user/project"
};

class HE_SC_API GitHubApi {
public:
	// Blocking (one HTTPS round trip) — worker thread only.
	static bool createRepo(const std::string& token, const std::string& name,
	                       bool isPrivate, CreatedRepo& out, std::string* err = nullptr);

	// Pure response interpretation, exposed so the error mapping is testable
	// without a network: 201 → filled result; 401/403/404/422 → the message a
	// user can act on.
	static bool parseCreateRepoResponse(int statusCode, const std::string& body,
	                                    CreatedRepo& out, std::string* err = nullptr);
};

} // namespace HE::Sc
