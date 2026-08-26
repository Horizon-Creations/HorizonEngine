#include "doctest.h"
#include "TestFsUtil.h"

#include <Diagnostics/Log.h>
#include <Diagnostics/Logger.h>
#include <Scripting/ScriptTypes.h>              // the script log tag under test
#include <HorizonCode/HorizonCodeGenSupport.h>  // hc::print — generated HorizonCode
#include <HorizonScene/ScriptApi.h>             // ScriptApi::log — Lua / Python / api registry

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Captures every record the log emits while it is installed. Sinks are called
// with the log mutex held, so the capture itself must not log.
struct Capture
{
	struct Entry
	{
		HE::Log::Level level;
		HE::Log::Cat   category;
		std::string    message;
		std::string    thread;
		uint64_t       frame;
		int            line;
		bool           hasFile;
	};

	Capture() { m_handle = HE::Log::addSink(&Capture::onRecord, this); }
	~Capture() { HE::Log::removeSink(m_handle); }

	Capture(const Capture&)            = delete;
	Capture& operator=(const Capture&) = delete;

	std::vector<Entry> entries() const
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_entries;
	}

	size_t count() const
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_entries.size();
	}

	bool contains(const std::string& needle) const
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		for (const Entry& e : m_entries)
			if (e.message.find(needle) != std::string::npos) return true;
		return false;
	}

private:
	static void onRecord(const HE::Log::Record& rec, void* user)
	{
		Capture* self = static_cast<Capture*>(user);
		std::lock_guard<std::mutex> lk(self->m_mutex);
		self->m_entries.push_back(Entry{
			rec.level, rec.category, rec.message ? rec.message : "",
			rec.thread ? rec.thread : "", rec.frame, rec.line, rec.file != nullptr });
	}

	mutable std::mutex m_mutex;
	std::vector<Entry> m_entries;
	int                m_handle = 0;
};

// Restores every category's verbosity when the test ends, so one test's filter
// experiments cannot silence another's.
struct VerbosityGuard
{
	VerbosityGuard()
	{
		for (int i = 0; i < static_cast<int>(HE::Log::Cat::Count); ++i)
			m_saved.push_back(HE::Log::verbosity(static_cast<HE::Log::Cat>(i)));
	}
	~VerbosityGuard()
	{
		for (int i = 0; i < static_cast<int>(HE::Log::Cat::Count); ++i)
			HE::Log::setVerbosity(static_cast<HE::Log::Cat>(i), m_saved[static_cast<size_t>(i)]);
	}
	std::vector<HE::Log::Level> m_saved;
};

// The script log tag is process-global and every test runs in the same process,
// so a tag left behind here would prefix (and break) another test's log
// assertions. Same reason VerbosityGuard exists.
struct ScriptLogTagGuard
{
	ScriptLogTagGuard() : m_saved(HE::scriptLogTag()) {}
	~ScriptLogTagGuard() { HE::setScriptLogTag(m_saved); }
	std::string m_saved;
};
} // namespace

TEST_CASE("Log category and level names round-trip")
{
	for (int i = 0; i < static_cast<int>(HE::Log::Cat::Count); ++i)
	{
		const auto cat  = static_cast<HE::Log::Cat>(i);
		const char* name = HE::Log::categoryName(cat);
		REQUIRE(name != nullptr);
		CHECK(std::string(name) != "?");

		HE::Log::Cat parsed{};
		CHECK(HE::Log::categoryFromName(name, parsed));
		CHECK(parsed == cat);
	}

	// Case-insensitive, as a verbosity spec is typed by hand.
	HE::Log::Cat cat{};
	CHECK(HE::Log::categoryFromName("render", cat));
	CHECK(cat == HE::Log::Cat::Render);
	CHECK(HE::Log::categoryFromName("PHYSICS", cat));
	CHECK(cat == HE::Log::Cat::Physics);
	CHECK_FALSE(HE::Log::categoryFromName("NotACategory", cat));

	HE::Log::Level lv{};
	CHECK(HE::Log::levelFromName("Warning", lv));
	CHECK(lv == HE::Log::Level::Warning);
	CHECK(HE::Log::levelFromName("warn", lv));
	CHECK(lv == HE::Log::Level::Warning);
	CHECK(HE::Log::levelFromName("verbose", lv));
	CHECK(lv == HE::Log::Level::Trace);
	CHECK(HE::Log::levelFromName("off", lv));
	CHECK(lv == HE::Log::Level::Off);
	CHECK_FALSE(HE::Log::levelFromName("loud", lv));
}

