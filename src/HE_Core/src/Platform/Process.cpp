#include "Platform/Process.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
  #include <windows.h>
  #include <thread>
#else
  #include <cerrno>
  #include <csignal>
  #include <fcntl.h>
  #include <poll.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
  extern char** environ;
#endif

namespace HE::Proc {
namespace {

// ─── Line splitting ──────────────────────────────────────────────────────────
// Output arrives in arbitrary chunks that have nothing to do with line
// boundaries — a single read can carry half a line, or forty of them. This holds
// the remainder between reads so the callback always sees whole lines.
class LineSplitter
{
public:
	explicit LineSplitter(const std::function<void(std::string_view)>& sink) : m_sink(sink) {}

	void feed(const char* data, std::size_t len)
	{
		if (!m_sink) return;
		for (std::size_t i = 0; i < len; ++i)
		{
			if (data[i] == '\n') { emit(); continue; }
			m_pending.push_back(data[i]);
		}
	}

	// A process may end without a trailing newline; that last line is still real
	// output and would otherwise be silently dropped.
	void flush() { if (m_sink && !m_pending.empty()) emit(); }

private:
	void emit()
	{
		// Strip a CR so callers on Windows (and anything using \r\n) do not get a
		// stray carriage return glued to every line.
		if (!m_pending.empty() && m_pending.back() == '\r') m_pending.pop_back();
		m_sink(m_pending);
		m_pending.clear();
	}

	const std::function<void(std::string_view)>& m_sink;
	std::string                                  m_pending;
};

// ─── PATH augmentation ───────────────────────────────────────────────────────
#ifndef _WIN32
const char* const kToolPrefixes[] = {
	"/opt/homebrew/bin",   // Homebrew on Apple Silicon
	"/opt/homebrew/sbin",
	"/usr/local/bin",      // Homebrew on Intel, and the usual Linux local prefix
	"/usr/local/sbin",
};
#endif

} // namespace

void augmentToolPath()
{
#ifdef _WIN32
	// Windows installers put tools on the machine PATH and a GUI process inherits
	// it, so there is nothing to repair.
#else
	// Runs once per process: the first caller does the work, everyone else sees
	// the finished result. Function-local statics are thread-safe initialisation.
	static const bool done = [] {
		const char* current = ::getenv("PATH");
		std::string path    = current ? current : "";

		std::string prefix;
		for (const char* dir : kToolPrefixes)
		{
			// Match whole entries only: a substring test would consider
			// "/usr/local/bin" present because "/usr/local/bin-old" is.
			bool present = false;
			std::size_t start = 0;
			while (start <= path.size())
			{
				const std::size_t end = path.find(':', start);
				const std::string entry =
					path.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (entry == dir) { present = true; break; }
				if (end == std::string::npos) break;
				start = end + 1;
			}
			if (!present) prefix += std::string(dir) + ":";
		}

		if (!prefix.empty())
		{
			const std::string merged = prefix + path;
			::setenv("PATH", merged.c_str(), 1);
		}
		return true;
	}();
	(void)done;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
namespace {

std::wstring widen(std::string_view utf8)
{
	if (utf8.empty()) return {};
	const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
	                                    static_cast<int>(utf8.size()), nullptr, 0);
	if (n <= 0) return {};
	std::wstring out(static_cast<std::size_t>(n), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
	                      out.data(), n);
	return out;
}

std::string narrow(const wchar_t* w, std::size_t len)
{
	if (len == 0) return {};
	const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len),
	                                    nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string out(static_cast<std::size_t>(n), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), out.data(), n,
	                      nullptr, nullptr);
	return out;
}

// Quote one argument per the rules the MSVC runtime uses to split a command line
// back into argv. Windows has no argv array at the OS level — the child parses
// one string — so this is where "an argument containing a quote" is either
// preserved or corrupted.
void appendQuoted(std::wstring& out, const std::wstring& arg)
{
	const bool needsQuotes =
		arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
	if (!needsQuotes) { out += arg; return; }

	out.push_back(L'"');
	for (std::size_t i = 0; i < arg.size(); ++i)
	{
		std::size_t backslashes = 0;
		while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }

		if (i == arg.size())
		{
			// Trailing backslashes precede the closing quote, so they must be
			// doubled or they would escape it.
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (arg[i] == L'"')
		{
			// Backslashes before a literal quote are doubled, then the quote is
			// escaped with one more.
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
		}
		else
		{
			out.append(backslashes, L'\\');
			out.push_back(arg[i]);
		}
	}
	out.push_back(L'"');
}

