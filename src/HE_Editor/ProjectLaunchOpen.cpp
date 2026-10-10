#include "ProjectLaunchOpen.h"

#include <cctype>
#include <system_error>

namespace ProjectLaunchOpen
{

namespace fs = std::filesystem;

namespace {

bool        s_hasRequest = false;
std::string s_request;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int hexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// "file:///Users/a/My%20Game.heproj" → "/Users/a/My Game.heproj". A host other
// than none or "localhost" names another machine and is left alone — it will
// simply not be found.
std::string fromFileUrl(std::string s)
{
	constexpr std::string_view kScheme = "file://";
	if (s.size() < kScheme.size()) return s;
	for (size_t i = 0; i < kScheme.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(s[i])) != kScheme[i]) return s;
	s.erase(0, kScheme.size());
	if (s.rfind("localhost/", 0) == 0) s.erase(0, 9);

	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size())
		{
			const int hi = hexValue(s[i + 1]);
			const int lo = hexValue(s[i + 2]);
			if (hi >= 0 && lo >= 0)
			{
				out += static_cast<char>(hi * 16 + lo);
				i += 2;
				continue;
			}
		}
		out += s[i];
	}
#ifdef _WIN32
	// file:///C:/… keeps the slash in front of the drive letter.
	if (out.size() >= 3 && out[0] == '/' && std::isalpha(static_cast<unsigned char>(out[1])) &&
	    out[2] == ':')
		out.erase(0, 1);
#endif
	return out;
}

bool hasProjectExtension(const fs::path& p)
{
	std::string ext = p.extension().string();
	for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".heproj";
}

} // namespace

const char* describe(PathError e)
{
	switch (e)
	{
	case PathError::None:           return "is a project file";
	case PathError::Empty:          return "is empty";
	case PathError::WrongExtension: return "is not a .heproj project file";
	case PathError::NotFound:       return "does not exist";
	case PathError::NotAFile:       return "is not a file";
	}
	return "cannot be opened";
}

ParsedPath parse(std::string_view raw, const fs::path& cwd)
{
	ParsedPath r;
	size_t b = 0, e = raw.size();
	while (b < e && isSpace(raw[b]))     ++b;
	while (e > b && isSpace(raw[e - 1])) --e;
	std::string s(raw.substr(b, e - b));
	if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
		s = s.substr(1, s.size() - 2);
	s = fromFileUrl(std::move(s));
	if (s.empty()) { r.error = PathError::Empty; return r; }

	fs::path p(s);
	if (p.is_relative()) p = cwd / p;
	p = p.lexically_normal();
	r.path = p.string();

	if (!hasProjectExtension(p)) { r.error = PathError::WrongExtension; return r; }
	std::error_code ec;
	const fs::file_status st = fs::status(p, ec);
	if (ec || !fs::exists(st))  { r.error = PathError::NotFound; return r; }
	if (!fs::is_regular_file(st)) { r.error = PathError::NotAFile; return r; }
	r.error = PathError::None;
	return r;
}

LaunchPick pickFromArguments(const std::vector<std::string>& args, const fs::path& cwd)
{
	LaunchPick pick;
	for (const std::string& a : args)
	{
		ParsedPath p = parse(a, cwd);
		if (!p.ok())              pick.rejected.emplace_back(a, p.error);
		else if (pick.project.empty()) pick.project = std::move(p.path);
		else if (!samePath(pick.project, p.path)) pick.ignored.push_back(std::move(p.path));
	}
	return pick;
}

bool samePath(const std::string& a, const std::string& b)
{
	if (a.empty() || b.empty()) return false;
	std::error_code ec;
	if (fs::exists(a, ec) && fs::exists(b, ec))
	{
		const bool eq = fs::equivalent(a, b, ec);
		if (!ec) return eq;
	}
	const fs::path na = fs::absolute(a, ec).lexically_normal();
	const fs::path nb = fs::absolute(b, ec).lexically_normal();
	return na == nb;
}

Action decide(bool projectLoaded, const std::string& currentProjectPath,
              const std::string& requested)
{
	if (!projectLoaded) return Action::OpenDirect;
	if (samePath(currentProjectPath, requested)) return Action::AlreadyOpen;
	return Action::OpenGuarded;
}

bool post(const std::string& absPath)
{
	if (s_hasRequest || absPath.empty()) return false;
	s_hasRequest = true;
	s_request    = absPath;
	return true;
}

bool take(std::string& absPath)
{
	if (!s_hasRequest) return false;
	s_hasRequest = false;
	absPath = std::move(s_request);
	s_request.clear();
	return true;
}

bool pending() { return s_hasRequest; }

} // namespace ProjectLaunchOpen
