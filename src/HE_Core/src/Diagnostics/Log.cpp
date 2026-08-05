#include "Diagnostics/Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>  // RtlGetVersion probe + GlobalMemoryStatusEx (system info block)
  #include <intrin.h>   // __cpuid — CPU brand string
  #include <io.h>       // _isatty/_fileno
  #include <process.h>  // _getpid
  #define HE_ISATTY(fd) _isatty(fd)
  #define HE_FILENO(f)  _fileno(f)
  #define HE_GETPID()   _getpid()
#else
  #include <unistd.h>
  #define HE_ISATTY(fd) isatty(fd)
  #define HE_FILENO(f)  fileno(f)
  #define HE_GETPID()   getpid()
#endif

#if defined(__APPLE__) || defined(__linux__)
  #include <sys/utsname.h>
#endif
#ifdef __APPLE__
  #include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;

namespace HE::Log
{
namespace
{
	constexpr int kCatCount = static_cast<int>(Cat::Count);

	// Index-aligned with Cat. Padded to 11 characters so the column stays put.
	constexpr const char* kCategoryNames[kCatCount] = {
		"Core",        "Config",    "Platform",    "Window",     "Input",
		"Job",         "Memory",    "Asset",       "Content",    "Pak",
		"Export",      "Render",    "RHI",         "Shader",     "Material",
		"Scene",       "World",     "Serialize",   "Physics",    "Collision",
		"Animation",   "Nav",       "Audio",       "Particle",   "Terrain",
		"Foliage",     "LOD",       "Weather",     "UI",         "Widget",
		"Script",      "Lua",       "Python",      "HorizonCode","GameLogic",
		"Net",         "Editor",    "Tool",        "Profiler",   "SourceControl",
	};
	static_assert(sizeof(kCategoryNames) / sizeof(kCategoryNames[0]) == kCatCount,
	              "kCategoryNames must stay index-aligned with HE::Log::Cat");

	constexpr const char* kLevelNames[] = {
		"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", "OFF"
	};
	// Fixed-width variants for the log column.
	constexpr const char* kLevelShort[] = {
		"TRACE", "DEBUG", " INFO", " WARN", "ERROR", " CRIT", "  OFF"
	};

	int levelIndex(Level l)
	{
		const int i = static_cast<int>(l);
		return (i >= 0 && i <= static_cast<int>(Level::Off)) ? i : static_cast<int>(Level::Info);
	}

	bool iequals(const char* a, const char* b)
	{
		for (; *a && *b; ++a, ++b)
		{
			const char ca = (*a >= 'A' && *a <= 'Z') ? char(*a - 'A' + 'a') : *a;
			const char cb = (*b >= 'A' && *b <= 'Z') ? char(*b - 'A' + 'a') : *b;
			if (ca != cb) return false;
		}
		return *a == *b;
	}

	// ── Global state ─────────────────────────────────────────────────────────
	// Verbosities are atomics so the filter check in the macros never takes a
	// lock; everything that touches the output channels sits behind s_mutex.

	std::array<std::atomic<uint8_t>, kCatCount>& verbosities()
	{
		// Atomics are not copyable, so the array is default-constructed and then
		// seeded in place on first use (function-local static → thread-safe init).
		static std::array<std::atomic<uint8_t>, kCatCount> v{};
		static const bool seeded = [] {
			for (auto& e : v) e.store(static_cast<uint8_t>(Level::Info), std::memory_order_relaxed);
			return true;
		}();
		(void)seeded;
		return v;
	}

	std::atomic<uint8_t>  s_globalFloor{ static_cast<uint8_t>(Level::Trace) };
	std::atomic<uint8_t>  s_sourceLocLevel{ static_cast<uint8_t>(Level::Warning) };
	std::atomic<bool>     s_console{ true };
	std::atomic<uint64_t> s_frame{ 0 };
	std::atomic<uint64_t> s_counts[static_cast<int>(Level::Off) + 1]{};

	std::mutex& mutex()
	{
		static std::mutex m;
		return m;
	}

	FILE*       s_file = nullptr;   // guarded by mutex()
	std::string s_filePath;         // guarded by mutex()

