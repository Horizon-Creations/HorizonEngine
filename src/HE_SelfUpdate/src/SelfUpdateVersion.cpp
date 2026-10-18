#include "SelfUpdate/SelfUpdateVersion.h"

#include <cctype>
#include <vector>

namespace HE::Su {

namespace {

// One decimal field. Capped at 9 digits so it always fits a uint32_t.
bool parseField(const std::string& text, std::uint32_t& out)
{
	if (text.empty() || text.size() > 9) return false;
	std::uint32_t value = 0;
	for (char c : text)
	{
		if (!std::isdigit(static_cast<unsigned char>(c))) return false;
		value = value * 10 + static_cast<std::uint32_t>(c - '0');
	}
	out = value;
	return true;
}

std::vector<std::string> split(const std::string& text, char sep)
{
	std::vector<std::string> parts;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t at = text.find(sep, start);
		if (at == std::string::npos) { parts.push_back(text.substr(start)); break; }
		parts.push_back(text.substr(start, at - start));
		start = at + 1;
	}
	return parts;
}

bool allDigits(const std::string& s)
{
	if (s.empty()) return false;
	for (char c : s)
		if (!std::isdigit(static_cast<unsigned char>(c))) return false;
	return true;
}

// Semver precedence for the dot-separated pre-release identifiers.
int comparePrerelease(const std::string& a, const std::string& b)
{
	const std::vector<std::string> pa = split(a, '.');
	const std::vector<std::string> pb = split(b, '.');
	for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i)
	{
		const bool na = allDigits(pa[i]);
		const bool nb = allDigits(pb[i]);
		if (na && nb)
		{
			// Equal-length digit runs compare lexically; otherwise the longer one
			// (no leading zeros assumed, but stripped below to be safe) is larger.
			std::string x = pa[i], y = pb[i];
			while (x.size() > 1 && x.front() == '0') x.erase(x.begin());
			while (y.size() > 1 && y.front() == '0') y.erase(y.begin());
			if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
			if (x != y) return x < y ? -1 : 1;
		}
		else if (na != nb)
		{
			return na ? -1 : 1;   // numeric identifiers sort below alphanumeric ones
		}
		else if (pa[i] != pb[i])
		{
			return pa[i] < pb[i] ? -1 : 1;
		}
	}
	if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;
	return 0;
}

} // namespace

std::string Version::toString() const
{
	std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	if (!prerelease.empty()) s += "-" + prerelease;
	return s;
}

bool parseVersion(const std::string& textIn, Version& out)
{
	std::string text = textIn;
	if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.erase(text.begin());

	// +BUILD metadata does not take part in precedence.
	if (const std::size_t plus = text.find('+'); plus != std::string::npos) text.erase(plus);

	std::string pre;
	if (const std::size_t dash = text.find('-'); dash != std::string::npos)
	{
		pre  = text.substr(dash + 1);
		text = text.substr(0, dash);
		if (pre.empty()) return false;
		for (const std::string& id : split(pre, '.'))
			if (id.empty()) return false;
	}

	const std::vector<std::string> fields = split(text, '.');
	if (fields.empty() || fields.size() > 3) return false;

	Version v;
	std::uint32_t* slots[3] = { &v.major, &v.minor, &v.patch };
	for (std::size_t i = 0; i < fields.size(); ++i)
		if (!parseField(fields[i], *slots[i])) return false;
	v.prerelease = std::move(pre);

	out = std::move(v);
	return true;
}

int compareVersions(const Version& a, const Version& b)
{
	if (a.major != b.major) return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

	// A release is newer than any pre-release of the same MAJOR.MINOR.PATCH.
	if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
	if (a.prerelease.empty()) return 0;
	return comparePrerelease(a.prerelease, b.prerelease);
}

} // namespace HE::Su
