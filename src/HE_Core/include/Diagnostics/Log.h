#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// HorizonEngine logging
//
// One engine-wide log with per-category verbosity, printf-style formatting,
// source location, thread names and a frame counter. Every subsystem logs into
// the same stream so a single HorizonEngine.log tells the whole story of a run:
//
//     [14:22:07.318] [ INFO] [Render  ] [Main    ] [f    120] Metal device: Apple M2 Pro
//     [14:22:07.402] [ WARN] [Asset   ] [Stream-0] [f    124] Mesh 'Rock' has no tangents — generating
//
// Usage:
//     #include <Diagnostics/Log.h>
//     HE_LOG_INFO (Render, "Backend '%s' initialised (%dx%d)", name, w, h);
//     HE_LOG_ERROR(Asset,  "Failed to load '%s': %s", path.c_str(), err.c_str());
//
// The macros evaluate their arguments ONLY when the category+level passes the
// filter, so leaving verbose Trace logging in hot paths costs one predictable
// branch. For per-frame code that can still fire thousands of times, use the
// throttling variants (HE_LOG_ONCE / HE_LOG_EVERY_N / HE_LOG_THROTTLE) instead
// of removing the log.
//
// Thread-safety: every entry point is safe to call from any thread. Records are
// serialised by a single mutex, so lines never interleave.
// ─────────────────────────────────────────────────────────────────────────────

namespace HE::Log
{
	using Level = HE::LogLevel;

	// ── Categories ───────────────────────────────────────────────────────────
	// A fixed enum rather than registered globals: filtering is a single array
	// lookup, and the set is shared across every DLL without exported data.
	// Add new entries before Count and extend kCategoryNames in Log.cpp.
	enum class Cat : uint8_t
	{
		Core,          // application lifecycle, generic engine plumbing
		Config,        // config.json, project settings, command line
		Platform,      // OS-level: paths, dynamic libraries, process
		Window,        // window/display/surface
		Input,         // keyboard/mouse/gamepad, input assets & mappings
		Job,           // thread pool / parallel_for
		Memory,        // allocations, GPU memory, budgets
		Asset,         // HAsset load/save, importers
		Content,       // content browser tree, project content scanning
		Pak,           // .hpak packaging, streaming, mounts
		Export,        // packaged-build export pipeline
		Render,        // renderer frontend: RenderWorld, extraction, passes
		RHI,           // backend device/swapchain/resources (GL/Metal/D3D/VK)
		Shader,        // shader compilation, pipeline state, caches
		Material,      // material assets + material graph
		Scene,         // scene load/save/lifecycle
		World,         // ECS world, entities, components
		Serialize,     // scene/asset (de)serialisation details
		Physics,       // Jolt world, bodies, character controller
		Collision,     // collision queries, callbacks, raycasts
		Animation,     // skeletal animation, blending, state machines
		Nav,           // navmesh build + agents
		Audio,         // audio engine, sources, listeners
		Particle,      // particle systems + graphs
		Terrain,       // landscape, chunks, sculpting, painting
		Foliage,       // foliage scattering & instancing
		LOD,           // level of detail selection
		Weather,       // weather + environment simulation
		UI,            // in-game UI system & input routing
		Widget,        // UI widget assets / WidgetManager
		Script,        // scripting host, dispatch, generic script errors
		Lua,           // Lua backend
		Python,        // Python backend
		HorizonCode,   // visual scripting graphs
		GameLogic,     // native C++ gameplay module (dlopen/hot reload)
		Net,           // HorizonNet
		Editor,        // editor application & panels
		Tool,          // offline tools (importers, packers, codegen)
		Profiler,      // profiler captures
		SourceControl, // git / LFS: probes, commands, provider calls
		Count
	};

	// Human-readable, fixed-width-friendly name ("Render", "Physics", …).
	HE_API const char* categoryName(Cat cat);
	// Case-insensitive lookup. Returns false when the name is unknown.
	HE_API bool categoryFromName(const char* name, Cat& out);
	// "INFO", "WARNING", … — also accepted by levelFromName (plus "verbose",
	// "all", "off" aliases).
	HE_API const char* levelName(Level level);
	HE_API bool levelFromName(const char* name, Level& out);