TEST_CASE("Per-category verbosity gates records")
{
	VerbosityGuard guard;
	Capture capture;

	HE::Log::setVerbosity(HE::Log::Cat::Physics, HE::Log::Level::Warning);
	HE::Log::setVerbosity(HE::Log::Cat::Audio,   HE::Log::Level::Trace);

	CHECK_FALSE(HE::Log::enabled(HE::Log::Cat::Physics, HE::Log::Level::Info));
	CHECK(HE::Log::enabled(HE::Log::Cat::Physics, HE::Log::Level::Error));
	CHECK(HE::Log::enabled(HE::Log::Cat::Audio,   HE::Log::Level::Trace));

	HE_LOG_INFO(Physics, "%s", "physics-info-should-be-dropped");
	HE_LOG_ERROR(Physics, "%s", "physics-error-should-pass");
	HE_LOG_TRACE(Audio,  "%s", "audio-trace-should-pass");

	CHECK_FALSE(capture.contains("physics-info-should-be-dropped"));
	CHECK(capture.contains("physics-error-should-pass"));
	CHECK(capture.contains("audio-trace-should-pass"));

	// Off silences a category completely, including Critical.
	HE::Log::setVerbosity(HE::Log::Cat::Physics, HE::Log::Level::Off);
	CHECK_FALSE(HE::Log::enabled(HE::Log::Cat::Physics, HE::Log::Level::Critical));
	HE_LOG_CRIT(Physics, "%s", "physics-crit-should-be-dropped");
	CHECK_FALSE(capture.contains("physics-crit-should-be-dropped"));
}

TEST_CASE("configureFromString parses a verbosity spec")
{
	VerbosityGuard guard;

	HE::Log::configureFromString("*=Warning, Render=Trace ;Physics=Debug");
	CHECK(HE::Log::verbosity(HE::Log::Cat::Render)  == HE::Log::Level::Trace);
	CHECK(HE::Log::verbosity(HE::Log::Cat::Physics) == HE::Log::Level::Debug);
	CHECK(HE::Log::verbosity(HE::Log::Cat::Audio)   == HE::Log::Level::Warning);

	// A bare level applies to everything.
	HE::Log::configureFromString("Error");
	CHECK(HE::Log::verbosity(HE::Log::Cat::Render) == HE::Log::Level::Error);
	CHECK(HE::Log::verbosity(HE::Log::Cat::Audio)  == HE::Log::Level::Error);

	// Unknown names are skipped, not fatal, and leave the rest applied.
	HE::Log::configureFromString("Nonsense=Trace,Audio=Info");
	CHECK(HE::Log::verbosity(HE::Log::Cat::Audio) == HE::Log::Level::Info);

	// The spec round-trips through verbositySpec().
	HE::Log::setVerbosity(HE::Log::Level::Info);          // back to the default
	HE::Log::setVerbosity(HE::Log::Cat::Nav, HE::Log::Level::Trace);
	const std::string spec = HE::Log::verbositySpec();
	CHECK(spec.find("Nav=TRACE") != std::string::npos);
	HE::Log::setVerbosity(HE::Log::Level::Info);
	HE::Log::configureFromString(spec.c_str());
	CHECK(HE::Log::verbosity(HE::Log::Cat::Nav) == HE::Log::Level::Trace);
}

TEST_CASE("Records carry category, level, source location and thread")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	HE::Log::setFrameNumber(4242);
	HE_LOG_WARN(Tool, "answer=%d name=%s", 42, "horizon");

	const auto entries = capture.entries();
	REQUIRE(entries.size() >= 1);
	const auto& e = entries.back();
	CHECK(e.category == HE::Log::Cat::Tool);
	CHECK(e.level    == HE::Log::Level::Warning);
	CHECK(e.message  == "answer=42 name=horizon");
	CHECK(e.frame    == 4242);
	CHECK(e.hasFile);
	CHECK(e.line > 0);
	CHECK_FALSE(e.thread.empty());

	HE::Log::setFrameNumber(0);
}

TEST_CASE("Message formatting handles literal percent signs and truncation")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	// Legacy call sites hand a prebuilt string to "%s"; a '%' inside it must
	// stay a literal rather than being read as a conversion.
	HE_LOG_INFO(Tool, "%s", "progress is 50% done");
	CHECK(capture.contains("progress is 50% done"));

	// An over-long message is truncated, never overruns, and says so.
	const std::string huge(4096, 'x');
	HE_LOG_INFO(Tool, "%s", huge.c_str());
	const auto entries = capture.entries();
	REQUIRE(entries.size() >= 2);
	CHECK(entries.back().message.size() < huge.size());
	CHECK(entries.back().message.find("...") != std::string::npos);
}

