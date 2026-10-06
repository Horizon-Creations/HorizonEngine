#include "SourceControl/GitHubTokenStore.h"

#include "SourceControl/GitCli.h"
#include "ScLog.h"

#include <algorithm>

namespace HE::Sc {

namespace {

void wipe(std::string& s)
{
	std::fill(s.begin(), s.end(), '\0');
	s.clear();
}

std::string helperFor(const std::filesystem::path& root, const GitHubTokenStore::Options& o)
{
	return o.helper.empty() ? GitHubTokenStore::helperOverrideFor(root) : o.helper;
}

// A helper that keeps several entries for one host hands them out one per
// fill. Bounded so a helper that ignores reject cannot hold the UI's worker
// forever.
constexpr int kMaxEntriesToForget = 8;

} // namespace

std::string GitHubTokenStore::helperOverrideFor(const std::filesystem::path& root)
{
	return GitCli::credentialHelper(root).empty() ? GitCli::defaultCredentialHelper()
	                                              : std::string{};
}

bool GitHubTokenStore::store(const std::filesystem::path& root, const std::string& token,
                             std::string* err, const Options& options)
{
	if (token.empty())
	{
		if (err) *err = "no token to store";
		return false;
	}
	if (options.helper.empty() && !GitCli::findRepoRoot(root).empty())
	{
		// Inside a repository: the token form's path, a repo-local helper when
		// none is configured. Not an error when it fails — the per-command
		// default below still stores the token.
		std::string chosen;
		GitCli::ensureCredentialHelper(root, &chosen, nullptr);
	}
	return GitCli::approveCredential(root, options.host, kUsername, token, err,
	                                 helperFor(root, options));
}

bool GitHubTokenStore::load(const std::filesystem::path& root, std::string& token,
                            const Options& options)
{
	std::string user;
	return GitCli::fillCredential(root, options.host, user, token, nullptr,
	                              helperFor(root, options));
}

bool GitHubTokenStore::forget(const std::filesystem::path& root, std::string* err,
                              const Options& options)
{
	const std::string helper = helperFor(root, options);
	for (int i = 0; i < kMaxEntriesToForget; ++i)
	{
		std::string user, secret;
		if (!GitCli::fillCredential(root, options.host, user, secret, nullptr, helper))
			return true;   // nothing (left) to forget

		std::string rejectErr;
		const bool ok = GitCli::rejectCredential(root, options.host, user, secret,
		                                         &rejectErr, helper);
		wipe(secret);
		if (!ok)
		{
			if (err) *err = rejectErr;
			return false;
		}
	}
	HE_SC_WARN("Sign-out: the credential helper still hands out a token for %s",
	           options.host.c_str());
	if (err) *err = "The credential helper did not remove the stored token.";
	return false;
}

} // namespace HE::Sc
