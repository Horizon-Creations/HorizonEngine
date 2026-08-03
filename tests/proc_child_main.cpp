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

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace {

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

	if (argc < 2) { std::fputs("usage: he_proc_child <mode> [...]\n", stderr); return 2; }
	const std::string mode = argv[1];

	if (mode == "exit")
	{
		return argc > 2 ? std::atoi(argv[2]) : 0;
	}
	if (mode == "out")
	{
		for (int i = 2; i < argc; ++i) std::printf("%s\n", argv[i]);
		return 0;
	}
	if (mode == "err")
	{
		for (int i = 2; i < argc; ++i) std::fprintf(stderr, "%s\n", argv[i]);
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
		const std::size_t bytes = argc > 2 ? static_cast<std::size_t>(std::atol(argv[2])) : 1000000;
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
		for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
		return 0;
	}
	if (mode == "env")
	{
		const char* v = argc > 2 ? std::getenv(argv[2]) : nullptr;
		std::printf("%s\n", v ? v : "<unset>");
		return 0;
	}
	if (mode == "sleep")
	{
		const int ms = argc > 2 ? std::atoi(argv[2]) : 1000;
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		std::printf("finished\n");
		return 0;
	}
	if (mode == "spawn-grandchild")
	{
		// Starts a detached grandchild that writes a marker file after a delay,
		// then blocks. If the runner kills only the direct child, the grandchild
		// survives and the marker appears — which is the failure this detects.
		const std::string marker = argc > 2 ? argv[2] : "";
		const std::string self   = argv[0];
#ifdef _WIN32
		std::string cmd = "\"" + self + "\" delayed-write \"" + marker + "\"";
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
		if (argc > 2)
		{
			std::ofstream out(argv[2]);
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
