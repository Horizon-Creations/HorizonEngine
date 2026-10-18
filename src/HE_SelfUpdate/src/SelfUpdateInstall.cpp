#include "SelfUpdate/SelfUpdateInstall.h"
#include "SelfUpdate/SelfUpdateProcess.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace HE::Su {

namespace {

bool startsWith(const std::string& s, const char* prefix)
{
	const std::size_t n = std::char_traits<char>::length(prefix);
	return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Looks for `bareName` first at each absolute candidate, then along $PATH (with
// .exe appended on Windows) — the layout-correct place for each tool is known
// (System32, /usr/bin), but a candidate list that is wrong on one machine should
// not be the only way to find it.
fs::path findTool(const std::vector<fs::path>& absoluteCandidates, const char* bareName)
{
	std::error_code ec;
	for (const fs::path& c : absoluteCandidates)
		if (!c.empty() && fs::is_regular_file(c, ec)) return c;

	const char* pathEnv = std::getenv("PATH");
	if (!pathEnv) return {};
#if defined(_WIN32)
	const char sep = ';';
	const std::string exeName = std::string(bareName) + ".exe";
#else
	const char sep = ':';
	const std::string exeName = bareName;
#endif
	const std::string p(pathEnv);
	std::size_t start = 0;
	while (start <= p.size())
	{
		std::size_t end = p.find(sep, start);
		if (end == std::string::npos) end = p.size();
		if (end > start)
		{
			const fs::path cand = fs::path(p.substr(start, end - start)) / exeName;
			if (fs::is_regular_file(cand, ec)) return cand;
		}
		start = end + 1;
	}
	return {};
}

fs::path windowsTarPath()
{
	const char* root = std::getenv("SystemRoot");
	return fs::path(root && *root ? root : "C:\\Windows") / "System32" / "tar.exe";
}

// Waits up to 10 minutes — a download has already happened by this point, so an
// extraction that is still slower than that is treated as stuck, not just large.
bool runToolSync(const fs::path& exe, const std::vector<std::string>& args,
                  const fs::path& cwd, std::string& error)
{
	SpawnOptions so;
	so.exe           = exe;
	so.args          = args;
	so.cwd           = cwd;
	so.waitForExitMs = 10 * 60 * 1000;
	const SpawnResult r = spawnDetached(so);
	if (!r.ok) { error = r.error; return false; }
	if (!r.exited) { error = "timed out after 10 minutes"; return false; }
	if (r.exitCode != 0) { error = "exited with code " + std::to_string(r.exitCode); return false; }
	return true;
}

// A directory extracted from a flat archive (Windows' zip of package/Editor/*)
// has many top-level entries; one wrapped in a single folder (Linux's tar with a
// top-level Editor/, a mounted dmg's single .app) has exactly one. Picking the
// install root this way needs no assumption about what that one folder is named.
fs::path collapseSingleTopLevelDir(const fs::path& extractedInto)
{
	std::error_code ec;
	fs::path only;
	int count = 0;
	for (const auto& entry : fs::directory_iterator(extractedInto, ec))
	{
		++count;
		only = entry.path();
		if (count > 1) break;
	}
	if (count == 1 && fs::is_directory(only, ec)) return only;
	return extractedInto;
}

} // namespace

std::string mainExecutableRelativePath(const std::string& platformKey)
{
	if (startsWith(platformKey, "macos")) return "Contents/MacOS/HorizonEditor";
	if (startsWith(platformKey, "windows")) return "HorizonEditor.exe";
	return "HorizonEditor";   // linux-* and anything unrecognised: the flat shape
}

fs::path installRootForRunningBuild(const fs::path& currentExecutable, const std::string& platformKey)
{
	if (currentExecutable.empty()) return {};
	if (startsWith(platformKey, "macos"))
	{
		// .../HorizonEditor.app/Contents/MacOS/HorizonEditor -> .../HorizonEditor.app
		const fs::path macOsDir   = currentExecutable.parent_path();        // Contents/MacOS
		const fs::path contentsDir = macOsDir.parent_path();                 // Contents
		const fs::path bundleDir   = contentsDir.parent_path();              // HorizonEditor.app
		if (macOsDir.filename() != "MacOS" || contentsDir.filename() != "Contents" || bundleDir.empty())
			return {};
		return bundleDir;
	}
	return currentExecutable.parent_path();
}

ExtractResult extractStagedBuild(const fs::path& artifactFile, const fs::path& destParentDir,
                                 const std::string& platformKey)
{
	ExtractResult result;
	std::error_code ec;
	fs::create_directories(destParentDir, ec);

	if (startsWith(platformKey, "windows"))
	{
		const fs::path tar = findTool({ windowsTarPath() }, "tar");
		if (tar.empty()) { result.error = "no tar.exe found (needs Windows 10 1803+ or tar on PATH)"; return result; }
		const fs::path into = destParentDir / "extract";
		fs::create_directories(into, ec);
		std::string err;
		if (!runToolSync(tar, { "-xf", utf8FromPath(artifactFile), "-C", utf8FromPath(into) }, destParentDir, err))
		{
			result.error = "tar.exe failed to extract the update: " + err;
			return result;
		}
		result.payloadRoot = collapseSingleTopLevelDir(into);
	}
	else if (startsWith(platformKey, "linux"))
	{
		const fs::path tar = findTool({ "/usr/bin/tar", "/bin/tar" }, "tar");
		if (tar.empty()) { result.error = "no tar found on this system"; return result; }
		const fs::path into = destParentDir / "extract";
		fs::create_directories(into, ec);
		std::string err;
		if (!runToolSync(tar, { "-xzf", utf8FromPath(artifactFile), "-C", utf8FromPath(into) }, destParentDir, err))
		{
			result.error = "tar failed to extract the update: " + err;
			return result;
		}
		result.payloadRoot = collapseSingleTopLevelDir(into);
	}
	else if (startsWith(platformKey, "macos"))
	{
		const fs::path hdiutil = findTool({ "/usr/bin/hdiutil" }, "hdiutil");
		const fs::path ditto   = findTool({ "/usr/bin/ditto" }, "ditto");
		if (hdiutil.empty() || ditto.empty()) { result.error = "hdiutil/ditto not found"; return result; }

		const fs::path mountPoint = destParentDir / "mnt";
		fs::create_directories(mountPoint, ec);
		std::string attachErr;
		if (!runToolSync(hdiutil, { "attach", "-nobrowse", "-readonly", "-mountpoint",
		                            utf8FromPath(mountPoint), utf8FromPath(artifactFile) },
		                  destParentDir, attachErr))
		{
			result.error = "hdiutil attach failed: " + attachErr;
			return result;
		}

		// Find the single .app bundle at the volume's top level.
		fs::path appSrc;
		for (const auto& entry : fs::directory_iterator(mountPoint, ec))
		{
			if (entry.path().extension() == ".app" && fs::is_directory(entry.path(), ec)) { appSrc = entry.path(); break; }
		}

		std::string copyErr;
		if (!appSrc.empty())
		{
			const fs::path into = destParentDir / "extract";
			fs::create_directories(into, ec);
			const fs::path appDst = into / appSrc.filename();
			if (runToolSync(ditto, { utf8FromPath(appSrc), utf8FromPath(appDst) }, destParentDir, copyErr))
				result.payloadRoot = appDst;
			else
				result.error = "ditto failed to copy the app bundle out of the dmg: " + copyErr;
		}
		else
		{
			result.error = "no .app bundle found on the mounted dmg";
		}

		// Detach regardless of whether the copy worked, so a failed attempt does
		// not leave the dmg mounted for the next attempt to collide with.
		std::string detachErr;
		runToolSync(hdiutil, { "detach", utf8FromPath(mountPoint), "-quiet" }, destParentDir, detachErr);

		if (!result.error.empty()) return result;
	}
	else
	{
		result.error = "unsupported platform key for extraction: " + platformKey;
		return result;
	}

	const fs::path mainExe = result.payloadRoot / pathFromUtf8(mainExecutableRelativePath(platformKey));
	if (!fs::is_regular_file(mainExe, ec))
	{
		result.error = "extracted build is missing its main executable: " + utf8FromPath(mainExe);
		result.payloadRoot.clear();
		return result;
	}
	result.ok = true;
	return result;
}

} // namespace HE::Su
