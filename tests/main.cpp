#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include <Diagnostics/Log.h>
#include <cstdlib>

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

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();
	if (context.shouldExit()) return result;
	return result;
}
