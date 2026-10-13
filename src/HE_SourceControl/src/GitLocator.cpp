#include "SourceControl/GitLocator.h"

#include "ScLog.h"

#include <Platform/Process.h>

#include <mutex>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace HE::Sc {
namespace {

// PATH strings are UTF-8 here. Built through u8string so a non-ASCII directory
// survives on Windows, where a plain std::string would be read as the ANSI code page.
std::filesystem::path pathFromUtf8(std::string_view s)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

#ifdef _WIN32

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
	                                    nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
	if (n > 0)
		::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
		                      s.data(), n, nullptr, nullptr);
	return s;
}

// The Path value as it is stored NOW, not as it was when the editor started.
// That is the difference that matters after an install: the Git for Windows
// installer and winget write here, and nobody tells running processes.
std::string registryPath(HKEY root, const wchar_t* subKey)
{
	DWORD bytes = 0;
	const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
	if (::RegGetValueW(root, subKey, L"Path", flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
	    bytes == 0)
		return {};

	std::wstring raw(bytes / sizeof(wchar_t), L'\0');
	if (::RegGetValueW(root, subKey, L"Path", flags, nullptr, raw.data(), &bytes) != ERROR_SUCCESS)
		return {};
	raw.resize(wcsnlen(raw.c_str(), raw.size()));

	// User PATH entries are usually REG_EXPAND_SZ with %USERPROFILE% and friends.
	const DWORD need = ::ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
	if (need == 0) return narrow(raw);
	std::wstring expanded(need, L'\0');
	::ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), need);
	expanded.resize(wcsnlen(expanded.c_str(), expanded.size()));
	return narrow(expanded);
}

std::optional<std::filesystem::path> envDir(const wchar_t* name)
{
	const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
	if (need == 0) return std::nullopt;
	std::wstring v(need, L'\0');
	const DWORD n = ::GetEnvironmentVariableW(name, v.data(), need);
	if (n == 0 || n >= need) return std::nullopt;
	v.resize(n);
	return std::filesystem::path(v);
}

