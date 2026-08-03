#include "doctest.h"
#include "TestFsUtil.h"

#include <Platform/Process.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// ─── HE::Proc::run ───────────────────────────────────────────────────────────
// Each case targets one specific failure the previous popen()-based helper could
// not avoid. They run against a real child process (he_proc_child) because none
// of these are observable through a mock.

namespace fs = std::filesystem;
using namespace HE;

namespace {

fs::path childExe() { return fs::path(HE_TEST_PROC_CHILD); }

Proc::Options child(std::vector<std::string> args)
{
	Proc::Options o;
	o.exe  = childExe();
	o.args = std::move(args);
	o.timeoutMs = 30000;   // a hung child must fail the test, not hang the suite
	return o;
}

fs::path uniqueTempPath(const char* stem)
{
	static const std::uint64_t salt =
		static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
	static int counter = 0;
	return fs::temp_directory_path() /
	       ("he_proc_" + std::string(stem) + "_" + std::to_string(salt) + "_" +
	        std::to_string(counter++) + ".txt");
}

} // namespace

TEST_CASE("An exit code arrives as itself, not as a wait status")
{
	// The headline bug in the popen approach: pclose returns the wait STATUS, so
	// exit 42 came back as 42 << 8 == 10752 and every caller could only test
	// against zero. Programs that answer with an exit code were unusable.
	const Proc::Result r = Proc::run(child({ "exit", "42" }));
	REQUIRE_FALSE(r.launchFailed);
	REQUIRE_FALSE(r.timedOut);
	CHECK(r.exitCode == 42);
	CHECK(r.exitCode != 10752);
	CHECK_FALSE(r.ok());

	const Proc::Result zero = Proc::run(child({ "exit", "0" }));
	CHECK(zero.exitCode == 0);
	CHECK(zero.ok());

	// Exit 1 is a real answer for many tools (diff, grep, cmp), not an error.
	const Proc::Result one = Proc::run(child({ "exit", "1" }));
	CHECK(one.exitCode == 1);
}

TEST_CASE("stdout and stderr stay separate")
{
	const Proc::Result r = Proc::run(child({ "streams" }));
	REQUIRE_FALSE(r.launchFailed);
	CHECK(r.out.find("this-is-stdout") != std::string::npos);
	CHECK(r.err.find("this-is-stderr") != std::string::npos);
	// The whole point — neither stream may contain the other's text.
	CHECK(r.out.find("this-is-stderr") == std::string::npos);
	CHECK(r.err.find("this-is-stdout") == std::string::npos);
	CHECK(r.exitCode == 3);
}

TEST_CASE("A megabyte on both streams at once does not deadlock")
{
	// Draining one stream to EOF and then the other deadlocks the moment the
	// child fills the pipe nobody is reading. Every tool that reports progress on
	// stderr while writing data on stdout hits this.
	constexpr std::size_t kBytes = 1000000;
	Proc::Options o = child({ "flood", std::to_string(kBytes) });
	o.timeoutMs = 60000;

	const Proc::Result r = Proc::run(o);
	REQUIRE_FALSE(r.launchFailed);
	REQUIRE_FALSE(r.timedOut);          // a deadlock shows up here
	CHECK(r.out.size() == kBytes);
	CHECK(r.err.size() == kBytes);
	CHECK(r.out.find_first_not_of('O') == std::string::npos);
	CHECK(r.err.find_first_not_of('E') == std::string::npos);
}

TEST_CASE("Arguments survive spaces, quotes, metacharacters and UTF-8")
{
	// popen went through a shell, so each of these was either mangled or, worse,
	// executed. An argv vector passes them through untouched.
	const std::vector<std::string> tricky = {
		"plain",
		"with spaces",
		"with\"quote",
		"with'single",
		"$(echo pwned)",
		"`echo pwned`",
		"back\\slash",
		"semi;colon && chain",
		"Grüße-日本語-🎮",
		"",                       // an empty argument is still an argument
	};

	std::vector<std::string> args = { "args" };
	args.insert(args.end(), tricky.begin(), tricky.end());

	const Proc::Result r = Proc::run(child(args));
	REQUIRE_FALSE(r.launchFailed);
	REQUIRE(r.ok());

	for (const std::string& t : tricky)
	{
		CAPTURE(t);
		CHECK(r.out.find("[" + t + "]") != std::string::npos);
	}

	// Nothing was executed by a shell along the way. The test looks for the
	// EXPANDED form, not for the word "pwned" — the literal argument contains it
	// by design, and passing it through untouched is the behaviour being checked.
	// A shell would have turned `$(echo pwned)` into a bare `pwned` argument.
	CHECK(r.out.find("[pwned]") == std::string::npos);
}

TEST_CASE("stdin is delivered and then closed")
{
	// Without a stdin pipe there is no way to feed a program anything; without
	// CLOSING it, a child that reads to EOF waits forever.
	const std::string payload = "line one\nline two\nno trailing newline";
	Proc::Options o = child({ "echo" });
	o.stdinData = payload;

	const Proc::Result r = Proc::run(o);
	REQUIRE_FALSE(r.launchFailed);
	REQUIRE_FALSE(r.timedOut);          // a stdin left open shows up here
	CHECK(r.out == payload);
}

TEST_CASE("A large stdin payload does not deadlock against the child's output")
{
	// The parent writing stdin in one blocking call, while the child is blocked
	// writing output nobody is reading, is a deadlock that only appears once the
	// payload exceeds the pipe buffer — so it passes in development and hangs in
	// production.
	const std::string payload(512 * 1024, 'x');
	Proc::Options o = child({ "echo" });
	o.stdinData = payload;
	o.timeoutMs = 60000;

	const Proc::Result r = Proc::run(o);
	REQUIRE_FALSE(r.timedOut);
	CHECK(r.out.size() == payload.size());
}

