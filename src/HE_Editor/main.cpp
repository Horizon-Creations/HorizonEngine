#include "EditorApplication.h"
#include <Diagnostics/CrashHandler.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    // Catch SIGSEGV/SIGABRT/… and write a backtrace to <tmp>/he_crash_<ts>.crash
    // (also flushes the log) so hard crashes leave a diagnosable trail.
    CrashHandler::install();

	std::string startupPath = argv[0];
    EditorApplication app(startupPath);
    return app.Run(argc, argv);
}
