#pragma once
// Internal header: _popen / std::system for Windows, minus the console window.
//
// Both CRT calls run `%COMSPEC% /c <line>` with default creation flags. From a
// process that has a console (he_tests, a terminal) the child shares it and
// nothing shows. From the editor or the game — GUI-subsystem, no console — every
// call gets a console of its own, and with Windows Terminal as the default
// terminal that is a real window that takes the foreground. The editor makes
// three such calls on every start (the cmake probe), more per GameLogic build;
// on a machine where agents launch the editor while a person works, each one
// pulled focus out of whatever that person was doing (Thema 110).
//
// These run the same command line through the same shell, with the same quoting
// contract (HcCodegen's cmdLine() still applies), the same text-mode stream and
// the same exit code. What differs: a caller WITHOUT a console starts the shell
// with CREATE_NO_WINDOW (a caller with one shares it, as before), and the child
// inherits nothing but its own pipe, so two calls on two threads cannot hold each
// other's pipe open. Windows only; POSIX keeps popen/system.
#if defined(_WIN32)
#include <cstdio>
#include <string>

namespace HE {

// Like _popen(line, "r"): the returned stream reads the child's stdout in text
// mode. stderr goes into the same pipe and stdin is NUL — every caller appends
// "2>&1" and none feeds input, so that is what _popen gave them too. nullptr if
// the shell could not be started.
FILE* hiddenPopen(const std::string& line);

// Like _pclose: closes the stream, waits for the shell and returns its exit code.
int hiddenPclose(FILE* stream);

// Like std::system(line), but with stdin, stdout and stderr on NUL. Only for
// lines whose output is discarded anyway (">NUL 2>&1"). -1 if it could not start.
int hiddenSystem(const std::string& line);

} // namespace HE
#endif
