// Test subject for HE::Proc::run — a program that does exactly one specified
// thing, so a test can assert on it.
//
// A real child process is the only way to test a process runner honestly. Every
// bug the runner exists to avoid (exit codes mangled into wait statuses, streams
// merged, pipes deadlocking, arguments mangled by quoting, grandchildren
// surviving a kill) is invisible to a mock and obvious here.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <fcntl.h>      // _O_BINARY
  #include <io.h>         // _setmode, _fileno
  #include <windows.h>    // GetCommandLineW, WideCharToMultiByte, CreateProcessA
  #include <shellapi.h>   // CommandLineToArgvW — NOT in windows.h
  #pragma comment(lib, "shell32.lib")
#else
  #include <unistd.h>
#endif

namespace {

// The arguments as UTF-8, on every platform.
//
// Windows does not hand a narrow main() its arguments in UTF-8: argv is encoded
// in the process's active code page, so anything outside it arrives as '?'. That
// is a property of narrow main(), NOT of how the arguments were delivered — the
// runner passes correct UTF-16 to CreateProcessW. Reading the wide command line
// back and converting it explicitly is what makes this program measure the
// delivery rather than the CRT's lossy conversion.
std::vector<std::string> utf8Args(int argc, char** argv)
{
	std::vector<std::string> out;
#ifdef _WIN32
	(void)argc; (void)argv;
	int      wideCount = 0;
	LPWSTR*  wide      = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount);
	if (!wide) return out;
	for (int i = 0; i < wideCount; ++i)
	{
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
		if (n > 1)
			::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
		out.push_back(std::move(s));
	}
	::LocalFree(wide);
#else
	for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
#endif
	return out;
}

void writeBytes(std::FILE* stream, std::size_t count, char fill)
{
	// Written in chunks with no newlines, so the receiving end is exercised on
	// raw volume rather than on line handling.
	std::string chunk(4096, fill);
	while (count > 0)
	{
		const std::size_t n = count < chunk.size() ? count : chunk.size();
		std::fwrite(chunk.data(), 1, n, stream);
		count -= n;
	}
	std::fflush(stream);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
	// Without this the CRT translates \n to \r\n on the way out, and a test that
	// counts bytes would be measuring the translation instead of the transfer.
	::_setmode(::_fileno(stdout), _O_BINARY);
	::_setmode(::_fileno(stderr), _O_BINARY);
	::_setmode(::_fileno(stdin),  _O_BINARY);
#endif

	const std::vector<std::string> args = utf8Args(argc, argv);
	const int argCount = static_cast<int>(args.size());
	if (argCount < 2) { std::fputs("usage: he_proc_child <mode> [...]\n", stderr); return 2; }
	const std::string mode = args[1];

	if (mode == "exit")
	{
		return argCount > 2 ? std::atoi(args[2].c_str()) : 0;
	}
	if (mode == "out")
	{
		for (int i = 2; i < argCount; ++i) std::printf("%s\n", args[i].c_str());
		return 0;
	}
	if (mode == "err")
	{
		for (int i = 2; i < argCount; ++i) std::fprintf(stderr, "%s\n", args[i].c_str());
		return 0;
	}
	if (mode == "streams")
	{
		// Distinct text on each stream, so a merged implementation is caught.
		std::printf("this-is-stdout\n");
		std::fprintf(stderr, "this-is-stderr\n");
		std::fflush(stdout);
		std::fflush(stderr);
		return 3;
	}
	if (mode == "flood")
	{
		// Fills both pipes far past their buffers. An implementation that drains
		// one stream to EOF before starting the other deadlocks here and never
		// returns — which is the point of the test.
		const std::size_t bytes = argCount > 2 ? static_cast<std::size_t>(std::atol(args[2].c_str())) : 1000000;
		std::thread t([&] { writeBytes(stderr, bytes, 'E'); });
		writeBytes(stdout, bytes, 'O');
		t.join();
		return 0;
	}
	if (mode == "echo")
	{
		// Copies stdin to stdout, then exits — so a runner that never closes the
		// child's stdin hangs here instead of completing.
		char buf[4096];
		std::size_t got = 0;
		while ((got = std::fread(buf, 1, sizeof(buf), stdin)) > 0)
			std::fwrite(buf, 1, got, stdout);
		std::fflush(stdout);
		return 0;
	}
	if (mode == "args")
	{
		// One argument per line, verbatim: proves nothing was split, merged or
		// unquoted in transit.
		for (int i = 2; i < argCount; ++i) std::printf("[%s]\n", args[i].c_str());
		return 0;
	}
	if (mode == "env")
	{
		const char* v = argCount > 2 ? std::getenv(args[2].c_str()) : nullptr;
		std::printf("%s\n", v ? v : "<unset>");
		return 0;
	}
	if (mode == "sleep")
	{
		const int ms = argCount > 2 ? std::atoi(args[2].c_str()) : 1000;
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		std::printf("finished\n");
		return 0;
	}
	if (mode == "spawn-grandchild")
	{
		// Starts a detached grandchild that writes a marker file after a delay,
		// then blocks. If the runner kills only the direct child, the grandchild
		// survives and the marker appears — which is the failure this detects.
		const std::string marker = argCount > 2 ? args[2] : "";
		const std::string self   = args[0];
#ifdef _WIN32
		const std::string cmdText = "\"" + self + "\" delayed-write \"" + marker + "\"";
		// CreateProcessA may write to its command-line argument, so it needs a
		// genuinely mutable buffer rather than a string's storage.
		std::vector<char> cmd(cmdText.begin(), cmdText.end());
		cmd.push_back('\0');
		STARTUPINFOA si{}; si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
		                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		if (pi.hProcess) { ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess); }
#else
		if (::fork() == 0)
		{
			::execl(self.c_str(), self.c_str(), "delayed-write", marker.c_str(), nullptr);
			::_exit(127);
		}
#endif
		std::printf("spawned\n");
		std::fflush(stdout);
		std::this_thread::sleep_for(std::chrono::seconds(30));
		return 0;
	}
	if (mode == "delayed-write")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		if (argCount > 2)
		{
			std::ofstream out(args[2]);
			out << "grandchild survived\n";
		}
		return 0;
	}
	if (mode == "cwd")
	{
		char buf[4096] = {};
#ifdef _WIN32
		::GetCurrentDirectoryA(static_cast<DWORD>(sizeof(buf)), buf);
#else
		if (!::getcwd(buf, sizeof(buf))) buf[0] = '\0';
#endif
		std::printf("%s\n", buf);
		return 0;
	}

	std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
	return 2;
}
