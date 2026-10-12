#pragma once

// ─── Version strings, compared as numbers ─────────────────────────────────────
// "0.10.0" is newer than "0.9.0", which a string compare gets backwards — and a
// self-updater that thinks 0.9 is current while 0.10 is installed would offer a
// downgrade as an update. So versions are parsed and compared field by field.
//
// Accepted shape: MAJOR[.MINOR[.PATCH]][-PRERELEASE][+BUILD], optionally with a
// leading 'v'. A missing MINOR/PATCH counts as 0. BUILD metadata is ignored, as
// semver says. A pre-release ("0.8.0-rc.1") sorts BELOW its release ("0.8.0"),
// and two pre-releases compare identifier by identifier (numeric identifiers
// numerically and below alphanumeric ones; a shorter list below a longer one
// that starts the same).

#include "SelfUpdate/SuCommon.h"

#include <cstdint>
#include <string>

namespace HE::Su {

struct Version
{
	std::uint32_t major = 0;
	std::uint32_t minor = 0;
	std::uint32_t patch = 0;
	std::string   prerelease;   // empty for a release

	// "0.8.0" or "0.8.0-rc.1" — the normalised form, without 'v' or +BUILD.
	std::string toString() const;
};

// False (and `out` untouched) for anything that is not a version: empty text,
// a non-numeric core field, a field too long to hold, an empty pre-release
// after the '-'.
HE_SU_API bool parseVersion(const std::string& text, Version& out);

// <0 when a is older than b, 0 when equal in precedence, >0 when newer.
HE_SU_API int compareVersions(const Version& a, const Version& b);

} // namespace HE::Su
