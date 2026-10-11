#include "EditorApplication.h"
#include "HeprojRegistration.h"
#include <Diagnostics/CrashHandler.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    // `HorizonEditor --register-file-types`: register .heproj for this editor, say what
    // happened and leave, before anything else exists — no window, no log file, no
    // config. The fallback for the editor's own registration at start, and what CI
    // runs against the packaged editor (HeprojRegistration.h).
    if (const int rc = HeprojRegistration::runCommandLine(argc, argv); rc >= 0)
        return rc;

    // Catch SIGSEGV/SIGABRT/… and write a backtrace to <tmp>/he_crash_<ts>.crash
    // (also flushes the log) so hard crashes leave a diagnosable trail.
    CrashHandler::install();

	std::string startupPath = argv[0];
    EditorApplication app(startupPath);
    return app.Run(argc, argv);
}
