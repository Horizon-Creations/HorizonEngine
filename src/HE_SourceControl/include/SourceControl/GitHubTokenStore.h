#pragma once

// ─── Where a GitHub sign-in lives ────────────────────────────────────────────
// One entry in git's credential helper: host github.com, username
// x-access-token, the token as the password. The same entry the token forms
// (Preferences ▸ Source Control) and "Create & push" have always written, and
// the one Report Issue, the repository list and every push read back — which is
// the point: signing in through the browser and pasting a token end up in the
// same place, so nothing downstream can tell them apart.
//
// The one thing that makes this more than three calls: storing, reading and
// forgetting must all ask the SAME helper. A helper configured repo-locally is
// only seen with that repository as the working directory, and with no helper
// configured anywhere the platform default has to be passed per command
// (Linux has none by default; its `cache` helper is only found through
// `cache` again). helperOverrideFor() is that decision, made once here instead
// of at every caller.
//
// Token rules as everywhere in this module: never logged, handed to git via
// stdin only, and the caller wipes its copy.

#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <string>

namespace HE::Sc {

// Outside the class only because a nested struct's member initialisers cannot
// serve as a default argument of the enclosing class's own functions.
struct GitHubTokenStoreOptions
{
	std::string host = "github.com";
	// Non-empty: use exactly this helper and no other (tests point it at a
	// throwaway `store --file=…`, so a developer's keychain is never touched).
	std::string helper;
};

class HE_SC_API GitHubTokenStore {
public:
	static constexpr const char* kHost     = "github.com";
	// What GitHub expects next to a token in the password field. It accepts any
	// name there, but every reader in the editor assumes this one.
	static constexpr const char* kUsername = "x-access-token";

	using Options = GitHubTokenStoreOptions;

	// The helper to pass per command at `root`: empty when one is configured
	// there (system, global or repo-local), the platform default otherwise.
	static std::string helperOverrideFor(const std::filesystem::path& root);

	// Hand `token` to the helper. Inside a git repository a missing helper is
	// first configured repo-locally — exactly what the token form does — so
	// that pushes from that repository find the token too.
	static bool store(const std::filesystem::path& root, const std::string& token,
	                  std::string* err = nullptr, const Options& options = {});

	// The stored token, or false with `token` empty when there is none. Never
	// prompts (see GitCli::fillCredential).
	static bool load(const std::filesystem::path& root, std::string& token,
	                 const Options& options = {});

	// Sign out: remove every entry the helper hands out for the host, then
	// check that it is really gone. True when nothing is stored afterwards —
	// also when nothing was stored to begin with.
	static bool forget(const std::filesystem::path& root, std::string* err = nullptr,
	                   const Options& options = {});
};

} // namespace HE::Sc