	// ── Filtering ────────────────────────────────────────────────────────────
	// A record is emitted when its level >= the category's verbosity AND
	// level >= the global floor. Level::Off silences a category completely.
	HE_API void  setVerbosity(Cat cat, Level level);
	HE_API void  setVerbosity(Level level);           // applies to all categories
	HE_API Level verbosity(Cat cat);
	HE_API bool  enabled(Cat cat, Level level);

	// Parse a verbosity spec: comma/semicolon separated "Category=Level" pairs,
	// where "*" or "all" sets every category. Unknown names are reported as
	// warnings and skipped. Example: "*=Warning,Render=Trace,Physics=Debug".
	// Applied at startup from the HE_LOG environment variable and from the
	// "logVerbosity" config.json entry.
	HE_API void configureFromString(const char* spec);
	// Current filter state serialised back into that same format (only entries
	// that differ from the default are listed).
	HE_API std::string verbositySpec();

	// ── Output channels ──────────────────────────────────────────────────────
	// Opens <path> for writing, rotating up to `keepBackups` previous runs to
	// "<stem>.1<ext>", "<stem>.2<ext>", … so the log of the run that crashed is
	// still around after the auto-restart. Returns false if the file could not
	// be opened (logging then continues to console + ring buffer only).
	HE_API bool openLogFile(const std::string& path, int keepBackups = 3);
	HE_API void closeLogFile();
	HE_API std::string logFilePath();
	HE_API void flush();

	// Mirror to stdout/stderr (Warning and above go to stderr). Enabled by
	// default; ANSI colours are used when the stream is a terminal.
	HE_API void setConsoleEnabled(bool enabled);
	HE_API bool consoleEnabled();
	// Append " (file.cpp:123)" to file/console records. Default: on for
	// Warning and above only — see setSourceLocationLevel.
	HE_API void setSourceLocationLevel(Level minLevel);

	// ── Per-record context ───────────────────────────────────────────────────
	// Frame number stamped onto every record; the game loop bumps it once per
	// frame so a log line can be tied to a specific frame in a profiler capture.
	HE_API void     setFrameNumber(uint64_t frame);
	HE_API uint64_t frameNumber();
	// Names the calling thread for the log ("Main", "Stream-0", …). Truncated
	// to 15 characters. Threads without a name show their numeric id.
	HE_API void        setThreadName(const char* name);
	HE_API const char* threadName();

	// ── Records + sinks ──────────────────────────────────────────────────────
	struct Record
	{
		Level       level;
		Cat         category;
		const char* message;   // formatted, NUL-terminated, never null
		const char* file;      // may be null
		int         line;      // 0 when unknown
		const char* function;  // may be null
		const char* thread;    // never null
		uint64_t    frame;
		int64_t     unixMillis;
	};

	// Additional consumers (editor console, play-session capture, telemetry).
	// Sinks are called on the logging thread with the log mutex held — keep
	// them short and never log from inside one. addSink returns a handle for
	// removeSink; 0 means "not installed".
	using SinkFn = void (*)(const Record& record, void* user);
	HE_API int  addSink(SinkFn sink, void* user);
	HE_API void removeSink(int handle);

	// ── In-memory ring buffer ────────────────────────────────────────────────
	// The last N fully formatted lines, kept in a fixed, preallocated buffer so
	// the crash handler can append recent history to the crash report without
	// allocating. Capacity is fixed at build time (kRingCapacity).
	inline constexpr int kRingCapacity = 512;
	inline constexpr int kRingLineSize = 512;

	// Calls fn for each retained line, oldest first. Used by the crash handler,
	// so it never allocates and never blocks: if the log mutex is held it reads
	// anyway (a torn line in a crash report beats a deadlock).
	HE_API void forEachRecent(void (*fn)(const char* line, void* user), void* user);
	// Convenience copy for in-process consumers (editor panels, tests).
	HE_API std::vector<std::string> recentLines(int maxLines = 0);

