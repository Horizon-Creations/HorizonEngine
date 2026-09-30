#include "HiddenShell.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace HE {
namespace {

std::mutex                          g_mx;
std::unordered_map<FILE*, HANDLE>   g_children;   // stream → its shell process

HANDLE openNul(DWORD access)
{
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	return ::CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
	                     OPEN_EXISTING, 0, nullptr);
}

// Start `%COMSPEC% /c <line>` without a console window. Every handle in `in`,
// `out`, `err` must be inheritable; they are the ONLY handles the child gets.
// Returns the process handle, or nullptr.
HANDLE startShell(const std::string& line, HANDLE in, HANDLE out, HANDLE err)
{
	// What the CRT does: COMSPEC, and cmd.exe when it is not set.
	char comspec[MAX_PATH] = {};
	const DWORD n = ::GetEnvironmentVariableA("COMSPEC", comspec, MAX_PATH);
	const std::string shell = (n > 0 && n < MAX_PATH) ? std::string(comspec) : std::string("cmd.exe");

	// Narrow on purpose, like _popen/system: the line was built from
	// path::string() in the active code page and goes out through the same
	// conversion. Everything after "/c " is handed to cmd verbatim.
	std::string cmd = "\"" + shell + "\" /c " + line;
	std::vector<char> buf(cmd.begin(), cmd.end());
	buf.push_back('\0');

	// Restrict inheritance to this child's own handles. With plain
	// bInheritHandles, a shell started on another thread at the same moment
	// would inherit our pipe's write end and keep it open until IT exits.
	HANDLE list[3];
	DWORD count = 0;
	for (HANDLE h : { in, out, err })
	{
		bool seen = false;
		for (DWORD i = 0; i < count; ++i) seen = seen || list[i] == h;
		if (!seen) list[count++] = h;
	}
	SIZE_T attrSize = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	std::vector<unsigned char> attrBuf(attrSize);
	auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
	if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) return nullptr;
	const bool listed = ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
	                                                list, count * sizeof(HANDLE), nullptr, nullptr);

	STARTUPINFOEXA si{};
	si.StartupInfo.cb         = sizeof(si);
	si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput  = in;
	si.StartupInfo.hStdOutput = out;
	si.StartupInfo.hStdError  = err;
	si.lpAttributeList        = listed ? attrs : nullptr;

	// CREATE_NO_WINDOW is the whole point: the console the shell needs exists,
	// it just never gets a window. Only when we have no console, though. One
	// that has a console (he_tests, the editor started from a terminal) lets the
	// child share it, exactly as _popen did: no new window can come of that, and
	// a fresh console would answer in its own code page — "Grüße" came back as
	// OEM bytes instead of the caller's UTF-8.
	const bool haveConsole = ::GetConsoleCP() != 0;
	const DWORD flags = (haveConsole ? 0 : CREATE_NO_WINDOW) |
	                    (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
	PROCESS_INFORMATION pi{};
	// No application name: a bare "cmd.exe" (COMSPEC unset) is then found the
	// way the OS finds any first token, instead of only in the current directory.
	const BOOL ok = ::CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, flags,
	                                 nullptr, nullptr, &si.StartupInfo, &pi);
	::DeleteProcThreadAttributeList(attrs);
	if (!ok) return nullptr;
	::CloseHandle(pi.hThread);
	return pi.hProcess;
}

int waitExitCode(HANDLE process)
{
	::WaitForSingleObject(process, INFINITE);
	DWORD code = static_cast<DWORD>(-1);
	::GetExitCodeProcess(process, &code);
	::CloseHandle(process);
	return static_cast<int>(code);
}

} // namespace

FILE* hiddenPopen(const std::string& line)
{
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	HANDLE rd = nullptr, wr = nullptr;
	if (!::CreatePipe(&rd, &wr, &sa, 0)) return nullptr;
	// Our end stays ours: an inherited read end would be one more reader the
	// pipe waits for.
	::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	HANDLE nul = openNul(GENERIC_READ);
	HANDLE proc = (nul != INVALID_HANDLE_VALUE) ? startShell(line, nul, wr, wr) : nullptr;
	// The child holds its own copies now. Ours must go, or the stream never
	// sees EOF.
	::CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
	if (!proc)
	{
		::CloseHandle(rd);
		return nullptr;
	}

	// Text mode, exactly like _popen(…, "r"): CRLF arrives as LF, so the
	// version, vswhere and capabilities parsers see the same bytes as before.
	const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(rd), _O_RDONLY | _O_TEXT);
	if (fd == -1)
	{
		::CloseHandle(rd);
		::TerminateProcess(proc, 1);
		::CloseHandle(proc);
		return nullptr;
	}
	FILE* f = ::_fdopen(fd, "rt");
	if (!f)
	{
		::_close(fd);
		::TerminateProcess(proc, 1);
		::CloseHandle(proc);
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_mx);
	g_children[f] = proc;
	return f;
}

int hiddenPclose(FILE* stream)
{
	if (!stream) return -1;
	HANDLE proc = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		const auto it = g_children.find(stream);
		if (it == g_children.end()) return -1;
		proc = it->second;
		g_children.erase(it);
	}
	std::fclose(stream);
	return waitExitCode(proc);
}

int hiddenSystem(const std::string& line)
{
	HANDLE in  = openNul(GENERIC_READ);
	HANDLE out = openNul(GENERIC_WRITE);
	HANDLE proc = (in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE)
	                  ? startShell(line, in, out, out) : nullptr;
	if (in != INVALID_HANDLE_VALUE)  ::CloseHandle(in);
	if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
	return proc ? waitExitCode(proc) : -1;
}

} // namespace HE
#endif