std::wstring buildCommandLine(const Options& o)
{
	std::wstring cmd;
	appendQuoted(cmd, o.exe.wstring());
	for (const std::string& a : o.args)
	{
		cmd.push_back(L' ');
		appendQuoted(cmd, widen(a));
	}
	return cmd;
}

// A Windows environment block: NUL-separated NAME=VALUE pairs, double-NUL
// terminated, sorted case-insensitively as the API expects.
std::wstring buildEnvironment(const Options& o, bool& outUseParent)
{
	if (o.env.empty() && o.inheritEnv) { outUseParent = true; return {}; }
	outUseParent = false;

	std::vector<std::wstring> entries;
	if (o.inheritEnv)
	{
		if (LPWCH block = ::GetEnvironmentStringsW())
		{
			for (LPWCH p = block; *p; )
			{
				const std::size_t len = ::wcslen(p);
				// Skip the "=C:" drive-current-directory pseudo-variables, which
				// must not be re-sorted into the middle of the block.
				if (len && p[0] != L'=') entries.emplace_back(p, len);
				p += len + 1;
			}
			::FreeEnvironmentStringsW(block);
		}
	}

	for (const auto& [k, v] : o.env)
	{
		const std::wstring key = widen(k);
		const std::wstring prefix = key + L"=";
		std::erase_if(entries, [&](const std::wstring& e) {
			return e.size() >= prefix.size() &&
			       ::_wcsnicmp(e.c_str(), prefix.c_str(),
			                   static_cast<int>(prefix.size())) == 0;
		});
		entries.push_back(prefix + widen(v));
	}

	std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
		return ::_wcsicmp(a.c_str(), b.c_str()) < 0;
	});

	std::wstring block;
	for (const std::wstring& e : entries) { block += e; block.push_back(L'\0'); }
	block.push_back(L'\0');
	return block;
}

struct Pipe
{
	HANDLE read  = nullptr;
	HANDLE write = nullptr;

	bool create(bool inheritRead, bool inheritWrite)
	{
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		if (!::CreatePipe(&read, &write, &sa, 0)) return false;
		// Only the end the child needs may be inheritable. Leaving the parent's
		// end inheritable means the child holds a copy, so the pipe never reports
		// EOF and the parent waits forever on a process that has already exited.
		::SetHandleInformation(read,  HANDLE_FLAG_INHERIT, inheritRead  ? HANDLE_FLAG_INHERIT : 0);
		::SetHandleInformation(write, HANDLE_FLAG_INHERIT, inheritWrite ? HANDLE_FLAG_INHERIT : 0);
		return true;
	}
	void closeRead()  { if (read)  { ::CloseHandle(read);  read  = nullptr; } }
	void closeWrite() { if (write) { ::CloseHandle(write); write = nullptr; } }
	~Pipe() { closeRead(); closeWrite(); }
};

void drainPipe(HANDLE h, std::string& into, LineSplitter& splitter, std::mutex& mx)
{
	char buf[4096];
	for (;;)
	{
		DWORD got = 0;
		if (!::ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
		std::lock_guard<std::mutex> lock(mx);
		into.append(buf, got);
		splitter.feed(buf, got);
	}
}

} // namespace

