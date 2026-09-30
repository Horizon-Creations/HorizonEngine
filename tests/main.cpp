#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "TestFsUtil.h"
#include <Diagnostics/Log.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
	// The engine mirrors its log to the console by default, which would bury the
	// doctest report under thousands of lines. Records still go to the ring
	// buffer (so the logging tests can inspect them) and to any file a test opens.
	// Set HE_TEST_LOG_CONSOLE=1 to see them while debugging a failure.
	if (!std::getenv("HE_TEST_LOG_CONSOLE"))
		HE::Log::setConsoleEnabled(false);
	HE::Log::setThreadName("Test");

	// The collab tests host twenty-five sessions, and hosting is not a local act:
	// it announces itself on the LAN every two seconds, asks the router to
	// forward a port, and registers the session on the public directory. On a
	// developer machine that means the editor next door lists twenty-five
	// sessions called "Anna" that nobody can join, and the live directory
	// collects the same number of ghosts per run. Loopback traffic between the
	// test's own host and client is untouched — that is the thing under test.
#if defined(_WIN32)
	_putenv_s("HE_COLLAB_OFFLINE", "1");
#else
	setenv("HE_COLLAB_OFFLINE", "1", 1);
#endif

	// Every listener the suite opens binds the loopback address rather than
	// every interface (see HE::Net::socketLoopbackOnly). On Windows the first
	// run of an exe from a new path that binds a non-loopback address raises the
	// Defender Firewall dialog, which takes the foreground from whoever is at
	// the machine, and every fresh build directory is a new path. The handful of
	// tests about dual-stack or LAN-wide reachability skip, and say so. Set
	// HE_NET_LOOPBACK_ONLY=0 to run them for real (CI does). Windows only by
	// default: that is where the dialog is and where the mode was verified;
	// elsewhere HE_NET_LOOPBACK_ONLY=1 opts in.
#if defined(_WIN32)
	if (!std::getenv("HE_NET_LOOPBACK_ONLY"))
		_putenv_s("HE_NET_LOOPBACK_ONLY", "1");
#endif

	// Every test that reads or writes GlobalState's config would otherwise land
	// in the per-user settings file (~/Library/Application Support/HorizonEngine,
	// %APPDATA%\HorizonEngine) — the real one of whoever runs the suite. A run
	// reset LastProjectPath, KnownProjects and the chosen RHI to the defaults.
	// configFilePath() resolves once, so this has to be in place before the first
	// test touches GlobalState; a scratch directory per process (ctest starts one
	// per source file) keeps parallel runs apart. Tests that pin a directory with
	// useShippedConfig() still get theirs: this ranks below the pin.
	const std::filesystem::path configDir = std::filesystem::temp_directory_path()
		/ ("he_tests_config_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(configDir);
#if defined(_WIN32)
	_putenv_s("HE_CONFIG_FALLBACK_DIR", configDir.string().c_str());
#else
	setenv("HE_CONFIG_FALLBACK_DIR", configDir.string().c_str(), 1);
#endif

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();
	he_test::removeAllQuiet(configDir);
	return result;
}
