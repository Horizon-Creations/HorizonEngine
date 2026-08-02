#include "Diagnostics/Logger.h"
#include "Diagnostics/Log.h"
#include <atomic>
#include <mutex>

// The legacy Logger is now a thin adapter over HE::Log — one log, one file, one
// ring buffer, one set of sinks.

void Logger::Log(Logger::LogLevel level, const char* message)
{
	LogTo(HE::Log::Cat::Core, level, message);
}

void Logger::LogTo(HE::Log::Cat category, Logger::LogLevel level, const char* message)
{
	// "%s" rather than passing the message as the format: legacy messages are
	// built by string concatenation and routinely contain '%' (percentages,
	// URL escapes, printf snippets in error text).
	HE::Log::write(category, level, nullptr, 0, nullptr, "%s", message ? message : "");
}

// ─── Legacy single-sink adapter ──────────────────────────────────────────────
// Bridges the old (level, message, user) signature onto HE::Log's record sinks.

namespace
{
	std::mutex        g_legacySinkMutex;
	Logger::Sink      g_legacySink     = nullptr;   // guarded by g_legacySinkMutex
	void*             g_legacySinkUser = nullptr;   // guarded by g_legacySinkMutex
	int               g_legacyHandle   = 0;         // guarded by g_legacySinkMutex

	void legacyBridge(const HE::Log::Record& rec, void*)
	{
		// HE::Log already holds its own mutex here, so the sink list cannot
		// change under us; the local mutex only guards g_legacySink itself,
		// which setSink may touch from another thread.
		Logger::Sink sink = nullptr;
		void*        user = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_legacySinkMutex);
			sink = g_legacySink;
			user = g_legacySinkUser;
		}
		if (sink) sink(rec.level, rec.message, user);
	}
}

void Logger::setSink(Sink sink, void* user)
{
	int handleToRemove = 0;
	int handleToAdd    = 0;
	{
		std::lock_guard<std::mutex> lk(g_legacySinkMutex);
		g_legacySink     = sink;
		g_legacySinkUser = user;
		if (!sink && g_legacyHandle)
		{
			handleToRemove = g_legacyHandle;
			g_legacyHandle = 0;
		}
		else if (sink && !g_legacyHandle)
		{
			handleToAdd = 1;
		}
	}

	// addSink/removeSink take the log mutex — call them outside our own lock so
	// a concurrent log record (which holds the log mutex and wants ours) cannot
	// deadlock against us.
	if (handleToRemove) HE::Log::removeSink(handleToRemove);
	if (handleToAdd)
	{
		const int h = HE::Log::addSink(&legacyBridge, nullptr);
		std::lock_guard<std::mutex> lk(g_legacySinkMutex);
		g_legacyHandle = h;
	}
}