	// ── Startup diagnostics ──────────────────────────────────────────────────
	// Writes the header block every log should start with: engine build, build
	// configuration, compiler, OS, CPU/RAM, process id, executable path,
	// working directory and the command line. Called once from Application.
	HE_API void logStartupBanner(const char* appName, int argc, char** argv);
	// The machine-identifying half of that banner as "Key      : value" lines
	// (newline separated, no trailing newline): build configuration, compiler,
	// OS, CPU and RAM. Same facts, minus anything run-specific — the editor's
	// Report-Issue dialog pastes this into a bug report, where "which machine"
	// is the first question anyone asks.
	HE_API std::string systemInfoBlock();
	// Totals for the run ("12 warnings, 2 errors") plus wall time. Called at
	// shutdown; also available to the editor for the post-PIE report.
	HE_API void logShutdownSummary();
	HE_API uint64_t messageCount(Level level);

	// ── Emission ─────────────────────────────────────────────────────────────
	// Prefer the macros below — they skip formatting entirely when filtered out.
	HE_API void writeV(Cat cat, Level level, const char* file, int line,
	                   const char* function, const char* fmt, va_list args);

#if defined(__GNUC__) || defined(__clang__)
	HE_API void write(Cat cat, Level level, const char* file, int line,
	                  const char* function, const char* fmt, ...)
		__attribute__((format(printf, 6, 7)));
#else
	HE_API void write(Cat cat, Level level, const char* file, int line,
	                  const char* function, const char* fmt, ...);
#endif

	// ── Scope timing ─────────────────────────────────────────────────────────
	// Logs "<label> …" on entry and "<label> done (12.4 ms)" on exit. With a
	// non-zero thresholdMs nothing is logged unless the scope overruns, which
	// makes it safe to sprinkle over per-frame systems: silent when healthy,
	// loud exactly when something stalls.
	class HE_API ScopeTimer
	{
	public:
		ScopeTimer(Cat cat, Level level, double thresholdMs, const char* file,
		           int line, const char* function, const char* label);
		~ScopeTimer();
		ScopeTimer(const ScopeTimer&)            = delete;
		ScopeTimer& operator=(const ScopeTimer&) = delete;

		double elapsedMs() const;

	private:
		Cat         m_cat;
		Level       m_level;
		double      m_thresholdMs;
		const char* m_file;
		int         m_line;
		const char* m_function;
		char        m_label[128];
		int64_t     m_startNanos;
	};
} // namespace HE::Log

// ─── Macros ──────────────────────────────────────────────────────────────────

#define HE_LOG_FILE_ __FILE__
#define HE_LOG_FUNC_ __func__

// Core macro. `cat` is an unqualified HE::Log::Cat enumerator (Render, Physics…),
// `level` an unqualified HE::LogLevel enumerator (Info, Warning…).
#define HE_LOG(cat, level, ...)                                                  \
	do {                                                                         \
		if (::HE::Log::enabled(::HE::Log::Cat::cat, ::HE::LogLevel::level))      \
			::HE::Log::write(::HE::Log::Cat::cat, ::HE::LogLevel::level,         \
			                 HE_LOG_FILE_, __LINE__, HE_LOG_FUNC_, __VA_ARGS__); \
	} while (0)

#define HE_LOG_TRACE(cat, ...) HE_LOG(cat, Trace,    __VA_ARGS__)
#define HE_LOG_DEBUG(cat, ...) HE_LOG(cat, Debug,    __VA_ARGS__)
#define HE_LOG_INFO(cat, ...)  HE_LOG(cat, Info,     __VA_ARGS__)
#define HE_LOG_WARN(cat, ...)  HE_LOG(cat, Warning,  __VA_ARGS__)
#define HE_LOG_ERROR(cat, ...) HE_LOG(cat, Error,    __VA_ARGS__)
#define HE_LOG_CRIT(cat, ...)  HE_LOG(cat, Critical, __VA_ARGS__)

