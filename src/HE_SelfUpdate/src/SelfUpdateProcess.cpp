#include "SelfUpdate/SelfUpdateProcess.h"

#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <sys/sysctl.h>
  #else
    #include <fstream>
    #include <sstream>
  #endif
  extern char** environ;
#endif

namespace fs = std::filesystem;

namespace HE::Su {

fs::path pathFromUtf8(const std::string& utf8)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8FromPath(const fs::path& path)
{
	const std::u8string u = path.u8string();
	return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

namespace {

std::string errnoText(int e)
{
	return std::error_code(e, std::generic_category()).message();
}

} // namespace

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& utf8)
{
	if (utf8.empty()) return {};
	const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
	if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
	return w;
}

std::string lastErrorText(DWORD code)
{
	wchar_t* buf = nullptr;
	const DWORD n = ::FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
	std::string text = "error " + std::to_string(code);
	if (n && buf)
	{
		std::wstring w(buf, n);
		while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
		const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<std::size_t>(len > 0 ? len : 0), '\0');
		if (len > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
		text += ": " + s;
	}
	if (buf) ::LocalFree(buf);
	return text;
}

// One argument, quoted so CommandLineToArgvW (which every C runtime uses) gives
// back exactly the string that went in. The rule is about backslashes in front
// of a quote, and it is the part that "just wrap it in quotes" gets wrong.
void appendQuoted(std::wstring& out, const std::wstring& arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
	{
		out += arg;
		return;
	}
	out += L'"';
	std::size_t backslashes = 0;
	for (const wchar_t c : arg)
	{
		if (c == L'\\') { ++backslashes; continue; }
		if (c == L'"')
		{
			out.append(backslashes * 2 + 1, L'\\');
			out += L'"';
		}
		else
		{
			out.append(backslashes, L'\\');
			out += c;
		}
		backslashes = 0;
	}
	out.append(backslashes * 2, L'\\');   // before the closing quote they all double
	out += L'"';
}

struct Handle
{
	HANDLE h = INVALID_HANDLE_VALUE;
	Handle() = default;
	explicit Handle(HANDLE x) : h(x) {}
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	~Handle() { reset(); }
	void reset() { if (h != INVALID_HANDLE_VALUE && h != nullptr) ::CloseHandle(h); h = INVALID_HANDLE_VALUE; }
	bool valid() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

} // namespace

SpawnResult spawnDetached(const SpawnOptions& o)
{
	SpawnResult r;
	std::error_code ec;
	if (o.exe.empty() || !fs::exists(o.exe, ec))
	{
		r.error = "program not found: " + utf8FromPath(o.exe);
		return r;
	}

	std::wstring cmd;
	appendQuoted(cmd, o.exe.wstring());
	for (const std::string& a : o.args)
	{
		cmd += L' ';
		appendQuoted(cmd, widen(a));
	}

	SECURITY_ATTRIBUTES inherit{};
	inherit.nLength        = sizeof(inherit);
	inherit.bInheritHandle = TRUE;

	Handle in(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	Handle out;
	if (o.logFile.empty())
		out.h = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
		                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	else
		out.h = ::CreateFileW(o.logFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
		                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!in.valid() || !out.valid())
	{
		r.error = "could not open the standard handles for the new process: " + lastErrorText(::GetLastError());
		return r;
	}

	// Exactly these three handles are inherited. With bInheritHandles = TRUE alone
	// the child gets EVERY inheritable handle of this process, among them the pipe
	// ends of a HE::Proc::run on another thread — which would then never see EOF.
	// (`out` serves stdout AND stderr, so two distinct handles are enough.)
	HANDLE inheritList[2] = { in.h, out.h };
	const DWORD inheritCount = 2;

	SIZE_T attrSize = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	std::vector<char> attrBuf(attrSize);
	auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
	if (!::InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
	{
		r.error = "InitializeProcThreadAttributeList: " + lastErrorText(::GetLastError());
		return r;
	}
	if (!::UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
	                                 inheritList, sizeof(HANDLE) * inheritCount, nullptr, nullptr))
	{
		r.error = "UpdateProcThreadAttribute: " + lastErrorText(::GetLastError());
		::DeleteProcThreadAttributeList(attrList);
		return r;
	}

	STARTUPINFOEXW si{};
	si.StartupInfo.cb         = sizeof(si);
	si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput  = in.h;
	si.StartupInfo.hStdOutput = out.h;
	si.StartupInfo.hStdError  = out.h;
	si.lpAttributeList        = attrList;

	const std::wstring cwd = o.cwd.empty() ? std::wstring{} : o.cwd.wstring();
	DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
	              EXTENDED_STARTUPINFO_PRESENT;

	PROCESS_INFORMATION pi{};
	// First as a process that leaves the caller's job: an editor started inside a
	// job object with kill-on-close (a terminal, a CI step, some launchers) would
	// otherwise take the updater down at the very moment it exits. A job that does
	// not allow breakaway answers ACCESS_DENIED — then without, which is the best
	// that can be had there.
	std::wstring mutableCmd = cmd;
	BOOL ok = ::CreateProcessW(o.exe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
	                           flags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
	                           cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
	if (!ok && ::GetLastError() == ERROR_ACCESS_DENIED)
	{
		mutableCmd = cmd;
		ok = ::CreateProcessW(o.exe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
		                      flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
	}
	const DWORD err = ok ? 0 : ::GetLastError();
	::DeleteProcThreadAttributeList(attrList);
	if (!ok)
	{
		r.error = "CreateProcess failed for " + utf8FromPath(o.exe) + ": " + lastErrorText(err);
		return r;
	}
	r.ok  = true;
	r.pid = static_cast<std::int64_t>(pi.dwProcessId);
	::CloseHandle(pi.hThread);
	if (o.waitForExitMs && ::WaitForSingleObject(pi.hProcess, o.waitForExitMs) == WAIT_OBJECT_0)
	{
		DWORD code = 0;
		if (::GetExitCodeProcess(pi.hProcess, &code))
		{
			r.exited   = true;
			r.exitCode = static_cast<int>(code);
		}
	}
	::CloseHandle(pi.hProcess);
	return r;
}

std::int64_t currentProcessId() { return static_cast<std::int64_t>(::GetCurrentProcessId()); }

bool processExists(std::int64_t pid)
{
	if (pid <= 0) return false;
	Handle h(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
	if (!h.valid()) return ::GetLastError() != ERROR_INVALID_PARAMETER;   // access denied = there, but not ours
	return ::WaitForSingleObject(h.h, 0) == WAIT_TIMEOUT;
}

bool waitForProcessExit(std::int64_t pid, std::uint32_t timeoutMs)
{
	if (pid <= 0) return true;
	Handle h(::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
	if (!h.valid())
	{
		if (::GetLastError() == ERROR_INVALID_PARAMETER) return true;   // no such process
		// Cannot wait on it — poll instead.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (processExists(pid))
		{
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return true;
	}
	return ::WaitForSingleObject(h.h, timeoutMs) == WAIT_OBJECT_0;
}

fs::path currentExecutablePath()
{
	std::wstring buf(512, L'\0');
	for (;;)
	{
		const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
		if (n == 0) return {};
		if (n < buf.size()) { buf.resize(n); break; }
		buf.resize(buf.size() * 2);
	}
	std::error_code ec;
	fs::path canon = fs::canonical(fs::path(buf), ec);
	return ec ? fs::path(buf) : canon;
}

#else // ── POSIX ───────────────────────────────────────────────────────────────

SpawnResult spawnDetached(const SpawnOptions& o)
{
	SpawnResult r;
	std::error_code ec;
	if (o.exe.empty() || !fs::exists(o.exe, ec))
	{
		r.error = "program not found: " + utf8FromPath(o.exe);
		return r;
	}

	posix_spawn_file_actions_t actions;
	::posix_spawn_file_actions_init(&actions);
	::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	const std::string logPath = o.logFile.empty() ? std::string("/dev/null") : o.logFile.string();
	::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(),
	                                   O_WRONLY | O_CREAT | O_APPEND, 0644);
	::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
	if (!o.cwd.empty())
		::posix_spawn_file_actions_addchdir_np(&actions, o.cwd.c_str());
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
	// Everything above the three standard descriptors stays behind: an editor holds
	// sockets, audio and file handles that the updater and the next editor have no
	// business inheriting.
	::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif

	posix_spawnattr_t attr;
	::posix_spawnattr_init(&attr);
	short flags = 0;
#ifdef POSIX_SPAWN_SETSID
	flags |= POSIX_SPAWN_SETSID;            // its own session: not killed with our terminal, no controlling tty
#else
	flags |= POSIX_SPAWN_SETPGROUP;         // at least its own process group
	::posix_spawnattr_setpgroup(&attr, 0);
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;   // Apple: only the descriptors set up above
#endif
	::posix_spawnattr_setflags(&attr, flags);

	std::vector<std::string> argStrings;
	argStrings.push_back(o.exe.string());
	for (const std::string& a : o.args) argStrings.push_back(a);
	std::vector<char*> argv;
	for (std::string& a : argStrings) argv.push_back(a.data());
	argv.push_back(nullptr);

	pid_t pid = -1;
	const int rc = ::posix_spawn(&pid, o.exe.c_str(), &actions, &attr, argv.data(), environ);
	::posix_spawn_file_actions_destroy(&actions);
	::posix_spawnattr_destroy(&attr);
	if (rc != 0)
	{
		r.error = "could not start " + o.exe.string() + ": " + errnoText(rc);
		return r;
	}
	r.ok  = true;
	r.pid = static_cast<std::int64_t>(pid);
	if (o.waitForExitMs)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.waitForExitMs);
		for (;;)
		{
			int status = 0;
			const pid_t got = ::waitpid(pid, &status, WNOHANG);
			if (got == pid)
			{
				r.exited   = true;
				r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
				           : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
				break;
			}
			if (got < 0 && errno != EINTR) break;
			if (std::chrono::steady_clock::now() >= deadline) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	return r;
}

std::int64_t currentProcessId() { return static_cast<std::int64_t>(::getpid()); }

bool processExists(std::int64_t pid)
{
	if (pid <= 0) return false;
	if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
#if defined(__APPLE__)
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
	struct kinfo_proc info;
	std::memset(&info, 0, sizeof(info));
	std::size_t size = sizeof(info);
	if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return false;
	return info.kp_proc.p_stat != SZOMB;
#else
	// "pid (comm) S ..." — comm may itself hold spaces and parentheses, so the
	// state is the first field after the LAST closing parenthesis.
	std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
	if (!stat) return false;
	std::ostringstream os;
	os << stat.rdbuf();
	const std::string text = os.str();
	const std::size_t close = text.rfind(')');
	if (close == std::string::npos || close + 2 >= text.size()) return true;
	const char state = text[close + 2];
	return state != 'Z' && state != 'X';
#endif
}

bool waitForProcessExit(std::int64_t pid, std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (processExists(pid))
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return true;
}

fs::path currentExecutablePath()
{
	fs::path raw;
#if defined(__APPLE__)
	std::uint32_t size = 0;
	::_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
	raw = fs::path(buf.c_str());
#else
	std::error_code linkEc;
	raw = fs::read_symlink("/proc/self/exe", linkEc);
	if (linkEc) return {};
#endif
	std::error_code ec;
	fs::path canon = fs::canonical(raw, ec);
	return ec ? raw : canon;
}

#endif

} // namespace HE::Su
