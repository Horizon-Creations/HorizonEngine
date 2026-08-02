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

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	const int result = context.run();
	if (context.shouldExit()) return result;
	return result;
}