// Fires at most once per call site for the lifetime of the process. The go-to
// for "this asset is broken" messages inside per-frame loops.
#define HE_LOG_ONCE(cat, level, ...)                                             \
	do {                                                                         \
		static ::std::atomic<bool> heLogOnceFired_{false};                       \
		if (!heLogOnceFired_.exchange(true, ::std::memory_order_relaxed))        \
			HE_LOG(cat, level, __VA_ARGS__);                                     \
	} while (0)

// Fires on the 1st, (n+1)th, (2n+1)th … hit and appends the hit count.
#define HE_LOG_EVERY_N(cat, level, n, ...)                                       \
	do {                                                                         \
		static ::std::atomic<unsigned long long> heLogCounter_{0};               \
		const unsigned long long heLogHit_ =                                     \
			heLogCounter_.fetch_add(1, ::std::memory_order_relaxed);             \
		if ((n) <= 1 || heLogHit_ % (unsigned long long)(n) == 0)                \
			HE_LOG(cat, level, __VA_ARGS__);                                     \
	} while (0)

// Fires at most once every `seconds` per call site.
#define HE_LOG_THROTTLE(cat, level, seconds, ...)                                \
	do {                                                                         \
		static ::std::atomic<int64_t> heLogNextMs_{0};                           \
		const int64_t heNow_ = ::HE::Log::detail::monotonicMillis();             \
		int64_t heNext_ = heLogNextMs_.load(::std::memory_order_relaxed);        \
		if (heNow_ >= heNext_ &&                                                 \
		    heLogNextMs_.compare_exchange_strong(                                \
		        heNext_, heNow_ + (int64_t)((seconds) * 1000.0),                 \
		        ::std::memory_order_relaxed))                                    \
			HE_LOG(cat, level, __VA_ARGS__);                                     \
	} while (0)

// Logs and evaluates to false when `cond` is false; otherwise true. Intended for
// `if (!HE_CHECK(ptr, Asset, "…")) return;` guard clauses that previously failed
// silently. Spelled out as an expression (not via HE_LOG_*, which are `do{}while`
// statements) so it can be used inside an `if` condition.
#define HE_LOG_EXPR_(cat, level, ...)                                            \
	(::HE::Log::enabled(::HE::Log::Cat::cat, ::HE::LogLevel::level)               \
		 ? (::HE::Log::write(::HE::Log::Cat::cat, ::HE::LogLevel::level,          \
		                     HE_LOG_FILE_, __LINE__, HE_LOG_FUNC_, __VA_ARGS__),  \
		    false)                                                                \
		 : false)

#define HE_CHECK(cond, cat, ...)                                                 \
	((cond) ? true : HE_LOG_EXPR_(cat, Error, __VA_ARGS__))

// Same, but only warns.
#define HE_CHECK_WARN(cond, cat, ...)                                            \
	((cond) ? true : HE_LOG_EXPR_(cat, Warning, __VA_ARGS__))

#define HE_LOG_CONCAT_(a, b) a##b
#define HE_LOG_CONCAT(a, b)  HE_LOG_CONCAT_(a, b)

// Logs entry + exit-with-duration at Debug level.
#define HE_LOG_SCOPE(cat, label)                                                 \
	::HE::Log::ScopeTimer HE_LOG_CONCAT(heScope_, __LINE__)(                     \
		::HE::Log::Cat::cat, ::HE::LogLevel::Debug, 0.0, HE_LOG_FILE_,           \
		__LINE__, HE_LOG_FUNC_, label)

// Silent unless the scope takes longer than `ms`, then logs a warning.
#define HE_LOG_SLOW_SCOPE(cat, ms, label)                                        \
	::HE::Log::ScopeTimer HE_LOG_CONCAT(heScope_, __LINE__)(                     \
		::HE::Log::Cat::cat, ::HE::LogLevel::Warning, (double)(ms),              \
		HE_LOG_FILE_, __LINE__, HE_LOG_FUNC_, label)

namespace HE::Log::detail
{
	// Steady-clock milliseconds; exposed for HE_LOG_THROTTLE.
	HE_API int64_t monotonicMillis();
}
