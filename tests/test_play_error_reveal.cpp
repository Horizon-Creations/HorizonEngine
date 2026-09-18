#include "doctest.h"
#include "PlayErrorReveal.h"

#include <vector>

// ── The first error of a play session opens the console ─────────────────────
// The console is closed by default, and until this decision existed nothing
// during a play session opened it: an error went to the bell (ambient), to the
// console's buffer (closed) and to the post-play report (after Stop). What is
// asserted here is the edge the editor acts on — once per session, for an
// Error or worse, never for a warning, and again for the next session.

using HE::Ed::PlayErrorReveal;

namespace
{
	PlayLogEntry entry(HE::LogLevel level, const char* msg)
	{
		PlayLogEntry e;
		e.level   = level;
		e.message = msg;
		return e;
	}
}

TEST_CASE("play error reveal: nothing happens outside a play session")
{
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	// A stale log from the previous session, read while NOT playing: the
	// report window's business, not the console's.
	log.push_back(entry(HE::LogLevel::Error, "old news"));
	for (int i = 0; i < 3; ++i) CHECK_FALSE(r.poll(false, log));
}

TEST_CASE("play error reveal: warnings do not open the console")
{
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Warning, "a texture is not power of two"));
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Warning, "another one"));
	CHECK_FALSE(r.poll(true, log));
}

TEST_CASE("play error reveal: the first error fires once, further errors do not")
{
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	// Play started, a few quiet frames.
	CHECK_FALSE(r.poll(true, log));
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Warning, "harmless"));
	CHECK_FALSE(r.poll(true, log));
	// The one: the frame the first error is in the log.
	log.push_back(entry(HE::LogLevel::Error, "Lua script instance 1 failed in onStart(): mover:3: boom"));
	CHECK(r.poll(true, log));
	// Same log next frame — nothing new, no second reveal (a console the user
	// closed again stays closed).
	CHECK_FALSE(r.poll(true, log));
	// More errors in the same session — still nothing: once per session.
	log.push_back(entry(HE::LogLevel::Error, "HorizonCode: null reference"));
	log.push_back(entry(HE::LogLevel::Critical, "worse"));
	CHECK_FALSE(r.poll(true, log));
	CHECK_FALSE(r.poll(true, log));
}

TEST_CASE("play error reveal: Critical counts as an error")
{
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Critical, "renderer lost the device"));
	CHECK(r.poll(true, log));
}

TEST_CASE("play error reveal: an error that is already in the log when play starts fires on the first frame")
{
	// appendPlayLog can run before the UI's first frame of the session (onStart
	// fails inside setPlayMode itself): the first poll of the session sees it.
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	CHECK_FALSE(r.poll(false, log));
	log.push_back(entry(HE::LogLevel::Error, "failed in onStart()"));
	CHECK(r.poll(true, log));
	CHECK_FALSE(r.poll(true, log));
}

TEST_CASE("play error reveal: stop and play again arms it for the next session")
{
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	log.push_back(entry(HE::LogLevel::Error, "first session"));
	CHECK(r.poll(true, log));
	CHECK_FALSE(r.poll(true, log));
	// Stop: the report window reads the log now; we stay quiet.
	CHECK_FALSE(r.poll(false, log));
	CHECK_FALSE(r.poll(false, log));
	// Play again: setPlayMode clears the log before isPlaying rises.
	log.clear();
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Warning, "quiet start"));
	CHECK_FALSE(r.poll(true, log));
	log.push_back(entry(HE::LogLevel::Error, "second session"));
	CHECK(r.poll(true, log));
	CHECK_FALSE(r.poll(true, log));
}

TEST_CASE("play error reveal: a log that shrank underneath it is rescanned, not skipped")
{
	// Not something appendPlayLog does — but a scan index past the end must
	// not turn into a session that never reveals.
	PlayErrorReveal r;
	std::vector<PlayLogEntry> log;
	for (int i = 0; i < 4; ++i) log.push_back(entry(HE::LogLevel::Warning, "w"));
	CHECK_FALSE(r.poll(true, log));
	log.clear();
	log.push_back(entry(HE::LogLevel::Error, "e"));
	CHECK(r.poll(true, log));
}