	struct SinkSlot { SinkFn fn; void* user; int handle; };
	std::vector<SinkSlot>& sinks()          // guarded by mutex()
	{
		static std::vector<SinkSlot> v;
		return v;
	}
	int s_nextSinkHandle = 1;

	// Fixed ring buffer — no allocation, so the crash handler can read it from a
	// signal handler.
	struct Ring
	{
		char lines[kRingCapacity][kRingLineSize]{};
		int  head  = 0;   // next write slot
		int  count = 0;
	};
	Ring& ring()
	{
		static Ring r;
		return r;
	}

	// Consecutive-duplicate suppression: a per-frame error would otherwise bury
	// everything else. Guarded by mutex().
	char     s_lastMessage[kRingLineSize]{};
	uint8_t  s_lastLevel = 0xFF;
	uint8_t  s_lastCat   = 0xFF;
	uint64_t s_repeatCount = 0;

	std::atomic<bool>    s_startTimeSet{ false };
	std::chrono::steady_clock::time_point s_startTime{};

	// ── Thread identity ──────────────────────────────────────────────────────
	struct ThreadIdent
	{
		char name[16]{};
		ThreadIdent()
		{
			// Default to the numeric id so unnamed threads are still
			// distinguishable in the log.
			const size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
			std::snprintf(name, sizeof(name), "t%04x", unsigned(h & 0xFFFFu));
		}
	};
	thread_local ThreadIdent t_ident;

	// ── Formatting helpers ───────────────────────────────────────────────────

	const char* shortFileName(const char* path)
	{
		if (!path) return nullptr;
		const char* slash = std::strrchr(path, '/');
#ifdef _WIN32
		const char* back = std::strrchr(path, '\\');
		if (back && (!slash || back > slash)) slash = back;
#endif
		return slash ? slash + 1 : path;
	}

	// "[14:22:07.318] [ INFO] [Render     ] [Main    ] [f    120] message (File.cpp:42)"
	// Returns the number of characters written (excluding the NUL).
	int formatLine(char* out, size_t outSize, const Record& rec)
	{
		const std::time_t secs = static_cast<std::time_t>(rec.unixMillis / 1000);
		const int         ms   = static_cast<int>(rec.unixMillis % 1000);
		char timeBuf[16] = "??:??:??";
#ifdef _WIN32
		std::tm tmBuf{};
		if (localtime_s(&tmBuf, &secs) == 0)
			std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);
#else
		std::tm tmBuf{};
		if (localtime_r(&secs, &tmBuf))
			std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);
#endif

		int n = std::snprintf(out, outSize, "[%s.%03d] [%s] [%-11s] [%-8s] [f%7llu] %s",
		                      timeBuf, ms, kLevelShort[levelIndex(rec.level)],
		                      categoryName(rec.category), rec.thread,
		                      static_cast<unsigned long long>(rec.frame), rec.message);
		if (n < 0) return 0;