TEST_CASE("Consecutive duplicate records are collapsed")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	// A per-frame error must not be able to flood the log: identical consecutive
	// records collapse into a single "repeated N times" note.
	for (int i = 0; i < 50; ++i)
		HE_LOG_ERROR(Tool, "%s", "identical-spam");
	HE_LOG_ERROR(Tool, "%s", "something-else");

	size_t spamRecords = 0;
	for (const auto& e : capture.entries())
		if (e.message == "identical-spam") ++spamRecords;

	CHECK(spamRecords == 1);
	CHECK(capture.contains("repeated 49 more times"));
	CHECK(capture.contains("something-else"));
}

TEST_CASE("HE_LOG_ONCE and HE_LOG_EVERY_N throttle per call site")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	for (int i = 0; i < 20; ++i)
		HE_LOG_ONCE(Tool, Warning, "once-%d", i);

	// Distinct payloads, so duplicate collapsing cannot be what limits this.
	size_t onceCount = 0;
	for (const auto& e : capture.entries())
		if (e.message.rfind("once-", 0) == 0) ++onceCount;
	CHECK(onceCount == 1);

	for (int i = 0; i < 20; ++i)
		HE_LOG_EVERY_N(Tool, Warning, 5, "every-%d", i);

	size_t everyCount = 0;
	for (const auto& e : capture.entries())
		if (e.message.rfind("every-", 0) == 0) ++everyCount;
	CHECK(everyCount == 4);   // hits 0, 5, 10, 15
}

TEST_CASE("HE_CHECK reports and yields the condition")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	const int* nullPtr = nullptr;
	const int  value   = 7;

	CHECK(HE_CHECK(&value != nullptr, Tool, "%s", "must-not-be-logged"));
	CHECK_FALSE(HE_CHECK(nullPtr != nullptr, Tool, "guard tripped for %s", "nullPtr"));

	CHECK_FALSE(capture.contains("must-not-be-logged"));
	CHECK(capture.contains("guard tripped for nullPtr"));
}

TEST_CASE("Ring buffer retains recent lines for the crash report")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);

	HE_LOG_INFO(Tool, "%s", "ring-buffer-marker-alpha");
	HE_LOG_INFO(Tool, "%s", "ring-buffer-marker-beta");

	const auto lines = HE::Log::recentLines();
	REQUIRE(lines.size() >= 2);

	bool sawAlpha = false, sawBeta = false;
	for (const std::string& l : lines)
	{
		if (l.find("ring-buffer-marker-alpha") != std::string::npos) sawAlpha = true;
		if (l.find("ring-buffer-marker-beta")  != std::string::npos) sawBeta  = true;
	}
	CHECK(sawAlpha);
	CHECK(sawBeta);

	// The buffer is bounded — it can never grow without limit in a long session.
	for (int i = 0; i < HE::Log::kRingCapacity + 200; ++i)
		HE_LOG_INFO(Tool, "ring-fill-%d", i);
	CHECK(HE::Log::recentLines().size() <= static_cast<size_t>(HE::Log::kRingCapacity));

	// The formatted line carries the category and level columns.
	const auto tail = HE::Log::recentLines(1);
	REQUIRE(tail.size() == 1);
	CHECK(tail[0].find("Tool") != std::string::npos);
	CHECK(tail[0].find("INFO") != std::string::npos);
}

