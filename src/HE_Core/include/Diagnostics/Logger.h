#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include <string>

class HE_API Logger
{
public:
	using LogLevel = HE::LogLevel;
	static void Log(LogLevel level, const char* message);

	// Optional secondary sink: every Log() call is forwarded to it (after the
	// file write). Used by the editor to capture a play session's warnings +
	// errors for the post-PIE report. Plain function pointer + user data so
	// installation is trivially safe; the sink may be called from ANY thread
	// (streaming/export workers log too) — it must synchronize itself.
	// Pass nullptr to uninstall.
	using Sink = void(*)(LogLevel level, const char* message, void* user);
	static void setSink(Sink sink, void* user);
private:
	std::string logfile;
};