		const bool wantLoc = rec.file &&
			static_cast<int>(rec.level) >= int(s_sourceLocLevel.load(std::memory_order_relaxed));
		if (wantLoc && static_cast<size_t>(n) < outSize)
		{
			const int extra = std::snprintf(out + n, outSize - static_cast<size_t>(n),
			                                "  (%s:%d)", shortFileName(rec.file), rec.line);
			if (extra > 0) n += extra;
		}
		if (static_cast<size_t>(n) >= outSize) n = static_cast<int>(outSize) - 1;
		return n;
	}

	const char* ansiColor(Level level)
	{
		switch (level)
		{
		case Level::Trace:    return "\x1b[90m";  // bright black
		case Level::Debug:    return "\x1b[36m";  // cyan
		case Level::Info:     return "";
		case Level::Warning:  return "\x1b[33m";  // yellow
		case Level::Error:    return "\x1b[31m";  // red
		case Level::Critical: return "\x1b[1;41m";// bold on red
		default:              return "";
		}
	}

	bool consoleIsTty(FILE* stream)
	{
		static const bool outTty = HE_ISATTY(HE_FILENO(stdout)) != 0;
		static const bool errTty = HE_ISATTY(HE_FILENO(stderr)) != 0;
		return stream == stderr ? errTty : outTty;
	}

	// Everything below assumes mutex() is held.

	void pushRing(const char* line, int length)
	{
		Ring& r = ring();
		char* slot = r.lines[r.head];
		const int n = std::min(length, kRingLineSize - 1);
		std::memcpy(slot, line, static_cast<size_t>(n));
		slot[n] = '\0';
		r.head = (r.head + 1) % kRingCapacity;
		if (r.count < kRingCapacity) ++r.count;
	}

	void emitLine(const Record& rec, const char* line, int length)
	{
		pushRing(line, length);

		if (s_file)
		{
			std::fwrite(line, 1, static_cast<size_t>(length), s_file);
			std::fputc('\n', s_file);
			// Flushed per record on purpose: a crash must not eat the last lines,
			// which is exactly when the log matters most.
			std::fflush(s_file);
		}

		if (s_console.load(std::memory_order_relaxed))
		{
			FILE* stream = rec.level >= Level::Warning ? stderr : stdout;
			const char* color = consoleIsTty(stream) ? ansiColor(rec.level) : "";
			if (color && *color)
				std::fprintf(stream, "%s%s\x1b[0m\n", color, line);
			else
				std::fprintf(stream, "%s\n", line);
			if (stream == stderr) std::fflush(stream);
		}

		for (const SinkSlot& s : sinks())
			if (s.fn) s.fn(rec, s.user);
	}

	// Emits the pending "repeated N times" note. Caller holds the mutex.
	void flushRepeats()
	{
		if (s_repeatCount == 0) return;
		const uint64_t repeats = s_repeatCount;
		s_repeatCount = 0;

		char msg[128];
		std::snprintf(msg, sizeof(msg), "(previous message repeated %llu more time%s)",
		              static_cast<unsigned long long>(repeats), repeats == 1 ? "" : "s");

		Record rec{};
		rec.level      = static_cast<Level>(s_lastLevel);
		rec.category   = static_cast<Cat>(s_lastCat);
		rec.message    = msg;
		rec.file       = nullptr;
		rec.line       = 0;
		rec.function   = nullptr;
		rec.thread     = t_ident.name;
		rec.frame      = s_frame.load(std::memory_order_relaxed);
		rec.unixMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();

		char line[kRingLineSize];
		const int n = formatLine(line, sizeof(line), rec);
		emitLine(rec, line, n);
	}
} // namespace

// ─── Names ───────────────────────────────────────────────────────────────────

const char* categoryName(Cat cat)
{
	const int i = static_cast<int>(cat);
	return (i >= 0 && i < kCatCount) ? kCategoryNames[i] : "?";
}

bool categoryFromName(const char* name, Cat& out)
{
	if (!name || !*name) return false;
	for (int i = 0; i < kCatCount; ++i)
	{
		if (iequals(name, kCategoryNames[i]))
		{
			out = static_cast<Cat>(i);
			return true;
		}
	}
	return false;
}

const char* levelName(Level level) { return kLevelNames[levelIndex(level)]; }

bool levelFromName(const char* name, Level& out)
{
	if (!name || !*name) return false;
	for (int i = 0; i <= static_cast<int>(Level::Off); ++i)
	{
		if (iequals(name, kLevelNames[i]))
		{
			out = static_cast<Level>(i);
			return true;
		}
	}
	// Aliases so specs read naturally.
	if (iequals(name, "verbose") || iequals(name, "all")) { out = Level::Trace;   return true; }
	if (iequals(name, "warn"))                            { out = Level::Warning; return true; }
	if (iequals(name, "err"))                             { out = Level::Error;   return true; }
	if (iequals(name, "fatal"))                           { out = Level::Critical;return true; }
	if (iequals(name, "none") || iequals(name, "silent")) { out = Level::Off;     return true; }
	return false;
}

// ─── Filtering ───────────────────────────────────────────────────────────────