TEST_CASE("recentProblemLines keeps only what went wrong, newest first")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);

	// A realistic mix: chatter around the two records that matter. A report with
	// room for a handful of lines must spend them on these, not on the chatter.
	HE_LOG_INFO (Tool, "%s", "problem-filter-noise-1");
	HE_LOG_WARN (Tool, "%s", "problem-filter-warning");
	HE_LOG_INFO (Tool, "%s", "problem-filter-noise-2");
	HE_LOG_ERROR(Tool, "%s", "problem-filter-error");
	HE_LOG_DEBUG(Tool, "%s", "problem-filter-noise-3");

	const auto problems = HE::Log::recentProblemLines(0, HE::Log::Level::Warning);
	REQUIRE(problems.size() >= 2);
	for (const std::string& l : problems)
		CHECK(l.find("problem-filter-noise") == std::string::npos);

	// Chronological order, oldest first — same contract as recentLines.
	const auto findLine = [&](const char* needle) {
		for (std::size_t i = 0; i < problems.size(); ++i)
			if (problems[i].find(needle) != std::string::npos) return static_cast<int>(i);
		return -1;
	};
	const int atWarning = findLine("problem-filter-warning");
	const int atError   = findLine("problem-filter-error");
	REQUIRE(atWarning >= 0);
	REQUIRE(atError   >= 0);
	CHECK(atWarning < atError);

	// A cap keeps the NEWEST matches: what happened just before the report is
	// worth more than what happened at startup.
	const auto capped = HE::Log::recentProblemLines(1, HE::Log::Level::Warning);
	REQUIRE(capped.size() == 1);
	CHECK(capped[0].find("problem-filter-error") != std::string::npos);

	// Raising the floor drops the warning too.
	const auto errorsOnly = HE::Log::recentProblemLines(0, HE::Log::Level::Error);
	for (const std::string& l : errorsOnly)
		CHECK(l.find("problem-filter-warning") == std::string::npos);
	CHECK(findLine("problem-filter-error") >= 0);

	// Severity comes from the ring, not from matching "[ WARN]" in the text —
	// a line that merely TALKS about a warning is still an Info record.
	HE_LOG_INFO(Tool, "%s", "problem-filter-mentions [ WARN] in its text");
	const auto after = HE::Log::recentProblemLines(0, HE::Log::Level::Warning);
	for (const std::string& l : after)
		CHECK(l.find("problem-filter-mentions") == std::string::npos);
}

TEST_CASE("Log file is written and previous runs are rotated")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);

	const fs::path dir = fs::temp_directory_path() / "he_log_rotation_test";
	he_test::removeAllQuiet(dir);
	std::error_code ec;
	fs::create_directories(dir, ec);
	const fs::path logPath = dir / "HorizonEngine.log";

	// Preserve whatever the process was logging to, and restore it afterwards —
	// the log file is global state shared with every other test.
	const std::string previous = HE::Log::logFilePath();

	REQUIRE(HE::Log::openLogFile(logPath.string(), /*keepBackups=*/2));
	CHECK(HE::Log::logFilePath() == logPath.string());
	HE_LOG_INFO(Tool, "%s", "first-run-marker");
	HE::Log::flush();

	// Second open rotates the first run's file to HorizonEngine.1.log.
	REQUIRE(HE::Log::openLogFile(logPath.string(), /*keepBackups=*/2));
	HE_LOG_INFO(Tool, "%s", "second-run-marker");
	HE::Log::closeLogFile();

	const fs::path rotated = dir / "HorizonEngine.1.log";
	REQUIRE(fs::exists(logPath));
	REQUIRE(fs::exists(rotated));

	const auto readAll = [](const fs::path& p) {
		std::ifstream in(p);
		return std::string((std::istreambuf_iterator<char>(in)),
		                   std::istreambuf_iterator<char>());
	};
	CHECK(readAll(logPath).find("second-run-marker") != std::string::npos);
	CHECK(readAll(rotated).find("first-run-marker")  != std::string::npos);

	if (!previous.empty()) HE::Log::openLogFile(previous, /*keepBackups=*/0);
	he_test::removeAllQuiet(dir);
}

TEST_CASE("Legacy Logger forwards into the new log")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Level::Trace);
	Capture capture;

	Logger::Log(Logger::LogLevel::Warning, "legacy-message");
	CHECK(capture.contains("legacy-message"));

	Logger::LogTo(HE::Log::Cat::Nav, Logger::LogLevel::Error, "legacy-categorised");
	bool sawNav = false;
	for (const auto& e : capture.entries())
		if (e.message == "legacy-categorised" && e.category == HE::Log::Cat::Nav) sawNav = true;
	CHECK(sawNav);
}

TEST_CASE("Legacy Logger sink installs, receives and uninstalls")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Level::Trace);

	static std::atomic<int> received{0};
	received = 0;
	Logger::setSink([](HE::LogLevel, const char*, void*) { ++received; }, nullptr);

	Logger::Log(Logger::LogLevel::Info, "sink-target-one");
	Logger::Log(Logger::LogLevel::Info, "sink-target-two");
	CHECK(received.load() >= 2);

	const int before = received.load();
	Logger::setSink(nullptr, nullptr);
	Logger::Log(Logger::LogLevel::Info, "sink-target-after-uninstall");
	CHECK(received.load() == before);
}