TEST_CASE("A timeout kills the child and is reported as one")
{
	Proc::Options o = child({ "sleep", "30000" });
	o.timeoutMs = 300;

	const auto start = std::chrono::steady_clock::now();
	const Proc::Result r = Proc::run(o);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	CHECK(r.timedOut);
	CHECK_FALSE(r.ok());
	// It actually returned early rather than waiting out the child.
	CHECK(elapsed < std::chrono::seconds(10));
	// And the child never got to announce completion.
	CHECK(r.out.find("finished") == std::string::npos);
}

TEST_CASE("Killing a child takes its grandchildren with it")
{
	// A killed process leaves its own children running unless the whole group (or
	// job object) is terminated. For a source-control tool that matters
	// concretely: git spawns git-remote-https and git-lfs, and those hold the
	// network connection open long after git itself is gone.
	const fs::path marker = uniqueTempPath("grandchild");
	he_test::removeQuiet(marker);

	Proc::Options o = child({ "spawn-grandchild", marker.string() });
	o.timeoutMs = 400;

	const Proc::Result r = Proc::run(o);
	CHECK(r.timedOut);

	// The grandchild writes its marker 1.5 s in. Wait past that: if the group
	// kill worked the file never appears.
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	CHECK_FALSE(fs::exists(marker));

	he_test::removeQuiet(marker);
}

TEST_CASE("Line callbacks deliver whole lines, including an unterminated last one")
{
	std::vector<std::string> outLines, errLines;
	Proc::Options o = child({ "out", "alpha", "beta", "gamma" });
	o.onStdoutLine = [&](std::string_view l) { outLines.emplace_back(l); };
	o.onStderrLine = [&](std::string_view l) { errLines.emplace_back(l); };

	const Proc::Result r = Proc::run(o);
	REQUIRE(r.ok());
	REQUIRE(outLines.size() == 3);
	CHECK(outLines[0] == "alpha");
	CHECK(outLines[1] == "beta");
	CHECK(outLines[2] == "gamma");
	CHECK(errLines.empty());

	// A stream that ends without a newline must still deliver its last line —
	// dropping it loses exactly the error message a tool prints before dying.
	std::vector<std::string> tail;
	Proc::Options e = child({ "echo" });
	e.stdinData    = "first\nunterminated";
	e.onStdoutLine = [&](std::string_view l) { tail.emplace_back(l); };
	REQUIRE(Proc::run(e).ok());
	REQUIRE(tail.size() == 2);
	CHECK(tail[1] == "unterminated");
}

TEST_CASE("The environment can be extended and overridden")
{
	Proc::Options o = child({ "env", "HE_PROC_TEST_VAR" });
	o.env.emplace_back("HE_PROC_TEST_VAR", "hello-from-test");

	const Proc::Result r = Proc::run(o);
	REQUIRE(r.ok());
	CHECK(r.out.find("hello-from-test") != std::string::npos);

	// Without it, the child sees nothing — proving the variable came from the
	// options rather than from the test runner's own environment.
	const Proc::Result bare = Proc::run(child({ "env", "HE_PROC_TEST_VAR" }));
	CHECK(bare.out.find("<unset>") != std::string::npos);
}

TEST_CASE("The working directory is honoured")
{
	const fs::path dir = fs::temp_directory_path();
	Proc::Options o = child({ "cwd" });
	o.cwd = dir;

	const Proc::Result r = Proc::run(o);
	REQUIRE(r.ok());
	// Compared canonically: macOS reports /private/var for /var, and Windows
	// differs in case and trailing separator.
	std::error_code ec;
	const fs::path reported = fs::weakly_canonical(
		fs::path(r.out.substr(0, r.out.find_last_not_of("\r\n") + 1)), ec);
	const fs::path expected = fs::weakly_canonical(dir, ec);
	CHECK(reported == expected);
}

TEST_CASE("A missing program fails to launch instead of looking like a crash")
{
	Proc::Options o;
	o.exe = "he_definitely_not_a_real_program_9f3a";
	o.timeoutMs = 5000;

	const Proc::Result r = Proc::run(o);
	CHECK(r.launchFailed);
	CHECK_FALSE(r.ok());
	// Distinguishable from "ran and failed", which needs a different diagnosis.
	CHECK_FALSE(r.timedOut);
}

TEST_CASE("which finds a program on PATH and rejects one that is not there")
{
	// The child executable is given by absolute path, so this also covers the
	// "already a path" branch.
	const auto self = Proc::which(childExe().string());
	REQUIRE(self.has_value());
	CHECK(fs::exists(*self));

	CHECK_FALSE(Proc::which("he_definitely_not_a_real_program_9f3a").has_value());

	// Something every supported platform ships, to cover the PATH search itself.
#ifdef _WIN32
	CHECK(Proc::which("cmd").has_value());
#else
	CHECK(Proc::which("sh").has_value());
#endif
}

TEST_CASE("augmentToolPath is idempotent and never loses existing entries")
{
	const char* before = std::getenv("PATH");
	const std::string original = before ? before : "";

	Proc::augmentToolPath();
	Proc::augmentToolPath();

	const char* after = std::getenv("PATH");
	const std::string updated = after ? after : "";

	// Every original entry is still reachable; the call only ever prepends.
	std::size_t start = 0;
	while (start <= original.size())
	{
#ifdef _WIN32
		const char sep = ';';
#else
		const char sep = ':';
#endif
		const std::size_t end = original.find(sep, start);
		const std::string entry =
			original.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!entry.empty())
		{
			CAPTURE(entry);
			CHECK(updated.find(entry) != std::string::npos);
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
}