void setVerbosity(Cat cat, Level level)
{
	const int i = static_cast<int>(cat);
	if (i < 0 || i >= kCatCount) return;
	verbosities()[static_cast<size_t>(i)].store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

void setVerbosity(Level level)
{
	for (auto& v : verbosities())
		v.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

Level verbosity(Cat cat)
{
	const int i = static_cast<int>(cat);
	if (i < 0 || i >= kCatCount) return Level::Off;
	return static_cast<Level>(verbosities()[static_cast<size_t>(i)].load(std::memory_order_relaxed));
}

bool enabled(Cat cat, Level level)
{
	const int i = static_cast<int>(cat);
	if (i < 0 || i >= kCatCount) return false;
	if (level >= Level::Off) return false;
	const uint8_t lv = static_cast<uint8_t>(level);
	return lv >= verbosities()[static_cast<size_t>(i)].load(std::memory_order_relaxed) &&
	       lv >= s_globalFloor.load(std::memory_order_relaxed);
}

void configureFromString(const char* spec)
{
	if (!spec || !*spec) return;

	std::string s(spec);
	size_t pos = 0;
	while (pos <= s.size())
	{
		size_t end = s.find_first_of(",;", pos);
		if (end == std::string::npos) end = s.size();
		std::string entry = s.substr(pos, end - pos);
		pos = end + 1;

		// trim
		const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
		entry.erase(entry.begin(), std::find_if(entry.begin(), entry.end(), notSpace));
		entry.erase(std::find_if(entry.rbegin(), entry.rend(), notSpace).base(), entry.end());
		if (entry.empty()) continue;

		const size_t eq = entry.find('=');
		if (eq == std::string::npos)
		{
			// A bare level ("Trace") sets everything, matching "-verbose".
			Level lv;
			if (levelFromName(entry.c_str(), lv)) setVerbosity(lv);
			else HE_LOG_WARN(Config, "Log verbosity spec: unknown entry '%s'", entry.c_str());
			continue;
		}

		std::string catName = entry.substr(0, eq);
		std::string lvName  = entry.substr(eq + 1);
		catName.erase(std::find_if(catName.rbegin(), catName.rend(), notSpace).base(), catName.end());
		lvName.erase(lvName.begin(), std::find_if(lvName.begin(), lvName.end(), notSpace));

		Level lv;
		if (!levelFromName(lvName.c_str(), lv))
		{
			HE_LOG_WARN(Config, "Log verbosity spec: unknown level '%s' in '%s'",
			            lvName.c_str(), entry.c_str());
			continue;
		}
		if (catName == "*" || iequals(catName.c_str(), "all"))
		{
			setVerbosity(lv);
			continue;
		}
		Cat cat;
		if (!categoryFromName(catName.c_str(), cat))
		{
			HE_LOG_WARN(Config, "Log verbosity spec: unknown category '%s'", catName.c_str());
			continue;
		}
		setVerbosity(cat, lv);
	}
}

std::string verbositySpec()
{
	std::string out;
	for (int i = 0; i < kCatCount; ++i)
	{
		const Level lv = verbosity(static_cast<Cat>(i));
		if (lv == Level::Info) continue;   // default
		if (!out.empty()) out += ',';
		out += kCategoryNames[i];
		out += '=';
		out += levelName(lv);
	}
	return out;
}

// ─── Output channels ─────────────────────────────────────────────────────────

bool openLogFile(const std::string& path, int keepBackups)
{
	std::lock_guard<std::mutex> lock(mutex());
	if (s_file)
	{
		std::fclose(s_file);
		s_file = nullptr;
	}

	{
		std::error_code ec;
		const fs::path dir = fs::path(path).parent_path();
		if (!dir.empty()) fs::create_directories(dir, ec);
	}

	// Rotate: HorizonEngine.2.log → .3.log, .1.log → .2.log, .log → .1.log.
	// Keeps the log of a run that crashed (and was restarted) available.
	if (keepBackups > 0)
	{
		std::error_code ec;
		const fs::path p    = fs::path(path);
		const fs::path dir  = p.parent_path();
		const std::string stem = p.stem().string();
		const std::string ext  = p.extension().string();
		const auto backup = [&](int n) {
			return dir / (stem + "." + std::to_string(n) + ext);
		};
		fs::remove(backup(keepBackups), ec);
		for (int n = keepBackups - 1; n >= 1; --n)
			fs::rename(backup(n), backup(n + 1), ec);
		fs::rename(p, backup(1), ec);
	}

	s_file = std::fopen(path.c_str(), "w");
	s_filePath = s_file ? path : std::string();
	return s_file != nullptr;
}

void closeLogFile()
{
	std::lock_guard<std::mutex> lock(mutex());
	flushRepeats();
	if (s_file)
	{
		std::fclose(s_file);
		s_file = nullptr;
	}
	s_filePath.clear();
}

std::string logFilePath()
{
	std::lock_guard<std::mutex> lock(mutex());
	return s_filePath;
}

void flush()
{
	std::lock_guard<std::mutex> lock(mutex());
	flushRepeats();
	if (s_file) std::fflush(s_file);
	std::fflush(stdout);
	std::fflush(stderr);
}

void setConsoleEnabled(bool e) { s_console.store(e, std::memory_order_relaxed); }
bool consoleEnabled()          { return s_console.load(std::memory_order_relaxed); }
void setSourceLocationLevel(Level minLevel)
{
	s_sourceLocLevel.store(static_cast<uint8_t>(minLevel), std::memory_order_relaxed);
}

// ─── Context ─────────────────────────────────────────────────────────────────

void     setFrameNumber(uint64_t frame) { s_frame.store(frame, std::memory_order_relaxed); }
uint64_t frameNumber()                  { return s_frame.load(std::memory_order_relaxed); }

void setThreadName(const char* name)
{
	if (!name) return;
	std::snprintf(t_ident.name, sizeof(t_ident.name), "%s", name);
}

const char* threadName() { return t_ident.name; }

// ─── Sinks ───────────────────────────────────────────────────────────────────

int addSink(SinkFn sink, void* user)
{
	if (!sink) return 0;
	std::lock_guard<std::mutex> lock(mutex());
	const int handle = s_nextSinkHandle++;
	sinks().push_back(SinkSlot{ sink, user, handle });
	return handle;
}

void removeSink(int handle)
{
	if (handle == 0) return;
	std::lock_guard<std::mutex> lock(mutex());
	auto& v = sinks();
	v.erase(std::remove_if(v.begin(), v.end(),
	                       [handle](const SinkSlot& s) { return s.handle == handle; }),
	        v.end());
}

// ─── Ring buffer ─────────────────────────────────────────────────────────────

void forEachRecent(void (*fn)(const char* line, void* user), void* user)
{
	if (!fn) return;
	// try_lock, not lock: this runs from the crash handler, where the crashing
	// thread may already own the mutex. A torn line beats a hung crash report.
	const bool locked = mutex().try_lock();
	const Ring& r = ring();
	const int start = (r.head - r.count + kRingCapacity) % kRingCapacity;
	for (int i = 0; i < r.count; ++i)
		fn(r.lines[(start + i) % kRingCapacity], user);
	if (locked) mutex().unlock();
}

std::vector<std::string> recentLines(int maxLines)
{
	std::lock_guard<std::mutex> lock(mutex());
	const Ring& r = ring();
	int count = r.count;
	if (maxLines > 0 && maxLines < count) count = maxLines;
	const int start = (r.head - count + kRingCapacity) % kRingCapacity;

	std::vector<std::string> out;
	out.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i)
		out.emplace_back(r.lines[(start + i) % kRingCapacity]);
	return out;
}

uint64_t messageCount(Level level)
{
	return s_counts[levelIndex(level)].load(std::memory_order_relaxed);
}

// ─── Emission ────────────────────────────────────────────────────────────────

void writeV(Cat cat, Level level, const char* file, int line, const char* function,
            const char* fmt, va_list args)
{
	if (!enabled(cat, level)) return;

	char message[kRingLineSize];
	if (fmt)
	{
		const int n = std::vsnprintf(message, sizeof(message), fmt, args);
		if (n < 0) std::snprintf(message, sizeof(message), "<format error: %s>", fmt);
		else if (static_cast<size_t>(n) >= sizeof(message))
		{
			// Mark truncation explicitly so nobody chases a "corrupt" message.
			std::snprintf(message + sizeof(message) - 5, 5, "...");
		}
	}
	else
	{
		message[0] = '\0';
	}

	s_counts[levelIndex(level)].fetch_add(1, std::memory_order_relaxed);

	Record rec{};
	rec.level      = level;
	rec.category   = cat;
	rec.message    = message;
	rec.file       = file;
	rec.line       = line;
	rec.function   = function;
	rec.thread     = t_ident.name;
	rec.frame      = s_frame.load(std::memory_order_relaxed);
	rec.unixMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	std::lock_guard<std::mutex> lock(mutex());

	// Collapse identical consecutive records (per-frame errors otherwise bury
	// the log). Counted and reported when the next distinct record arrives.
	if (s_lastLevel == static_cast<uint8_t>(level) &&
	    s_lastCat == static_cast<uint8_t>(cat) &&
	    std::strcmp(s_lastMessage, message) == 0)
	{
		++s_repeatCount;
		return;
	}
	flushRepeats();
	std::snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", message);
	s_lastLevel = static_cast<uint8_t>(level);
	s_lastCat   = static_cast<uint8_t>(cat);

	char lineBuf[kRingLineSize];
	const int n = formatLine(lineBuf, sizeof(lineBuf), rec);
	emitLine(rec, lineBuf, n);
}

void write(Cat cat, Level level, const char* file, int line, const char* function,
           const char* fmt, ...)
{
	if (!enabled(cat, level)) return;
	va_list args;
	va_start(args, fmt);
	writeV(cat, level, file, line, function, fmt, args);
	va_end(args);
}

// ─── Scope timing ────────────────────────────────────────────────────────────

int64_t detail::monotonicMillis()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace
{
	int64_t monotonicNanos()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

ScopeTimer::ScopeTimer(Cat cat, Level level, double thresholdMs, const char* file,
                       int line, const char* function, const char* label)
	: m_cat(cat), m_level(level), m_thresholdMs(thresholdMs), m_file(file),
	  m_line(line), m_function(function), m_startNanos(0)
{
	std::snprintf(m_label, sizeof(m_label), "%s", label ? label : "");
	// A threshold-scope is silent on entry — only the overrun is interesting.
	if (m_thresholdMs <= 0.0 && enabled(cat, level))
		write(cat, level, file, line, function, "%s ...", m_label);
	m_startNanos = monotonicNanos();
}

double ScopeTimer::elapsedMs() const
{
	return double(monotonicNanos() - m_startNanos) / 1.0e6;
}

ScopeTimer::~ScopeTimer()
{
	const double ms = elapsedMs();
	if (m_thresholdMs > 0.0)
	{
		if (ms >= m_thresholdMs)
			write(m_cat, m_level, m_file, m_line, m_function,
			      "%s took %.2f ms (budget %.2f ms)", m_label, ms, m_thresholdMs);
	}
	else
	{
		write(m_cat, m_level, m_file, m_line, m_function, "%s done (%.2f ms)", m_label, ms);
	}
}

// ─── Startup / shutdown diagnostics ──────────────────────────────────────────

namespace
{
	const char* buildConfigName()
	{
#if defined(NDEBUG)
		return "Release";
#else
		return "Debug";
#endif
	}

	const char* compilerName()
	{
#if defined(__clang__)
		return "Clang " __clang_version__;
#elif defined(_MSC_VER)
		return "MSVC";
#elif defined(__GNUC__)
		return "GCC " __VERSION__;
#else
		return "unknown";
#endif
	}

	const char* platformName()
	{
#if defined(_WIN32)
		return "Windows";
#elif defined(__APPLE__)
		return "macOS";
#elif defined(__linux__)
		return "Linux";
#else
		return "unknown";
#endif
	}

#if defined(__linux__)
	// First "Key=value" / "Key : value" match in a /etc or /proc text file, with
	// surrounding quotes and whitespace stripped. Empty when the file or the key
	// is missing — every caller has a fallback, none of this is load-bearing.
	std::string firstFieldFromFile(const char* path, const char* key, char separator)
	{
		std::ifstream in(path);
		if (!in) return {};
		const size_t keyLen = std::strlen(key);
		std::string line;
		while (std::getline(in, line))
		{
			if (line.compare(0, keyLen, key) != 0) continue;
			const size_t sep = line.find(separator, keyLen);
			if (sep == std::string::npos) continue;
			std::string value = line.substr(sep + 1);
			const size_t first = value.find_first_not_of(" \t\"");
			const size_t last  = value.find_last_not_of(" \t\"\r");
			if (first == std::string::npos) continue;
			return value.substr(first, last - first + 1);
		}
		return {};
	}
#endif

	std::string osVersionString()
	{
#if defined(_WIN32)
		// GetVersionEx reports 6.2 for anything past Windows 8 unless the exe
		// carries a compatibility manifest. RtlGetVersion always tells the truth;
		// resolving it from the already-loaded ntdll keeps this link-dependency
		// free. The 22000 build cut-off is Microsoft's own Windows 11 boundary.
		using RtlGetVersionFn = long(__stdcall*)(OSVERSIONINFOEXW*);
		if (HMODULE nt = ::GetModuleHandleW(L"ntdll.dll"))
		{
			if (auto rtlGetVersion =
			        reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(nt, "RtlGetVersion")))
			{
				OSVERSIONINFOEXW vi{};
				vi.dwOSVersionInfoSize = sizeof(vi);
				if (rtlGetVersion(&vi) == 0)
				{
					char buf[96];
					const char* name = vi.dwMajorVersion == 10
						? (vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10")
						: "Windows";
					std::snprintf(buf, sizeof(buf), "%s (%lu.%lu build %lu)", name,
					              static_cast<unsigned long>(vi.dwMajorVersion),
					              static_cast<unsigned long>(vi.dwMinorVersion),
					              static_cast<unsigned long>(vi.dwBuildNumber));
					return buf;
				}
			}
		}
#elif defined(__APPLE__) || defined(__linux__)
		struct utsname u{};
		if (uname(&u) == 0)
		{
			std::string kernel = std::string(u.sysname) + " " + u.release + " (" + u.machine + ")";
	#if defined(__linux__)
			// The kernel version alone rarely identifies a Linux bug; the distro does.
			const std::string distro = firstFieldFromFile("/etc/os-release", "PRETTY_NAME", '=');
			if (!distro.empty()) return distro + " — " + kernel;
	#endif
			return kernel;
		}
#endif
		return "unknown";
	}

	std::string cpuBrandString()
	{
#if defined(__APPLE__)
		char brand[128]{};
		size_t size = sizeof(brand);
		if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0)
			return brand;
#elif defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
		// Leaves 0x80000002..4 spell the brand string 16 bytes at a time; leaf
		// 0x80000000 reports how far the extended range goes.
		int regs[4]{};
		__cpuid(regs, static_cast<int>(0x80000000u));
		if (static_cast<unsigned>(regs[0]) >= 0x80000004u)
		{
			char brand[49]{};
			for (int leaf = 0; leaf < 3; ++leaf)
			{
				__cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(leaf)));
				std::memcpy(brand + leaf * 16, regs, 16);
			}
			brand[48] = '\0';
			const std::string s(brand);
			const size_t first = s.find_first_not_of(' ');
			if (first != std::string::npos) return s.substr(first);
		}
#elif defined(__linux__)
		// x86 spells it "model name"; ARM boards usually only offer "Hardware".
		std::string brand = firstFieldFromFile("/proc/cpuinfo", "model name", ':');
		if (brand.empty()) brand = firstFieldFromFile("/proc/cpuinfo", "Hardware", ':');
		if (!brand.empty()) return brand;
#endif
		return "unknown";
	}

	uint64_t physicalMemoryMB()
	{
#if defined(__APPLE__)
		uint64_t mem = 0;
		size_t size = sizeof(mem);
		if (sysctlbyname("hw.memsize", &mem, &size, nullptr, 0) == 0)
			return mem / (1024 * 1024);
#elif defined(_WIN32)
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (::GlobalMemoryStatusEx(&status))
			return status.ullTotalPhys / (1024 * 1024);
#elif defined(__linux__)
		const long pages = sysconf(_SC_PHYS_PAGES);
		const long page  = sysconf(_SC_PAGE_SIZE);
		if (pages > 0 && page > 0)
			return (uint64_t(pages) * uint64_t(page)) / (1024 * 1024);
#endif
		return 0;
	}
}