std::optional<std::filesystem::path> locateGitUncached()
{
	// 1. What the OS loader would find with the environment this process has.
	//    Only an .exe counts: Proc::run uses CreateProcess, which cannot start the
	//    git.cmd wrappers some installs put first on PATH.
	if (const auto found = HE::Proc::which("git"))
	{
		if (found->extension() == ".exe") return found;
	}

	// 2. PATH as the registry holds it now (machine, then user — the order
	//    Windows builds a new process's PATH in), then Git for Windows' own
	//    install folders for an install that did not touch PATH at all.
	std::vector<std::filesystem::path> dirs = splitPathList(
		registryPath(HKEY_LOCAL_MACHINE,
		             L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"), ';');
	for (auto& d : splitPathList(registryPath(HKEY_CURRENT_USER, L"Environment"), ';'))
		dirs.push_back(std::move(d));

	for (const wchar_t* var : { L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)" })
		if (const auto base = envDir(var)) dirs.push_back(*base / "Git" / "cmd");
	if (const auto local = envDir(L"LOCALAPPDATA"))
		dirs.push_back(*local / "Programs" / "Git" / "cmd");

	return findExecutableInDirs(dirs, "git.exe");
}

bool isRootUser() { return false; }

#else // POSIX

std::optional<std::filesystem::path> locateGitUncached()
{
	// which() augments PATH with the Homebrew prefixes first, so a Finder-launched
	// editor sees /opt/homebrew/bin/git the way a terminal does.
	return HE::Proc::which("git");
}

bool isRootUser() { return ::geteuid() == 0; }

#endif

// The cached answer. The outer optional is "resolved yet?", the inner one the answer.
std::mutex                                           g_locMutex;
std::optional<std::optional<std::filesystem::path>> g_location;

void appendCommand(std::string& line, const std::string& command, bool withSudo)
{
	// "a && b" with sudo needs it on every part, not just the first.
	std::size_t start = 0;
	while (start <= command.size())
	{
		std::size_t end = command.find(" && ", start);
		const std::string part = command.substr(start, end == std::string::npos ? std::string::npos
		                                                                         : end - start);
		if (!line.empty()) line += " && ";
		if (withSudo) line += "sudo ";
		line += part;
		if (end == std::string::npos) break;
		start = end + 4;
	}
}

} // namespace

// ── The executable ───────────────────────────────────────────────────────────

std::optional<std::filesystem::path> gitExecutable()
{
	std::lock_guard<std::mutex> lock(g_locMutex);
	if (!g_location)
	{
		g_location = locateGitUncached();
		if (*g_location) HE_SC_DEBUG("git located at %s", (*g_location)->string().c_str());
		else             HE_SC_INFO("git was not found on PATH or in its known install folders");
	}
	return *g_location;
}

void invalidateGitLocation()
{
	std::lock_guard<std::mutex> lock(g_locMutex);
	g_location.reset();
}

std::filesystem::path gitLaunchPath()
{
	if (auto exe = gitExecutable()) return *exe;
	return "git";
}

// ── Pure helpers ─────────────────────────────────────────────────────────────

std::vector<std::filesystem::path> splitPathList(std::string_view list, char separator)
{
	std::vector<std::filesystem::path> dirs;
	while (!list.empty())
	{
		const std::size_t sep = list.find(separator);
		std::string_view entry = list.substr(0, sep);

		while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.remove_prefix(1);
		while (!entry.empty() && (entry.back()  == ' ' || entry.back()  == '\t')) entry.remove_suffix(1);
		if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"')
			entry = entry.substr(1, entry.size() - 2);
		if (!entry.empty()) dirs.push_back(pathFromUtf8(entry));

		if (sep == std::string_view::npos) break;
		list.remove_prefix(sep + 1);
	}
	return dirs;
}

std::optional<std::filesystem::path>
findExecutableInDirs(const std::vector<std::filesystem::path>& dirs, std::string_view fileName)
{
	const std::filesystem::path name = pathFromUtf8(fileName);
	for (const std::filesystem::path& dir : dirs)
	{
		if (dir.empty()) continue;
		const std::filesystem::path candidate = dir / name;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(candidate, ec)) continue;
#ifndef _WIN32
		if (::access(candidate.c_str(), X_OK) != 0) continue;
#endif
		return candidate;
	}
	return std::nullopt;
}

// ── Is the binary actually git? ──────────────────────────────────────────────

HostPlatform currentPlatform()
{
#if defined(_WIN32)
	return HostPlatform::Windows;
#elif defined(__APPLE__)
	return HostPlatform::MacOS;
#else
	return HostPlatform::Linux;
#endif
}

GitBinaryState classifyGitBinary(HostPlatform platform,
                                 const std::optional<std::filesystem::path>& found,
                                 bool developerToolsPresent)
{
	if (!found) return GitBinaryState::NotFound;
	// Every Mac has /usr/bin/git, installed or not: it is an xcrun shim that
	// forwards to the Command Line Tools and, when they are missing, fails (and
	// may pop Apple's own install dialog). Finding it says nothing on its own.
	if (platform == HostPlatform::MacOS && !developerToolsPresent &&
	    found->lexically_normal() == std::filesystem::path("/usr/bin/git"))
		return GitBinaryState::Stub;
	return GitBinaryState::Runnable;
}

// ── How would git be installed? ──────────────────────────────────────────────

const char* gitInstallRouteName(GitInstallRoute route)
{
	switch (route)
	{
	case GitInstallRoute::Winget:                return "winget";
	case GitInstallRoute::DownloadInstaller:     return "Git for Windows installer";
	case GitInstallRoute::Homebrew:              return "Homebrew";
	case GitInstallRoute::XcodeCommandLineTools: return "Xcode Command Line Tools";
	case GitInstallRoute::Apt:                   return "apt";
	case GitInstallRoute::Dnf:                   return "dnf";
	case GitInstallRoute::Pacman:                return "pacman";
	case GitInstallRoute::Zypper:                return "zypper";
	case GitInstallRoute::None:                  break;
	}
	return "none";
}

GitInstallPlan planGitInstall(HostPlatform platform,
                              const std::function<bool(std::string_view)>& toolExists,
                              bool isRoot)
{
	GitInstallPlan p;
	auto has = [&](std::string_view tool) { return toolExists && toolExists(tool); };

	switch (platform)
	{
	case HostPlatform::Windows:
	{
		if (has("winget")) p.available.push_back(GitInstallRoute::Winget);
		// Always possible on Windows, given a network: the engine downloads the
		// official installer. Nothing is bundled with the engine itself.
		p.available.push_back(GitInstallRoute::DownloadInstaller);
		p.route     = p.available.front();
		p.elevation = GitInstallElevation::OsPrompt;
		p.bringsLfs = true;   // Git for Windows ships git-lfs, by either route
		if (p.route == GitInstallRoute::Winget)
		{
			p.installCommand = "winget install --id Git.Git -e --source winget";
			p.manualCommand  = p.installCommand;
			// Only reached when git came from somewhere else (Git for Windows
			// ships LFS, so after its own install this is never needed).
			p.lfsInstallCommand = "winget install --id GitHub.GitLFS -e --source winget";
			p.lfsManualCommand  = p.lfsInstallCommand;
			p.reason = "winget is available; it installs Git for Windows (with Git LFS)";
		}
		else
		{
			p.reason = "winget (App Installer) is missing; the Git for Windows installer "
			           "would have to be downloaded from git-scm.com";
		}
		break;
	}
	case HostPlatform::MacOS:
	{
		if (has("brew"))         p.available.push_back(GitInstallRoute::Homebrew);
		if (has("xcode-select")) p.available.push_back(GitInstallRoute::XcodeCommandLineTools);
		if (p.available.empty())
		{
			p.reason = "neither Homebrew nor xcode-select was found";
			break;
		}
		p.route = p.available.front();
		if (p.route == GitInstallRoute::Homebrew)
		{
			// Preferred: no dialog, runs as the user, and the same tool can add
			// git-lfs afterwards. The Command Line Tools are a 700 MB GUI install.
			p.elevation      = GitInstallElevation::NotNeeded;
			p.installCommand = "brew install git";
			p.lfsInstallCommand = "brew install git-lfs";
			p.lfsManualCommand  = p.lfsInstallCommand;
			p.reason         = "Homebrew is available; it installs git without a dialog";
		}
		else
		{
			p.elevation      = GitInstallElevation::OsPrompt;
			p.installCommand = "xcode-select --install";
			p.reason         = "no Homebrew; the Xcode Command Line Tools (Apple's installer) bring git";
		}
		p.manualCommand = p.installCommand;
		break;
	}
	case HostPlatform::Linux:
	{
		struct Manager { const char* tool; GitInstallRoute route; const char* command; const char* lfsCommand; };
		static const Manager kManagers[] = {
			{ "apt-get", GitInstallRoute::Apt,    "apt-get update && apt-get install -y git",
			                                      "apt-get update && apt-get install -y git-lfs" },
			{ "dnf",     GitInstallRoute::Dnf,    "dnf install -y git",
			                                      "dnf install -y git-lfs" },
			{ "pacman",  GitInstallRoute::Pacman, "pacman -S --needed --noconfirm git",
			                                      "pacman -S --needed --noconfirm git-lfs" },
			{ "zypper",  GitInstallRoute::Zypper, "zypper --non-interactive install git",
			                                      "zypper --non-interactive install git-lfs" },
		};
		const Manager* chosen = nullptr;
		for (const Manager& m : kManagers)
		{
			if (!has(m.tool)) continue;
			p.available.push_back(m.route);
			if (!chosen) chosen = &m;
		}
		if (!chosen)
		{
			p.reason = "no supported package manager (apt-get, dnf, pacman, zypper) was found";
			break;
		}
		p.route          = chosen->route;
		p.installCommand = chosen->command;
		p.lfsInstallCommand = chosen->lfsCommand;
		appendCommand(p.manualCommand, chosen->command, !isRoot);
		appendCommand(p.lfsManualCommand, chosen->lfsCommand, !isRoot);

		if (isRoot)
		{
			p.elevation = GitInstallElevation::AlreadyRoot;
			p.reason    = std::string(chosen->tool) + " is available and the editor runs as root";
		}
		else if (has("pkexec"))
		{
			p.elevation = GitInstallElevation::Pkexec;
			p.reason    = std::string(chosen->tool) + " is available; pkexec asks for the password";
		}
		else
		{
			// The case the fallback hint exists for: a package manager, but no way
			// to get root without a terminal for sudo to prompt in.
			p.elevation = GitInstallElevation::Unavailable;
			p.reason    = std::string(chosen->tool) + " is available, but without pkexec the "
			              "editor cannot get administrator rights; run the command in a terminal";
		}
		break;
	}
	}
	return p;
}

GitInstallPlan detectGitInstallPlan()
{
	return planGitInstall(currentPlatform(),
	                      [](std::string_view tool) { return HE::Proc::which(tool).has_value(); },
	                      isRootUser());
}

} // namespace HE::Sc
