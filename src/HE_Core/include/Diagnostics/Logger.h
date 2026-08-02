#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include "Diagnostics/Log.h"
#include <string>

// Legacy front-end for the engine log. New code should use the categorised
// macros in Diagnostics/Log.h instead:
//
//     HE_LOG_INFO(Render, "Backend '%s' ready", name);
//
// Everything here forwards into HE::Log, so old call sites still end up in the
// same file, ring buffer and sinks — they just carry no category (they land in
// "Core") and no source location.
class HE_API Logger
{
public:
	using LogLevel = HE::LogLevel;

	static void Log(LogLevel level, const char* message);
	static void Log(LogLevel level, const std::string& message) { Log(level, message.c_str()); }

	// Explicit category variant for call sites that are not worth converting to
	// the macros but do know where they belong.
	static void LogTo(HE::Log::Cat category, LogLevel level, const char* message);

	// Optional secondary sink: every Log() call is forwarded to it (after the
	// file write). Used by the editor to capture a play session's warnings +
	// errors for the post-PIE report. Plain function pointer + user data so
	// installation is trivially safe; the sink may be called from ANY thread
	// (streaming/export workers log too) — it must synchronize itself.
	// Pass nullptr to uninstall.
	//
	// Only ONE such sink exists (installing a second replaces the first).
	// HE::Log::addSink supports any number of concurrent consumers and hands
	// over the full record; prefer it for anything new.
	using Sink = void(*)(LogLevel level, const char* message, void* user);
	static void setSink(Sink sink, void* user);
};