Result run(const Options& o)
{
	Result r;
	augmentToolPath();

	Pipe outPipe, errPipe, inPipe;
	if (!outPipe.create(/*inheritRead=*/false, /*inheritWrite=*/true) ||
	    !errPipe.create(/*inheritRead=*/false, /*inheritWrite=*/true) ||
	    !inPipe.create(/*inheritRead=*/true,   /*inheritWrite=*/false))
	{
		r.launchFailed = true;
		return r;
	}

	STARTUPINFOW si{};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdInput  = inPipe.read;
	si.hStdOutput = outPipe.write;
	si.hStdError  = errPipe.write;

	std::wstring cmd = buildCommandLine(o);
	bool useParentEnv = true;
	std::wstring envBlock = buildEnvironment(o, useParentEnv);
	const std::wstring cwd = o.cwd.empty() ? std::wstring{} : o.cwd.wstring();

	// A job object is how the whole tree is killed. Terminating just the child
	// leaves its own children — a launcher's real worker, for instance — running
	// and still holding the pipes.
	HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
	if (job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	PROCESS_INFORMATION pi{};
	// CREATE_NO_WINDOW: without it every call from a GUI-subsystem process flashes
	// a console window. CREATE_SUSPENDED so the child is in the job before it can
	// spawn anything of its own.
	const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
	const BOOL started = ::CreateProcessW(
		nullptr, cmd.data(), nullptr, nullptr, TRUE, flags,
		useParentEnv ? nullptr : const_cast<wchar_t*>(envBlock.c_str()),
		cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);

	if (!started)
	{
		if (job) ::CloseHandle(job);
		r.launchFailed = true;
		return r;
	}

	if (job) ::AssignProcessToJobObject(job, pi.hProcess);
	::ResumeThread(pi.hThread);

	// The parent must let go of the child's ends, or the pipes never see EOF.
	outPipe.closeWrite();
	errPipe.closeWrite();

	// Both streams are drained concurrently. Reading one to EOF and then the
	// other deadlocks as soon as the child fills the pipe we are not reading —
	// which is exactly what happens with a progress-reporting tool.
	std::mutex   mx;
	LineSplitter outSplit(o.onStdoutLine);
	LineSplitter errSplit(o.onStderrLine);
	std::thread outThread([&] { drainPipe(outPipe.read, r.out, outSplit, mx); });
	std::thread errThread([&] { drainPipe(errPipe.read, r.err, errSplit, mx); });

	// Written only AFTER the readers are running. WriteFile blocks until the
	// whole payload is consumed, and a child that echoes its input fills its
	// output pipe long before that — so with the write first, the child blocks on
	// output, stops reading input, and both sides wait on each other. It only
	// shows up once the payload exceeds the pipe buffer.
	if (!o.stdinData.empty())
	{
		DWORD written = 0;
		::WriteFile(inPipe.write, o.stdinData.data(),
		            static_cast<DWORD>(o.stdinData.size()), &written, nullptr);
	}
	inPipe.closeWrite();   // signals EOF to a child reading until end of input

	const DWORD waitMs = o.timeoutMs ? o.timeoutMs : INFINITE;
	if (::WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT)
	{
		r.timedOut = true;
		if (job) ::TerminateJobObject(job, 1);
		else     ::TerminateProcess(pi.hProcess, 1);
		::WaitForSingleObject(pi.hProcess, 5000);
	}

	outThread.join();
	errThread.join();
	outSplit.flush();
	errSplit.flush();

	DWORD code = 0;
	if (::GetExitCodeProcess(pi.hProcess, &code)) r.exitCode = static_cast<int>(code);

	::CloseHandle(pi.hThread);
	::CloseHandle(pi.hProcess);
	if (job) ::CloseHandle(job);
	return r;
}

std::optional<std::filesystem::path> which(std::string_view exe)
{
	augmentToolPath();
	const std::wstring name = widen(exe);
	wchar_t buf[MAX_PATH * 4] = {};
	// SearchPathW applies the same rules the loader does, including PATHEXT when
	// an extension is supplied via the third argument.
	for (const wchar_t* ext : { L".exe", L".cmd", L".bat", static_cast<const wchar_t*>(nullptr) })
	{
		const DWORD n = ::SearchPathW(nullptr, name.c_str(), ext,
		                              static_cast<DWORD>(std::size(buf)), buf, nullptr);
		if (n > 0 && n < std::size(buf)) return std::filesystem::path(buf);
		if (!ext) break;
	}
	return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
#else   // POSIX

namespace {

struct Fd
{
	int v = -1;
	Fd() = default;
	explicit Fd(int fd) : v(fd) {}
	Fd(const Fd&)            = delete;
	Fd& operator=(const Fd&) = delete;
	~Fd() { close(); }
	void close() { if (v >= 0) { ::close(v); v = -1; } }
	int  release() { const int t = v; v = -1; return t; }
};

std::vector<std::string> buildEnvStrings(const Options& o)
{
	std::vector<std::string> entries;
	if (o.inheritEnv)
		for (char** e = environ; e && *e; ++e) entries.emplace_back(*e);

	for (const auto& [k, v] : o.env)
	{
		const std::string prefix = k + "=";
		std::erase_if(entries, [&](const std::string& e) {
			return e.compare(0, prefix.size(), prefix) == 0;
		});
		entries.push_back(prefix + v);
	}
	return entries;
}

std::int64_t nowMillis()
{
	timespec ts{};
	::clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

Result run(const Options& o)
{
	Result r;
	augmentToolPath();

	int outFds[2] = { -1, -1 };
	int errFds[2] = { -1, -1 };
	int inFds[2]  = { -1, -1 };
	if (::pipe(outFds) != 0) { r.launchFailed = true; return r; }
	if (::pipe(errFds) != 0) { ::close(outFds[0]); ::close(outFds[1]); r.launchFailed = true; return r; }
	if (::pipe(inFds)  != 0) {
		::close(outFds[0]); ::close(outFds[1]); ::close(errFds[0]); ::close(errFds[1]);
		r.launchFailed = true; return r;
	}
	Fd outRead(outFds[0]), outWrite(outFds[1]);
	Fd errRead(errFds[0]), errWrite(errFds[1]);
	Fd inRead(inFds[0]),   inWrite(inFds[1]);

	posix_spawn_file_actions_t actions;
	::posix_spawn_file_actions_init(&actions);
	::posix_spawn_file_actions_adddup2(&actions, inRead.v,   STDIN_FILENO);
	::posix_spawn_file_actions_adddup2(&actions, outWrite.v, STDOUT_FILENO);
	::posix_spawn_file_actions_adddup2(&actions, errWrite.v, STDERR_FILENO);
	// The parent's ends must not survive into the child, or the pipes never
	// report EOF once the child exits.
	::posix_spawn_file_actions_addclose(&actions, outRead.v);
	::posix_spawn_file_actions_addclose(&actions, errRead.v);
	::posix_spawn_file_actions_addclose(&actions, inWrite.v);
	if (!o.cwd.empty())
	{
#if defined(__APPLE__) || (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L)
		::posix_spawn_file_actions_addchdir_np(&actions, o.cwd.c_str());
#endif
	}

	posix_spawnattr_t attr;
	::posix_spawnattr_init(&attr);
	// Own process group, so a kill can take the child AND anything it spawned.
	// Killing only the direct child leaves grandchildren holding the pipes.
	::posix_spawnattr_setpgroup(&attr, 0);
	::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

	std::vector<std::string> argStrings;
	argStrings.push_back(o.exe.string());
	for (const std::string& a : o.args) argStrings.push_back(a);
	std::vector<char*> argv;
	argv.reserve(argStrings.size() + 1);
	for (std::string& a : argStrings) argv.push_back(a.data());
	argv.push_back(nullptr);

	std::vector<std::string> envStrings = buildEnvStrings(o);
	std::vector<char*> envp;
	envp.reserve(envStrings.size() + 1);
	for (std::string& e : envStrings) envp.push_back(e.data());
	envp.push_back(nullptr);

	pid_t pid = -1;
	// spawnp, not spawn: a bare program name has to be resolved through PATH the
	// same way a shell would.
	const int rc = ::posix_spawnp(&pid, o.exe.c_str(), &actions, &attr,
	                              argv.data(), envp.data());
	::posix_spawn_file_actions_destroy(&actions);
	::posix_spawnattr_destroy(&attr);

	if (rc != 0) { r.launchFailed = true; return r; }

	// Release the child's ends in the parent.
	outWrite.close();
	errWrite.close();
	inRead.close();

	LineSplitter outSplit(o.onStdoutLine);
	LineSplitter errSplit(o.onStderrLine);

	std::size_t stdinOffset = 0;
	if (o.stdinData.empty()) inWrite.close();
	// The write end MUST be non-blocking. poll() reporting POLLOUT only promises
	// that one byte fits; a blocking write() of the whole payload then sits there
	// until every byte is gone. Meanwhile this thread is not draining stdout, so
	// the child fills its output pipe, blocks, and stops reading stdin — both
	// sides waiting on the other. It only shows up once the payload exceeds the
	// pipe buffer, so it passes with small inputs and hangs on real ones.
	// O_NONBLOCK is a property of this open file description alone, so the
	// child's read end stays blocking, as it expects.
	else ::fcntl(inWrite.v, F_SETFL, ::fcntl(inWrite.v, F_GETFL, 0) | O_NONBLOCK);

	const std::int64_t deadline = o.timeoutMs ? nowMillis() + o.timeoutMs : 0;
	bool killed = false;

	// One poll loop over all three descriptors. stdin is written incrementally
	// rather than up front: a large payload would otherwise fill the pipe buffer
	// and block the parent while the child is blocked writing output nobody is
	// reading — a deadlock that only shows up on big inputs.
	while (outRead.v >= 0 || errRead.v >= 0 || inWrite.v >= 0)
	{
		pollfd fds[3];
		int    n = 0;
		int    outIdx = -1, errIdx = -1, inIdx = -1;
		if (outRead.v >= 0) { fds[n] = { outRead.v, POLLIN,  0 }; outIdx = n++; }
		if (errRead.v >= 0) { fds[n] = { errRead.v, POLLIN,  0 }; errIdx = n++; }
		if (inWrite.v >= 0) { fds[n] = { inWrite.v, POLLOUT, 0 }; inIdx  = n++; }

		int timeout = -1;
		if (deadline)
		{
			const std::int64_t remaining = deadline - nowMillis();
			if (remaining <= 0)
			{
				r.timedOut = true;
				::kill(-pid, SIGTERM);
				killed = true;
				break;
			}
			timeout = static_cast<int>(std::min<std::int64_t>(remaining, 100));
		}

		const int ready = ::poll(fds, static_cast<nfds_t>(n), timeout);
		if (ready < 0)
		{
			if (errno == EINTR) continue;
			break;
		}
		if (ready == 0) continue;   // timeout slice elapsed; re-check the deadline

		auto readInto = [&](int idx, Fd& fd, std::string& into, LineSplitter& split) {
			if (idx < 0 || !(fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
			char  buf[4096];
			const ssize_t got = ::read(fd.v, buf, sizeof(buf));
			if (got > 0) { into.append(buf, static_cast<std::size_t>(got)); split.feed(buf, static_cast<std::size_t>(got)); }
			else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) fd.close();
		};
		readInto(outIdx, outRead, r.out, outSplit);
		readInto(errIdx, errRead, r.err, errSplit);

		if (inIdx >= 0 && (fds[inIdx].revents & (POLLOUT | POLLERR | POLLHUP)))
		{
			if (fds[inIdx].revents & (POLLERR | POLLHUP)) inWrite.close();
			else
			{
				const std::size_t left = o.stdinData.size() - stdinOffset;
				const ssize_t wrote = ::write(inWrite.v, o.stdinData.data() + stdinOffset, left);
				if (wrote > 0)
				{
					stdinOffset += static_cast<std::size_t>(wrote);
					if (stdinOffset >= o.stdinData.size()) inWrite.close();
				}
				else if (wrote < 0 && errno != EINTR && errno != EAGAIN) inWrite.close();
			}
		}
	}

	outSplit.flush();
	errSplit.flush();

	int status = 0;
	if (killed)
	{
		// Give the group a moment to die politely, then insist. Without SIGKILL a
		// child that ignores SIGTERM would leave us blocked in waitpid forever —
		// the very hang the timeout exists to prevent.
		const std::int64_t hardDeadline = nowMillis() + 2000;
		for (;;)
		{
			const pid_t w = ::waitpid(pid, &status, WNOHANG);
			if (w == pid || w < 0) break;
			if (nowMillis() >= hardDeadline) { ::kill(-pid, SIGKILL); ::waitpid(pid, &status, 0); break; }
			::usleep(10000);
		}
	}
	else
	{
		while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	}

	// The distinction popen could not make: a wait status is not an exit code.
	if (WIFEXITED(status))        r.exitCode = WEXITSTATUS(status);
	else if (WIFSIGNALED(status)) r.exitCode = 128 + WTERMSIG(status);
	return r;
}

std::optional<std::filesystem::path> which(std::string_view exe)
{
	augmentToolPath();
	const std::string name(exe);
	if (name.find('/') != std::string::npos)
	{
		std::error_code ec;
		if (std::filesystem::exists(name, ec) && ::access(name.c_str(), X_OK) == 0)
			return std::filesystem::path(name);
		return std::nullopt;
	}

	const char* path = ::getenv("PATH");
	if (!path) return std::nullopt;

	std::string_view sv(path);
	while (!sv.empty())
	{
		const std::size_t sep = sv.find(':');
		const std::string dir(sv.substr(0, sep));
		if (!dir.empty())
		{
			const std::filesystem::path candidate = std::filesystem::path(dir) / name;
			if (::access(candidate.c_str(), X_OK) == 0) return candidate;
		}
		if (sep == std::string_view::npos) break;
		sv.remove_prefix(sep + 1);
	}
	return std::nullopt;
}

#endif  // _WIN32

} // namespace HE::Proc
