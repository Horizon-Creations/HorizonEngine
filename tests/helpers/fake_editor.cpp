// A stand-in for HorizonEditor in test_selfupdate_apply.cpp: no engine
// dependency, just enough behaviour to drive SelfUpdateApplyCore's state
// machine from the outside. Plays three different roles across the test cases
// (the editor being waited on, the new build being health-checked, the old
// build being relaunched after a rollback) — which one just depends on the
// flags the test passes it.
//
//   --marker <path>     write "<pid>\n<argv[1]>\n<argv[2]>\n..." here on start
//   --sleep-ms <n>      stay alive this long before exiting (default 4000)
//   --exit-code <n>     exit code after the sleep (default 0)
//
// Unknown arguments are ignored (not an error) so a test can pass the plan's
// relaunchArgs straight through without this program choking on them.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
static long long currentPid() { return static_cast<long long>(::GetCurrentProcessId()); }
#else
#include <unistd.h>
static long long currentPid() { return static_cast<long long>(::getpid()); }
#endif

int main(int argc, char** argv)
{
	std::string marker;
	int sleepMs = 4000;
	int exitCode = 0;
	std::vector<std::string> passthrough;

	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--marker" && i + 1 < argc) marker = argv[++i];
		else if (a == "--sleep-ms" && i + 1 < argc) sleepMs = std::atoi(argv[++i]);
		else if (a == "--exit-code" && i + 1 < argc) exitCode = std::atoi(argv[++i]);
		else passthrough.push_back(a);
	}

	if (!marker.empty())
	{
		std::ofstream f(marker, std::ios::binary | std::ios::trunc);
		f << currentPid() << "\n";
		for (const std::string& a : passthrough) f << a << "\n";
	}

	if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
	return exitCode;
}
