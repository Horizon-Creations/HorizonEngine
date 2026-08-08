#pragma once

// ─── HorizonContentSync — logging, with redaction that is not optional ───────
// Everything here logs to HE::Log::Cat::ContentSync. Unlike HorizonSourceControl
// (see ScLog.h), there is no pattern of token shapes to scrub for — the one
// secret in this module is a single known string (the configured SFTP
// password), so scrub() just removes exact occurrences of it rather than
// pattern-matching. Still a single funnel, for the same reason: the one call
// site that logs a raw string instead of going through scrub() is the one that
// leaks.
#include "ContentSync/SftpCredentials.h"

#include <Diagnostics/Log.h>

#include <string>
#include <string_view>

namespace HE::Cs::detail {

inline std::string scrub(std::string_view text)
{
	std::string out(text);
	const std::string& pw = engineContentEndpoint().password;
	if (pw.empty()) return out;

	std::size_t pos = 0;
	while ((pos = out.find(pw, pos)) != std::string::npos)
	{
		out.replace(pos, pw.size(), "<redacted>");
		pos += 10; // length of "<redacted>" — skip past the replacement
	}
	return out;
}

} // namespace HE::Cs::detail
