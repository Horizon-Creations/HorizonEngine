#include "Diagnostics/Logger.h"
#include "Diagnostics/GlobalState.h"
#include <fstream>
#include <ctime>
#include <atomic>

static const char* levelName(Logger::LogLevel level)
{
	switch (level)
	{
	case Logger::LogLevel::Trace:    return "TRACE";
	case Logger::LogLevel::Debug:    return "DEBUG";
	case Logger::LogLevel::Info:     return "INFO";
	case Logger::LogLevel::Warning:  return "WARNING";
	case Logger::LogLevel::Error:    return "ERROR";
	case Logger::LogLevel::Critical: return "CRITICAL";
	default:                         return "UNKNOWN";
	}
}

// Secondary sink (editor play-session capture). Atomics: setSink is called from
// the main thread; Log() may run on workers concurrently.
static std::atomic<Logger::Sink> s_sink{ nullptr };
static std::atomic<void*>        s_sinkUser{ nullptr };

void Logger::setSink(Sink sink, void* user)
{
	s_sinkUser.store(user);
	s_sink.store(sink);
}

void Logger::Log(Logger::LogLevel level, const char* message)
{
	if (const Sink sink = s_sink.load())
		sink(level, message, s_sinkUser.load());

	GlobalState& globalState = GlobalState::getInstance();
	std::ofstream& logFile = globalState.getLogFileStream();
	if (!logFile.is_open()) return;

	std::time_t t = std::time(nullptr);
	char timeBuf[32]{};
	std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&t));
	logFile << "[" << timeBuf << "] [" << levelName(level) << "] " << message << '\n';
	logFile.flush();
}