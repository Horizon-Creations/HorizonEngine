#pragma once

// ── "The first error of this play session" ───────────────────────────────────
// The console is closed by default and a play session's errors used to go to
// three places nobody was looking at while playing: the footer bell (ambient by
// its own description), the console buffer (closed) and the post-play report
// (opens after Stop). A script that failed in onStart therefore looked like a
// script that does nothing until the session was over. The editor opens the
// console for the FIRST Error/Critical of a play session, the way it raises
// the Watch window for a new breakpoint stop — once, on the edge, so a console
// the user closed again stays closed for the rest of that session.
//
// The decision alone, ImGui-free, so the test can run it through a session:
// EditorUI owns the window flag and does the revealing.

#include "EditorApplication.h"   // PlayLogEntry
#include <Diagnostics/Log.h>     // HE::LogLevel

#include <cstddef>
#include <vector>

namespace HE::Ed
{
	struct PlayErrorReveal
	{
		// Feed once per frame with the session state and the play log (the
		// caller holds playLogMutex). Returns true on exactly one frame per
		// play session: the first in which an Error or worse is in the log.
		// Warnings do not count — the post-play report lists them, and a
		// session with a handful of warnings is an ordinary session.
		bool poll(bool isPlaying, const std::vector<PlayLogEntry>& log)
		{
			if (!isPlaying)
			{
				// Session over (or none): arm for the next one. Also what a
				// play start looks like from here — the log is cleared before
				// isPlaying rises, so the scan index starts at 0 with it.
				m_fired   = false;
				m_scanned = 0;
				return false;
			}
			if (m_fired) return false;
			// A session's log only grows (appendPlayLog), so scanning from the
			// last seen row is enough — and a repeat collapsed into an existing
			// row was already looked at when it was new.
			if (m_scanned > log.size()) m_scanned = 0;   // defensive: a cleared log
			for (; m_scanned < log.size(); ++m_scanned)
			{
				if (log[m_scanned].level >= HE::LogLevel::Error)
				{
					m_fired = true;
					++m_scanned;
					return true;
				}
			}
			return false;
		}

	private:
		bool        m_fired   = false;
		std::size_t m_scanned = 0;
	};
}
