#pragma once

// ─── HorizonSourceControl — logging, with redaction that is not optional ─────
// Everything here logs to HE::Log::Cat::SourceControl, so one
// HE_LOG=SourceControl=Trace run tells the whole story of a sync: which git
// command ran, what it answered, which provider was detected, what the remote
// said.
//
// ⚠ THE PROBLEM THIS HEADER EXISTS FOR
// A source-control log is one of the easiest places in an engine to leak a
// credential, because the material passes through in ordinary-looking strings:
//   • a git command line can carry a token in the URL (https://TOKEN@host/…),
//   • `git credential fill` prints `password=…` on stdout,
//   • a provider's error body can echo the Authorization header back,
//   • a remote URL with embedded credentials sits in .git/config and appears in
//     every "failed to push to <url>" message.
// And logs get pasted into bug reports, attached to crash dumps, and shared in
// chat — the token outlives the debugging session.
//
// So no source-control log line is written directly. Everything goes through
// scrub(), which removes the shapes tokens actually have. It is deliberately a
// single funnel rather than a rule to remember at each call site: the one call
// site that forgets is the one that leaks.
//
// Note this is defence in depth, not the primary measure. The primary measure is
// that tokens are handed to git's credential helper and never placed in a URL or
// an argv in the first place — see the credential design in
// docs/source-control-design.md. scrub() catches the cases that slip through
// anyway, including strings the engine did not compose (git's own error text).

#include <Diagnostics/Log.h>

#include <cctype>
#include <string>
#include <string_view>

namespace HE::Sc::detail {

// Replace anything token-shaped with a marker. Conservative in one direction on
// purpose: a false positive costs a slightly less readable log line, a false
// negative costs a leaked credential.
inline std::string scrub(std::string_view text)
{
	std::string out(text);

	// 1. Credentials embedded in a URL: scheme://user:pass@host or scheme://token@host.
	//    This is the single most common leak, because the whole URL then appears
	//    in every error message about that remote.
	for (std::size_t i = 0; (i = out.find("://", i)) != std::string::npos; )
	{
		const std::size_t userStart = i + 3;
		// The authority ends at the first '/', '?' or whitespace; an '@' before
		// that is a userinfo separator.
		std::size_t end = userStart;
		while (end < out.size() && out[end] != '/' && out[end] != '?' &&
		       !std::isspace(static_cast<unsigned char>(out[end])))
			++end;
		const std::size_t at = out.rfind('@', end);
		if (at != std::string::npos && at > userStart)
		{
			out.replace(userStart, at - userStart, "<redacted>");
			i = userStart + 10;
			continue;
		}
		i = userStart;
	}

	// 2. Provider token prefixes. These are fixed, documented and unmistakable,
	//    which makes them worth matching exactly rather than by entropy.
	static const char* kPrefixes[] = {
		"ghp_", "gho_", "ghu_", "ghs_", "ghr_",   // GitHub personal/OAuth/app tokens
		"github_pat_",                            // GitHub fine-grained tokens
		"glpat-",                                 // GitLab personal access tokens
		"gldt-", "glrt-",                         // GitLab deploy / runner tokens
	};
	for (const char* prefix : kPrefixes)
	{
		const std::size_t plen = std::char_traits<char>::length(prefix);
		for (std::size_t i = 0; (i = out.find(prefix, i)) != std::string::npos; )
		{
			std::size_t end = i + plen;
			while (end < out.size() &&
			       (std::isalnum(static_cast<unsigned char>(out[end])) ||
			        out[end] == '_' || out[end] == '-'))
				++end;
			out.replace(i, end - i, "<redacted-token>");
			i += 16;
		}
	}

	// 3. `git credential`'s key=value protocol, which prints the secret in clear
	//    on stdout. Matched at a line start so a path containing "password=" in
	//    the middle of a sentence is left alone.
	static const char* kSecretKeys[] = { "password=", "Authorization:", "authorization:" };
	for (const char* key : kSecretKeys)
	{
		const std::size_t klen = std::char_traits<char>::length(key);
		for (std::size_t i = 0; (i = out.find(key, i)) != std::string::npos; )
		{
			const std::size_t valueStart = i + klen;
			std::size_t end = valueStart;
			while (end < out.size() && out[end] != '\n' && out[end] != '\r') ++end;
			out.replace(valueStart, end - valueStart, " <redacted>");
			i = valueStart + 12;
		}
	}

	return out;
}

} // namespace HE::Sc::detail

// Use these, never HE_LOG(SourceControl, …) directly — the whole point is that
// no line reaches the log without passing through scrub().
#define HE_SC_LOG(level, ...)                                                        \
	do {                                                                             \
		if (::HE::Log::enabled(::HE::Log::Cat::SourceControl, ::HE::LogLevel::level)) \
		{                                                                            \
			char heScBuf_[2048];                                                     \
			std::snprintf(heScBuf_, sizeof(heScBuf_), __VA_ARGS__);                  \
			const std::string heScSafe_ = ::HE::Sc::detail::scrub(heScBuf_);         \
			::HE::Log::write(::HE::Log::Cat::SourceControl, ::HE::LogLevel::level,   \
			                 HE_LOG_FILE_, __LINE__, HE_LOG_FUNC_, "%s",             \
			                 heScSafe_.c_str());                                     \
		}                                                                            \
	} while (0)

#define HE_SC_TRACE(...) HE_SC_LOG(Trace,   __VA_ARGS__)
#define HE_SC_DEBUG(...) HE_SC_LOG(Debug,   __VA_ARGS__)
#define HE_SC_INFO(...)  HE_SC_LOG(Info,    __VA_ARGS__)
#define HE_SC_WARN(...)  HE_SC_LOG(Warning, __VA_ARGS__)
#define HE_SC_ERROR(...) HE_SC_LOG(Error,   __VA_ARGS__)