TEST_CASE("Logging is safe from many threads at once")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Job, HE::Log::Level::Trace);
	Capture capture;

	constexpr int kThreads = 8;
	constexpr int kPerThread = 100;

	std::vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([t] {
			char name[16];
			std::snprintf(name, sizeof(name), "T%d", t);
			HE::Log::setThreadName(name);
			for (int i = 0; i < kPerThread; ++i)
				HE_LOG_INFO(Job, "thread %d record %d", t, i);
		});
	}
	for (auto& th : threads) th.join();

	// Every record arrives exactly once and carries its own thread's name — no
	// interleaving, no loss, no duplicate suppression (each payload is unique).
	const auto entries = capture.entries();
	CHECK(entries.size() == kThreads * kPerThread);

	std::vector<int> perThread(kThreads, 0);
	for (const auto& e : entries)
	{
		int t = -1, i = -1;
		if (std::sscanf(e.message.c_str(), "thread %d record %d", &t, &i) == 2 &&
		    t >= 0 && t < kThreads)
		{
			++perThread[static_cast<size_t>(t)];
			CHECK(e.thread == ("T" + std::to_string(t)));
		}
	}
	for (int t = 0; t < kThreads; ++t)
		CHECK(perThread[static_cast<size_t>(t)] == kPerThread);
}

TEST_CASE("Scope timer reports only when it overruns its budget")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);
	Capture capture;

	{
		// Generous budget — a trivial scope must stay silent.
		HE_LOG_SLOW_SCOPE(Tool, 60000.0, "fast-scope");
	}
	CHECK_FALSE(capture.contains("fast-scope"));

	{
		// Zero budget — always reports, so any scope overruns it.
		HE_LOG_SLOW_SCOPE(Tool, 0.0001, "slow-scope");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(capture.contains("slow-scope"));

	{
		// Threshold-free scope logs entry and exit with a duration.
		HE_LOG_SCOPE(Tool, "traced-scope");
	}
	CHECK(capture.contains("traced-scope ..."));
	CHECK(capture.contains("traced-scope done"));
}

TEST_CASE("Message counters track warnings and errors for the session summary")
{
	VerbosityGuard guard;
	HE::Log::setVerbosity(HE::Log::Cat::Tool, HE::Log::Level::Trace);

	const uint64_t warnBefore = HE::Log::messageCount(HE::Log::Level::Warning);
	const uint64_t errBefore  = HE::Log::messageCount(HE::Log::Level::Error);

	HE_LOG_WARN(Tool, "%s", "counter-warning-a");
	HE_LOG_WARN(Tool, "%s", "counter-warning-b");
	HE_LOG_ERROR(Tool, "%s", "counter-error-a");

	CHECK(HE::Log::messageCount(HE::Log::Level::Warning) == warnBefore + 2);
	CHECK(HE::Log::messageCount(HE::Log::Level::Error)   == errBefore + 1);
}

TEST_CASE("Script log lines carry the project's language tag")
{
	ScriptLogTagGuard tagGuard;
	VerbosityGuard    guard;
	HE::Log::setVerbosity(HE::Log::Cat::HorizonCode, HE::Log::Level::Trace);
	HE::Log::setVerbosity(HE::Log::Cat::Script,      HE::Log::Level::Trace);

	// No tag set → the message passes through untouched. This is what an
	// application that never sets one (the packaged game, the tools) still logs.
	HE::setScriptLogTag("");
	CHECK(HE::scriptLogLine("untagged-line") == "untagged-line");

	HE::setScriptLogTag("[Lua] ");
	CHECK(HE::scriptLogTag() == "[Lua] ");
	CHECK(HE::scriptLogLine("tagged-line") == "[Lua] tagged-line");

	// The point of the shared helper: the interpreter's Print (via generated
	// C++, hc::print) and the Lua/Python/api path (ScriptApi::log) must emit the
	// SAME prefix. They once did not — generated code hard-coded "[Widget] "
	// after the interpreter had dropped it.
	{
		Capture capture;
		hc::print("hc-print-line");
		ScriptApi::log("script-api-line");

		CHECK(capture.contains("[Lua] hc-print-line"));
		CHECK(capture.contains("[Lua] script-api-line"));
		CHECK_FALSE(capture.contains("[Widget] "));
	}

	// A different project language re-tags both the same way.
	HE::setScriptLogTag("[HC] ");
	{
		Capture capture;
		hc::print("hc-print-again");
		ScriptApi::log("script-api-again");

		CHECK(capture.contains("[HC] hc-print-again"));
		CHECK(capture.contains("[HC] script-api-again"));
	}
}