std::string systemInfoBlock()
{
	char buf[512];
	std::string out;

	std::snprintf(buf, sizeof(buf), "Build    : %s | %s | C++%ld | built %s %s",
	              buildConfigName(), compilerName(), long(__cplusplus), __DATE__, __TIME__);
	out += buf;
	std::snprintf(buf, sizeof(buf), "\nPlatform : %s — %s",
	              platformName(), osVersionString().c_str());
	out += buf;
	std::snprintf(buf, sizeof(buf), "\nCPU      : %s (%u hardware threads)",
	              cpuBrandString().c_str(), std::thread::hardware_concurrency());
	out += buf;
	if (const uint64_t mb = physicalMemoryMB())
	{
		std::snprintf(buf, sizeof(buf), "\nMemory   : %llu MB physical",
		              static_cast<unsigned long long>(mb));
		out += buf;
	}
	return out;
}

void logStartupBanner(const char* appName, int argc, char** argv)
{
	if (!s_startTimeSet.exchange(true))
		s_startTime = std::chrono::steady_clock::now();

	std::time_t t = std::time(nullptr);
	char dateBuf[32] = "?";
#ifdef _WIN32
	std::tm tmBuf{};
	if (localtime_s(&tmBuf, &t) == 0) std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
#else
	std::tm tmBuf{};
	if (localtime_r(&t, &tmBuf)) std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
#endif

	HE_LOG_INFO(Core, "==================== HorizonEngine ====================");
	HE_LOG_INFO(Core, "Application : %s", appName ? appName : "(unknown)");
	HE_LOG_INFO(Core, "Started     : %s", dateBuf);
	HE_LOG_INFO(Core, "Build       : %s | %s | C++%ld | built %s %s",
	            buildConfigName(), compilerName(), long(__cplusplus), __DATE__, __TIME__);
	HE_LOG_INFO(Core, "Platform    : %s — %s", platformName(), osVersionString().c_str());
	HE_LOG_INFO(Core, "CPU         : %s (%u hardware threads)",
	            cpuBrandString().c_str(), std::thread::hardware_concurrency());
	if (const uint64_t mb = physicalMemoryMB())
		HE_LOG_INFO(Core, "Memory      : %llu MB physical", static_cast<unsigned long long>(mb));
	HE_LOG_INFO(Core, "Process     : pid %d", int(HE_GETPID()));

	std::error_code ec;
	HE_LOG_INFO(Core, "Working dir : %s", fs::current_path(ec).string().c_str());
	if (argc > 0 && argv && argv[0])
		HE_LOG_INFO(Core, "Executable  : %s", argv[0]);
	if (argc > 1 && argv)
	{
		std::string cmd;
		for (int i = 1; i < argc; ++i)
		{
			if (!cmd.empty()) cmd += ' ';
			cmd += argv[i] ? argv[i] : "";
		}
		HE_LOG_INFO(Core, "Arguments   : %s", cmd.c_str());
	}
	{
		const std::string spec = verbositySpec();
		HE_LOG_INFO(Core, "Log filter  : %s", spec.empty() ? "default (Info)" : spec.c_str());
	}
	HE_LOG_INFO(Core, "=======================================================");
}

void logShutdownSummary()
{
	double upSeconds = 0.0;
	if (s_startTimeSet.load())
		upSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_startTime).count();

	HE_LOG_INFO(Core, "Session summary: %.1f s uptime, %llu frames, "
	                  "%llu warning(s), %llu error(s), %llu critical",
	            upSeconds,
	            static_cast<unsigned long long>(frameNumber()),
	            static_cast<unsigned long long>(messageCount(Level::Warning)),
	            static_cast<unsigned long long>(messageCount(Level::Error)),
	            static_cast<unsigned long long>(messageCount(Level::Critical)));
	flush();
}

} // namespace HE::Log
