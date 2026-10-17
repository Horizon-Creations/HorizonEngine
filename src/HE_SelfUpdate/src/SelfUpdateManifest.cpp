#include "SelfUpdate/SelfUpdateManifest.h"
#include "SelfUpdate/SelfUpdateVersion.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace HE::Su {

namespace {

bool isNameChar(char c)
{
	const unsigned char u = static_cast<unsigned char>(c);
	return std::isalnum(u) || c == '.' || c == '_' || c == '+' || c == '-';
}

bool isPlatformKey(const std::string& s)
{
	if (s.empty() || s.size() > 32) return false;
	for (char c : s)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if (!(std::islower(u) || std::isdigit(u) || c == '-' || c == '_')) return false;
	}
	return true;
}

bool isHex64(const std::string& s)
{
	if (s.size() != 64) return false;
	for (char c : s)
		if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
	return true;
}

// Reads an optional string field; a present field of another type is an error
// rather than silently ignored, so a manifest typo shows up at the first check.
bool optionalString(const json& obj, const char* key, std::string& out, std::string& error)
{
	const auto it = obj.find(key);
	if (it == obj.end() || it->is_null()) return true;
	if (!it->is_string())
	{
		error = std::string("'") + key + "' must be a string";
		return false;
	}
	out = it->get<std::string>();
	return true;
}

} // namespace

std::string UpdateArtifact::fileName() const
{
	const std::size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

const UpdateArtifact* UpdateManifest::findArtifact(const std::string& platform) const
{
	for (const UpdateArtifact& a : artifacts)
		if (a.platform == platform) return &a;
	return nullptr;
}

bool isSafeRelativePath(const std::string& path)
{
	if (path.empty() || path.size() > 512) return false;
	if (path.front() == '/' || path.back() == '/') return false;

	std::size_t start = 0;
	for (;;)
	{
		const std::size_t at = path.find('/', start);
		const std::string seg = path.substr(start, at == std::string::npos ? std::string::npos : at - start);
		if (seg.empty() || seg == "." || seg == "..") return false;
		for (char c : seg)
			if (!isNameChar(c)) return false;
		// A Windows file name may not end in '.', and a segment of only dots
		// ("...") is a name nobody means.
		if (seg.back() == '.') return false;
		if (at == std::string::npos) break;
		start = at + 1;
	}
	return true;
}

bool parseUpdateManifest(const std::string& text, UpdateManifest& out, std::string& error)
{
	json root;
	try
	{
		root = json::parse(text);
	}
	catch (const json::parse_error& e)
	{
		error = std::string("manifest is not valid JSON: ") + e.what();
		return false;
	}
	if (!root.is_object())
	{
		error = "manifest must be a JSON object";
		return false;
	}

	UpdateManifest m;

	if (const auto it = root.find("schema"); it != root.end())
	{
		if (!it->is_number_integer())
		{
			error = "'schema' must be an integer";
			return false;
		}
		m.schema = it->get<int>();
	}
	if (m.schema < 1 || m.schema > kManifestSchema)
	{
		error = "manifest schema " + std::to_string(m.schema) + " is not one this editor understands (it knows up to " +
		        std::to_string(kManifestSchema) + ")";
		return false;
	}

	if (!optionalString(root, "version", m.version, error)) return false;
	if (!optionalString(root, "codename", m.codename, error)) return false;
	if (!optionalString(root, "released", m.released, error)) return false;
	if (!optionalString(root, "notes", m.notes, error)) return false;

	Version parsed;
	if (m.version.empty() || !parseVersion(m.version, parsed))
	{
		error = "manifest 'version' is missing or not a version: '" + m.version + "'";
		return false;
	}
	// The version names a staging folder later, so it is held to the same
	// allow-list as a path segment ("0.8.0-rc.1" passes, "1.0/../x" does not).
	if (m.version.find('/') != std::string::npos || !isSafeRelativePath(m.version))
	{
		error = "manifest 'version' contains characters that are not allowed: '" + m.version + "'";
		return false;
	}

	const auto arts = root.find("artifacts");
	if (arts == root.end() || !arts->is_array() || arts->empty())
	{
		error = "manifest has no 'artifacts'";
		return false;
	}

	for (const json& a : *arts)
	{
		if (!a.is_object())
		{
			error = "an entry of 'artifacts' is not an object";
			return false;
		}
		UpdateArtifact art;
		if (!optionalString(a, "platform", art.platform, error)) return false;
		if (!optionalString(a, "path", art.path, error)) return false;
		if (!optionalString(a, "sha256", art.sha256, error)) return false;

		if (!isPlatformKey(art.platform))
		{
			error = "artifact has no usable 'platform' ('" + art.platform + "')";
			return false;
		}
		if (!isSafeRelativePath(art.path))
		{
			error = "artifact '" + art.platform + "' has an unsafe or empty 'path'";
			return false;
		}
		if (!isHex64(art.sha256))
		{
			error = "artifact '" + art.platform + "' needs a 64-character hex 'sha256'";
			return false;
		}
		std::transform(art.sha256.begin(), art.sha256.end(), art.sha256.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		const auto sz = a.find("size");
		if (sz == a.end() || !sz->is_number_unsigned() || sz->get<std::uint64_t>() == 0)
		{
			error = "artifact '" + art.platform + "' needs a positive integer 'size'";
			return false;
		}
		art.size = sz->get<std::uint64_t>();

		if (m.findArtifact(art.platform))
		{
			error = "platform '" + art.platform + "' appears twice in 'artifacts'";
			return false;
		}
		m.artifacts.push_back(std::move(art));
	}

	out = std::move(m);
	return true;
}

std::string serializeUpdateManifest(const UpdateManifest& m)
{
	json arts = json::array();
	for (const UpdateArtifact& a : m.artifacts)
		arts.push_back({ { "platform", a.platform }, { "path", a.path }, { "sha256", a.sha256 }, { "size", a.size } });

	json root;
	root["schema"]  = m.schema;
	root["version"] = m.version;
	if (!m.codename.empty()) root["codename"] = m.codename;
	if (!m.released.empty()) root["released"] = m.released;
	if (!m.notes.empty())    root["notes"] = m.notes;
	root["artifacts"] = std::move(arts);
	return root.dump(2);
}

} // namespace HE::Su
