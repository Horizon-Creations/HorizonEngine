#include "ConsoleView.h"

#include <Diagnostics/Log.h>
#include <HorizonCode/HorizonCode.h>   // currentExecSite — which node wrote a record
#include <Scripting/ScriptTypes.h>     // parseScriptErrorLocation — which line a Lua/Python error names

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<imgui.h>)
#define HE_CONSOLEVIEW_IMGUI 1
#include "EditorWidgets.h"       // the help-aware checkbox / button / menu item
#include "EditorHelp.h"          // "Console/<label>" scope for its controls
#include "EditorTheme.h"         // body / dimmed text
#include "EditorToolbar.h"       // the shared "needs attention" / "went wrong" colours
#include <cfloat>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>   // InputTextWithHint over std::string
#endif

namespace ConsoleView
{

// ─── The buffer, and the sink that fills it ──────────────────────────────────
// ImGui-free on purpose (same split as NotificationStore): everything below
// this line runs on whatever thread called HE_LOG.

namespace
{
	// One row as the panel keeps it. The whole line is composed HERE, in the
	// sink, rather than per frame from the record's parts — the record's char
	// pointers only live for the duration of the sink call, and composing once
	// leaves the draw a clipper over ready strings.
	struct Line
	{
		HE::LogLevel level  = HE::LogLevel::Info;
		int          repeat = 1;     // consecutive identical lines collapse into one row
		std::uint64_t id    = 0;     // stable across trimming, for the selection
		std::uint8_t cat    = 0;     // HE::Log::Cat
		bool         cont   = false; // 2nd+ row of a multi-line record: no time, no category
		std::string  time;           // "HH:MM:SS.mmm" of the first occurrence
		std::string  lastTime;       // of the latest one; empty while repeat == 1
		std::string  msg;            // this row's part of the message
		std::string  loc;            // "File.cpp:212" — only on the record's last row, and only from Warning up
		// The log-file shape — time, level, category, message, location — which is
		// what Copy puts on the clipboard and what the search looks through, so a
		// pasted line reads like the same line in HorizonEngine.log.
		std::string  text;
		// The HorizonCode node that was executing when the record was written —
		// a Print's own line, a warning from inside a node, an engine row's
		// error — so the row can lead back to it.
		// Empty key / node 0 = written by nothing that was a node.
		std::string  hcKey;
		int          hcNode     = 0;
		std::uint32_t hcInstance = 0;
		// The Lua/Python script and line a script error names (`mover:7: …`,
		// HE::parseScriptErrorLocation), so the row can lead to the line. Line 0
		// = names none. The name is the module name the script runs under; the
		// asset behind it is looked up on the click, not here — the sink must
		// stay short and lock-free.
		std::string  scriptName;
		int          scriptLine = 0;
		bool         python     = false;   // which backend said it (Cat::Python)
	};

	// The hand-off between "any thread logs" and "the frame draws". Kept small
	// and drained whole, so the lock is held for a push or a swap and never
	// across the drawing — the UI must never be inside this mutex while ImGui
	// runs, and the sink must never log while inside it (see attachToEngineLog).
	std::mutex       s_pendingMutex;
	std::deque<Line> s_pending;
	int              s_sink = 0;

	// A console that grows for eight hours is a leak with a scrollbar. The bound
	// applies to the hand-off buffer as well as to what the panel keeps: nothing
	// drains this until somebody opens the window, and plenty of sessions never
	// do.
	constexpr std::size_t k_maxLines = 5000;

	// The log's own vocabulary and column width, so a line here and the same line
	// in HorizonEngine.log read alike (Log.cpp's kLevelShort).
	constexpr const char* kLevelTag[] = { "TRACE", "DEBUG", " INFO", " WARN", "ERROR", " CRIT" };
	constexpr int         kLevelCount = 6;
	static_assert(kLevelCount == static_cast<int>(HE::LogLevel::Off),
	              "LogLevel gained a severity the console does not name");
	static_assert(static_cast<int>(HE::Log::Cat::Count) <= 64,
	              "the category filter is a 64-bit mask");

	int levelIndex(HE::LogLevel level)
	{
		const int i = static_cast<int>(level);
		return (i >= 0 && i < kLevelCount) ? i : static_cast<int>(HE::LogLevel::Info);
	}

	// The absolute build path of a machine that is not the user's says nothing to
	// them and eats the whole line.
	const char* shortFileName(const char* path)
	{
		const char* slash = std::strrchr(path, '/');
#ifdef _WIN32
		const char* back = std::strrchr(path, '\\');
		if (back && (!slash || back > slash)) slash = back;
#endif
		return slash ? slash + 1 : path;
	}

	void pushLocked(Line&& line)
	{
		s_pending.push_back(std::move(line));
		if (s_pending.size() > k_maxLines) s_pending.pop_front();
	}

	void logSink(const HE::Log::Record& record, void* /*user*/)
	{
		// Called on the logging thread with the LOG's mutex held (Log.h). So:
		// short, and it must NEVER log — write() would take that same mutex again,
		// and a sink that logs what it was handed is an endless loop besides.
		const char* message = record.message ? record.message : "";
		std::size_t msgLen  = std::strlen(message);
		while (msgLen > 0 && (message[msgLen - 1] == '\n' || message[msgLen - 1] == '\r'))
			--msgLen;
		if (msgLen == 0) return;

		char timeFull[24] = "??:??:??.???";
		{
			const std::time_t secs = static_cast<std::time_t>(record.unixMillis / 1000);
			const int         ms   = static_cast<int>(record.unixMillis % 1000);
			char              timeBuf[16] = "??:??:??";
			std::tm           tmBuf{};
#ifdef _WIN32
			if (localtime_s(&tmBuf, &secs) == 0)
				std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);
#else
			if (localtime_r(&secs, &tmBuf))
				std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);
#endif
			std::snprintf(timeFull, sizeof(timeFull), "%s.%03d", timeBuf, ms);
		}
		const char* categoryText = HE::Log::categoryName(record.category);
		char prefix[96];
		std::snprintf(prefix, sizeof(prefix), "%s  %s  %-11s  ", timeFull,
		              kLevelTag[levelIndex(record.level)], categoryText);

		// Where it came from, for the severities where that is the next question.
		// Below Warning it is noise: a Trace line nobody is hunting does not need
		// half its width spent on a filename.
		std::string loc;
		if (record.file && record.file[0] != '\0' && record.level >= HE::LogLevel::Warning)
		{
			char buf[160];
			std::snprintf(buf, sizeof(buf), "%s:%d", shortFileName(record.file), record.line);
			loc = buf;
		}

		// One row per line of the message. A multi-line record (a shader
		// compiler's error list, the startup banner) would otherwise be a single
		// row several times taller than the rest, and the list clipper below
		// assumes every row has the same height — get that wrong and the
		// scrollbar stops agreeing with the content.
		const char* const msgEnd = message + msgLen;

		// Which node this came out of, if any. The site is thread-local and the
		// sink runs on the thread that logged, so this is the node that is
		// executing right now on the thread that wrote the record — for every
		// category, not just HorizonCode's: an engine row an Engine Call node
		// invoked logs under Physics or Audio, and that line wants its node
		// just as much. The key pointer is only good for the duration of the
		// node (HorizonCode.h), hence the copy.
		const HorizonCode::ExecSite& site = HorizonCode::currentExecSite();

		// Which script line this names, if any. Parsed from the RAW message,
		// never from the composed row: the row starts with a clock, and
		// "12:34:56" has the shape of a position. Only the script categories
		// are asked — a shader compiler's "file.glsl:12:" is not a script.
		HE::ScriptErrorLocation scriptAt;
		const bool scriptCat = record.category == HE::Log::Cat::Lua ||
		                       record.category == HE::Log::Cat::Python ||
		                       record.category == HE::Log::Cat::Script;
		const bool hasScriptAt = scriptCat && record.level >= HE::LogLevel::Warning &&
		                         HE::parseScriptErrorLocation(std::string(message, msgLen), scriptAt);

		std::lock_guard<std::mutex> lock(s_pendingMutex);
		for (const char* at = message; at < msgEnd; )
		{
			const char* end = at;
			while (end < msgEnd && *end != '\n') ++end;

			Line line;
			line.level = record.level;
			line.cat   = static_cast<std::uint8_t>(record.category);
			line.cont  = at != message;
			line.time  = timeFull;
			if (site.nodeId != 0)
			{
				line.hcKey      = site.classKey;
				line.hcNode     = site.nodeId;
				line.hcInstance = site.instance;
			}
			// The position belongs to the record's first row, like the suffix
			// belongs to its last: a multi-line message names one place.
			if (hasScriptAt && at == message)
			{
				line.scriptName = scriptAt.script;
				line.scriptLine = scriptAt.line;
				// Compile errors log under the backend's own category; a handler
				// that failed at run time logs under Script with the language as
				// the first word ("Python script instance 3 failed in …",
				// ScriptContext's HE_SCRIPT_CALL), so that word is read too.
				line.python     = record.category == HE::Log::Cat::Python ||
				                  (record.category == HE::Log::Cat::Script &&
				                   std::strncmp(message, "Python", 6) == 0);
			}
			line.msg.assign(at, static_cast<std::size_t>(end - at));
			line.text = prefix;
			line.text += line.msg;
			if (end == msgEnd && !loc.empty())   // the location belongs to the record, once
			{
				line.loc   = loc;
				line.text += "  (" + loc + ")";
			}
			pushLocked(std::move(line));

			at = (end < msgEnd) ? end + 1 : msgEnd;
		}
	}
}

void attachToEngineLog()
{
	// No lock of ours across addSink/removeSink. Those take the LOG's mutex, and
	// the log calls sinks with that same mutex held — holding s_pendingMutex here
	// would establish ours→log against the log→ours the sink itself walks, and
	// any thread logging during startup or teardown would deadlock against it.
	// s_sink needs no lock instead: attach and detach run once each on the main
	// thread, and the sink never reads it. Same lesson as
	// NotificationStore::attachToEngineLog — the long form of it is over there.
	if (s_sink != 0) return;
	s_sink = HE::Log::addSink(&logSink, nullptr);
}

void detachFromEngineLog()
{
	if (s_sink == 0) return;
	const int handle = s_sink;
	// Cleared BEFORE the removal, so a record already inside the sink finds a
	// buffer that is still whole — removeSink only promises no FURTHER calls.
	s_sink = 0;
	HE::Log::removeSink(handle);
}

// ─── The view ────────────────────────────────────────────────────────────────

#ifdef HE_CONSOLEVIEW_IMGUI
namespace
{
	// Frame thread only, like every other panel's state.
	std::vector<Line> s_lines;                  // the ring the panel shows
	std::vector<int>  s_visible;                // indices into s_lines that pass the filter
	bool              s_visibleDirty = true;
	std::string       s_search;
	std::uint64_t     s_nextId       = 1;
	std::uint64_t     s_selectedId   = 0;   // the row a click picked; 0 = none
	// Everything on by default: the log's own verbosity has already dropped what
	// the user did not ask for (Trace and Debug are off for most categories), so
	// a console that hides more on top of that would be lying twice.
	constexpr unsigned kAllLevels    = (1u << kLevelCount) - 1u;
	unsigned          s_levelMask    = kAllLevels;
	// A bit per HE::Log::Cat; SET = hidden, so "nothing hidden" is the zero a
	// fresh session starts with. s_catSeen is which categories the buffer has
	// ever held — the popup lists those, not the 44 the engine knows.
	std::uint64_t     s_catHidden    = 0;
	std::uint64_t     s_catSeen      = 0;

	// What the last rebuildVisible counted, in OCCURRENCES (a row folded from 38
	// repeats counts 38) — the chips and the footer say how much happened, not
	// how many rows it took to show.
	// Level counts ignore the level filter and category counts ignore the
	// category filter: a chip says what clicking it would show, not what it
	// shows now. Both respect the search.
	long              s_countByLevel[kLevelCount] = {};
	long              s_countByCat[64]            = {};
	long              s_totalShown   = 0;
	long              s_totalAll     = 0;

	// Following the newest line. s_atBottom is this frame's answer (set while
	// the rows are drawn, read by the footer); s_pinnedShown is how many
	// occurrences were showing the last time the view WAS at the bottom, which
	// is what makes "3 new" a subtraction instead of a second counter to keep
	// right across trims, filters and Clear.
	bool              s_atBottom     = true;
	bool              s_jumpToLatest = false;
	long              s_pinnedShown  = 0;

	constexpr const char* kLevelName[] = {
		"Trace", "Debug", "Info", "Warning", "Error", "Critical"
	};

	// The four chips. Trace folds into Debug and Critical into Error: nobody
	// reads a log by asking for exactly "Critical", and six chips would be a
	// toolbar of their own.
	struct LevelChip
	{
		const char* label;      // what it says before its count
		const char* helpKey;    // fixed, because the label carries a number
		unsigned    bits;
		HE::LogLevel colour;    // whose colour it wears
	};
	constexpr LevelChip kChips[] = {
		{ "Debug",  "Console/Debug",    (1u << 0) | (1u << 1), HE::LogLevel::Debug   },
		{ "Info",   "Console/Info",     (1u << 2),             HE::LogLevel::Info    },
		{ "Warn",   "Console/Warnings", (1u << 3),             HE::LogLevel::Warning },
		{ "Error",  "Console/Errors",   (1u << 4) | (1u << 5), HE::LogLevel::Error   },
	};
	static_assert(static_cast<int>(HE::LogLevel::Trace) == 0 && static_cast<int>(HE::LogLevel::Debug) == 1 &&
	              static_cast<int>(HE::LogLevel::Info) == 2 && static_cast<int>(HE::LogLevel::Warning) == 3 &&
	              static_cast<int>(HE::LogLevel::Error) == 4 && static_cast<int>(HE::LogLevel::Critical) == 5,
	              "kChips names levels by their bit");

	// Severities are the one thing EditorTheme deliberately does NOT hand out a
	// palette for — "there hue IS the data" (EditorTheme.h) — so they come from
	// the shared state colours instead, the same amber every warning in the
	// editor wears and the same red every failure does. Nothing here is picked
	// by eye.
	ImU32 colourFor(HE::LogLevel level)
	{
		using namespace HE::Ed;
		switch (level)
		{
		case HE::LogLevel::Trace:
		case HE::LogLevel::Debug:    return Theme::u32(Theme::TextDim);
		case HE::LogLevel::Warning:  return EditorToolbar::kWarn;
		case HE::LogLevel::Error:
		case HE::LogLevel::Critical: return EditorToolbar::kBad;
		default:                     return Theme::u32(Theme::Text);
		}
	}

	// Takes everything the sink has collected since the last frame. The lock
	// covers the swap alone; the collapsing and the trimming happen on our own
	// vector, where no other thread can be.
	bool drainPending()
	{
		std::deque<Line> taken;
		{
			std::lock_guard<std::mutex> lock(s_pendingMutex);
			taken.swap(s_pending);
		}
		if (taken.empty()) return false;

		for (Line& line : taken)
		{
			s_catSeen |= std::uint64_t(1) << (line.cat & 63);

			// The log folds consecutive identical records itself and says so
			// afterwards ("(previous message repeated 37 more times)", Log.cpp
			// flushRepeats), under the same level and category as what it folded.
			// Read as the row's repeat count, that note is the ×N badge — as its
			// own row it would be a second line of noise under every error that
			// fired more than once.
			if (!s_lines.empty() && !line.cont)
			{
				unsigned long long more = 0;
				int                end  = 0;
				Line&              last = s_lines.back();
				if (last.level == line.level && last.cat == line.cat &&
				    std::sscanf(line.msg.c_str(), "(previous message repeated %llu more time%n",
				                &more, &end) == 1 && end > 0 && more > 0 &&
				    line.msg.back() == ')')
				{
					last.repeat += static_cast<int>(std::min<unsigned long long>(more, 1000000));
					last.lastTime = line.time;
					continue;
				}
			}

			// A repeat is counted, not dropped. The notification bell drops one
			// (NotificationStore.h) because a bell that rings four thousand times
			// is a bell nobody reads; a console that drops lines misrepresents
			// what the program did. Counting bounds the same case — one error
			// logged every frame would otherwise flush the whole ring in a minute
			// and leave nothing but itself.
			// Same text from another NODE is not a repeat: two Prints saying
			// "here" are two places, and collapsing them would lose one. The
			// CLOCK is not part of the comparison — it is what differs between
			// every repeat there ever was.
			if (!s_lines.empty())
			{
				Line& last = s_lines.back();
				if (last.level == line.level && last.cat == line.cat && last.cont == line.cont &&
				    last.msg == line.msg && last.loc == line.loc &&
				    last.hcNode == line.hcNode && last.hcKey == line.hcKey)
				{
					++last.repeat;
					last.lastTime = line.time;
					continue;
				}
			}
			line.id = s_nextId++;
			s_lines.push_back(std::move(line));
		}

		if (s_lines.size() > k_maxLines)
			s_lines.erase(s_lines.begin(),
			              s_lines.begin() + static_cast<std::ptrdiff_t>(s_lines.size() - k_maxLines));
		return true;
	}

	bool containsNoCase(const std::string& hay, const std::string& needle)
	{
		const auto lower = [](char c) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		};
		return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
		                   [&lower](char a, char b) { return lower(a) == lower(b); }) != hay.end();
	}

	// Which rows pass the filter, and what each filter would show if it were
	// the one clicked (the chips' and the popup's counts) — one pass over the
	// ring, only when something changed, so it is also what keeps those counts
	// right after a trim: they are recounted, never decremented.
	void rebuildVisible()
	{
		s_visible.clear();
		s_visible.reserve(s_lines.size());
		std::fill(std::begin(s_countByLevel), std::end(s_countByLevel), 0L);
		std::fill(std::begin(s_countByCat), std::end(s_countByCat), 0L);
		s_totalShown = 0;
		s_totalAll   = 0;
		for (int i = 0; i < static_cast<int>(s_lines.size()); ++i)
		{
			const Line& line = s_lines[static_cast<std::size_t>(i)];
			if (!s_search.empty() && !containsNoCase(line.text, s_search)) continue;
			const int  lv       = levelIndex(line.level);
			const bool levelOn  = (s_levelMask & (1u << lv)) != 0;
			const bool catOn    = (s_catHidden & (std::uint64_t(1) << (line.cat & 63))) == 0;
			if (catOn)   s_countByLevel[lv] += line.repeat;
			if (levelOn) s_countByCat[line.cat & 63] += line.repeat;
			if (!levelOn || !catOn) continue;
			s_totalShown += line.repeat;
			s_visible.push_back(i);
		}
		for (const Line& line : s_lines) s_totalAll += line.repeat;
		s_visibleDirty = false;
	}

	// The row as the user sees it. Drawing and copying go through the same
	// function so the text on screen and the text on the clipboard cannot drift.
	void appendRow(const Line& line, std::string& out)
	{
		out += line.text;
		if (line.repeat > 1)
		{
			out += "  (x";
			out += std::to_string(line.repeat);
			out += ')';
		}
	}

	void copyVisible()
	{
		std::string all;
		for (const int index : s_visible)
		{
			appendRow(s_lines[static_cast<std::size_t>(index)], all);
			all += '\n';
		}
		if (!all.empty()) ImGui::SetClipboardText(all.c_str());
	}

	void clearLines()
	{
		// The undrained half too: those lines are older than the click that
		// cleared, and letting them arrive next frame would look like the Clear
		// button had missed.
		{
			std::lock_guard<std::mutex> lock(s_pendingMutex);
			s_pending.clear();
		}
		s_lines.clear();
		s_visible.clear();
		s_visibleDirty = true;   // the counts go to zero with the rows
		s_selectedId   = 0;
		s_pinnedShown  = 0;
		s_catSeen      = 0;
		s_catHidden    = 0;
	}

	ImU32 withAlpha(ImU32 colour, int alpha)
	{
		return (colour & ~static_cast<ImU32>(IM_COL32_A_MASK)) |
		       (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
	}

	// A ">" drawn from two lines, because the editor's face has no arrow glyphs
	// (they come out as empty boxes) and the monospace face is not guaranteed
	// to either.
	void drawChevron(ImDrawList* dl, ImVec2 centre, float half, ImU32 colour)
	{
		dl->AddLine(ImVec2(centre.x - half * 0.5f, centre.y - half),
		            ImVec2(centre.x + half * 0.5f, centre.y), colour, 1.5f);
		dl->AddLine(ImVec2(centre.x + half * 0.5f, centre.y),
		            ImVec2(centre.x - half * 0.5f, centre.y + half), colour, 1.5f);
	}

	// What a click on a chip does. The plain click is "only this": the common
	// reason to touch the level filter is to look at errors, and with every level
	// on, a toggle would make that three clicks. A second click on the lone chip
	// brings everything back; Ctrl (Cmd on macOS) adds or removes one instead.
	void applyChip(unsigned bits, bool additive)
	{
		if (additive)
		{
			s_levelMask = ((s_levelMask & bits) == bits) ? (s_levelMask & ~bits)
			                                             : (s_levelMask | bits);
			if (s_levelMask == 0) s_levelMask = kAllLevels;
		}
		else
		{
			s_levelMask = (s_levelMask == bits) ? kAllLevels : bits;
		}
		s_visibleDirty = true;
	}

	// A pill: outlined while it is just an option, filled with its level's colour
	// while it is THE filter. With nothing filtered, "All" is the filled one —
	// four filled chips side by side would read as "everything is selected" and
	// say nothing about whether the list is cut down.
	bool chip(const char* id, const char* text, ImU32 colour, bool active, bool dim)
	{
		const ImVec2 textSize = ImGui::CalcTextSize(text);
		const ImVec2 size(textSize.x + 20.0f, ImGui::GetFrameHeight());
		const ImVec2 pos = ImGui::GetCursorScreenPos();

		ImGui::PushID(id);
		const bool pressed = ImGui::InvisibleButton("##chip", size);
		ImGui::PopID();
		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 end(pos.x + size.x, pos.y + size.y);
		const float  round = size.y * 0.5f;
		const ImU32  dimText = HE::Ed::Theme::u32(HE::Ed::Theme::TextDim);
		if (active)
		{
			dl->AddRectFilled(pos, end, withAlpha(colour, hovered ? 72 : 52), round);
			dl->AddRect(pos, end, withAlpha(colour, 170), round);
		}
		else
		{
			if (hovered) dl->AddRectFilled(pos, end, withAlpha(colour, 22), round);
			dl->AddRect(pos, end, hovered ? withAlpha(colour, 130) : withAlpha(dimText, 90), round);
		}
		dl->AddText(ImVec2(pos.x + 10.0f, pos.y + (size.y - textSize.y) * 0.5f),
		            dim && !active ? dimText : colour, text);
		return pressed;
	}

	void drawLevelChips()
	{
		const bool additive = ImGui::GetIO().KeyCtrl;   // Cmd on macOS (ConfigMacOSXBehaviors)
		const bool filtered = s_levelMask != kAllLevels;
		char       label[40];

		long all = 0;
		for (int i = 0; i < kLevelCount; ++i) all += s_countByLevel[i];
		std::snprintf(label, sizeof(label), "All %ld", all);
		if (chip("all", label, HE::Ed::Theme::u32(HE::Ed::Theme::Text), !filtered, false))
		{
			s_levelMask    = kAllLevels;
			s_visibleDirty = true;
		}
		EditorWidgets::helpForKey("Console/All");

		for (const LevelChip& c : kChips)
		{
			long n = 0;
			for (int i = 0; i < kLevelCount; ++i)
				if (c.bits & (1u << i)) n += s_countByLevel[i];
			std::snprintf(label, sizeof(label), "%s %ld", c.label, n);

			ImGui::SameLine();
			const bool active = filtered && (s_levelMask & c.bits) == c.bits;
			if (chip(c.label, label, colourFor(c.colour), active, n == 0))
				applyChip(c.bits, additive);
			EditorWidgets::helpForKey(c.helpKey);
		}
	}

	void drawCategoryFilter()
	{
		int hidden = 0;
		for (int i = 0; i < 64; ++i)
			if (s_catHidden & (std::uint64_t(1) << i)) ++hidden;

		char label[32];
		if (hidden == 0) std::snprintf(label, sizeof(label), "Category");
		else             std::snprintf(label, sizeof(label), "Category (%d hidden)", hidden);

		if (ImGui::Button(label)) ImGui::OpenPopup("##consoleCategories");
		EditorWidgets::helpForKey("Console/Category");
		if (!ImGui::BeginPopup("##consoleCategories")) return;

		if (EditorWidgets::smallButton("Show all categories"))
		{
			s_catHidden    = 0;
			s_visibleDirty = true;
		}
		ImGui::Separator();

		bool any = false;
		for (int i = 0; i < static_cast<int>(HE::Log::Cat::Count); ++i)
		{
			const std::uint64_t bit = std::uint64_t(1) << i;
			if ((s_catSeen & bit) == 0) continue;
			any = true;
			bool on = (s_catHidden & bit) == 0;
			if (EditorWidgets::checkbox(HE::Log::categoryName(static_cast<HE::Log::Cat>(i)), &on))
			{
				s_catHidden    = on ? (s_catHidden & ~bit) : (s_catHidden | bit);
				s_visibleDirty = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%ld", s_countByCat[i]);
		}
		if (!any) ImGui::TextDisabled("Nothing logged yet.");
		ImGui::EndPopup();
	}

	// The time column's text for a row: the clock without the milliseconds,
	// which are in the tooltip and in the copied line.
	constexpr int kClockChars = 8;   // "HH:MM:SS"

	std::string tooltipFor(const Line& line, const Hooks& hooks, bool fromNode, bool toLine)
	{
		std::string t = line.time;
		t += "   ";
		t += kLevelName[levelIndex(line.level)];
		t += "   ";
		t += HE::Log::categoryName(static_cast<HE::Log::Cat>(line.cat));
		if (line.repeat > 1)
		{
			t += "\nSeen " + std::to_string(line.repeat) + " times";
			if (!line.lastTime.empty()) t += ", last at " + line.lastTime;
		}
		if (!line.loc.empty()) t += "\nLogged at " + line.loc;
		if (fromNode)
		{
			const std::string where = hooks.tabLabelFor ? hooks.tabLabelFor(line.hcKey) : line.hcKey;
			t += "\nWritten by node " + std::to_string(line.hcNode) + " of " + where +
			     "\nClick the link or double-click the row to show it.";
		}
		else if (toLine)
		{
			t += "\nLine " + std::to_string(line.scriptLine) + " of script '" + line.scriptName +
			     "'\nClick the link or double-click the row to show it.";
		}
		return t;
	}

	// What a row needs of the frame's layout. Measured once per frame in draw() and
	// only read per row.
	struct RowLayout
	{
		float  availW, lineH, rowH, pitch;
		float  xTime, xCat, xMsg;
		ImU32  textCol, dimCol, linkCol, linkHot;
		ImVec4 surface;
	};

	constexpr float kPadL = 12.0f;

	// One visible row: the Selectable with the content painted over it. The context
	// menu's Clear only sets clearRequested, so draw() can clear after the loop.
	void drawRow(const Line& line, const Hooks& hooks, const RowLayout& lay, const ImGuiStyle& style,
	             ImDrawList* dl, std::string& row, bool& clearRequested)
	{
		const auto& [availW, lineH, rowH, pitch, xTime, xCat, xMsg, textCol, dimCol, linkCol, linkHot, surface] = lay;
		row.clear();
		appendRow(line, row);

		// A line a HorizonCode node wrote leads back to that node; a
		// Lua/Python error that names its line leads to that line of
		// the script. The node wins when a row has both (an Engine
		// Call node that ran a script) — the node is where the user
		// was. The asset behind a script is looked up on the click.
		const bool fromNode = line.hcNode != 0 && hooks.goToNode;
		const bool toLine   = !fromNode && line.scriptLine > 0 && hooks.goToLine;
		const bool leads    = fromNode || toLine;
		auto goTo = [&]()
		{
			if (fromNode)    hooks.goToNode(line.hcKey, line.hcNode);
			else if (toLine) hooks.goToLine(line.scriptName, line.scriptLine, line.python);
		};

		// Where each part sits. The badge and the link follow the
		// message rather than hugging the right edge: in a list that
		// scrolls sideways, "the right edge" is wherever the longest
		// line ends, not where the viewer is looking.
		const ImVec2 p = ImGui::GetCursorScreenPos();
		float       x  = p.x + xMsg + ImGui::CalcTextSize(line.msg.c_str()).x;
		char        badge[16] = "";
		float       badgeX = 0.0f, badgeW = 0.0f;
		if (line.repeat > 1)
		{
			std::snprintf(badge, sizeof(badge), "x%d", line.repeat);
			badgeW = ImGui::CalcTextSize(badge).x + 12.0f;
			badgeX = x + 10.0f;
			x      = badgeX + badgeW;
		}
		char  linkText[96] = "";
		float linkX = 0.0f, linkW = 0.0f;
		if (leads)
		{
			if (fromNode) std::snprintf(linkText, sizeof(linkText), "node %d", line.hcNode);
			else          std::snprintf(linkText, sizeof(linkText), "%s:%d",
			                            line.scriptName.c_str(), line.scriptLine);
			linkW = ImGui::CalcTextSize(linkText).x + 16.0f;   // text, gap, chevron
			linkX = x + 14.0f;
			x     = linkX + linkW;
		}
		const float rowW = std::max(availW, x - p.x + kPadL);

		// Under the Selectable, so its hover and selection wash lies over
		// the tint and not the other way round. Spanning the pitch, as the
		// Selectable's own highlight does, keeps neighbouring rows of one
		// severity a single band.
		const bool isCrit  = line.level == HE::LogLevel::Critical;
		const bool isError = line.level >= HE::LogLevel::Error;
		const bool isWarn  = line.level == HE::LogLevel::Warning;
		// The row's text sits on its own severity's tint: red on red,
		// which put an error at 3.9:1 and a critical one at 3.2:1. So the
		// text is held to the theme's floor on that ground (the window's,
		// the lighter one the console can sit on, under the tint).
		ImVec4 ground = surface;
		if (isError || isWarn)
		{
			const float up   = std::floor(style.ItemSpacing.y * 0.5f);
			const ImVec2 lo(p.x, p.y - up), hi(p.x + rowW, p.y - up + pitch);
			ImU32 tint = isWarn ? EditorToolbar::kWarnWell : EditorToolbar::kBadWell;
			if (isCrit) tint = withAlpha(EditorToolbar::kBad, 76);
			dl->AddRectFilled(lo, hi, tint);
			ground = HE::Ed::Theme::over(ImGui::ColorConvertU32ToFloat4(tint), surface);
			dl->AddRectFilled(lo, ImVec2(p.x + (isCrit ? 4.0f : 3.0f), hi.y),
			                  isWarn ? EditorToolbar::kWarn : EditorToolbar::kBad);
		}

		// Identified by the line's own serial, which survives a trim, so
		// the selection and an open menu stay on their row while the
		// ring scrolls under them.
		ImGui::PushID(static_cast<int>(line.id & 0x7fffffff));
		if (ImGui::Selectable("##row", line.id == s_selectedId,
		                      ImGuiSelectableFlags_AllowDoubleClick, ImVec2(rowW, rowH)))
			s_selectedId = line.id;
		const bool hovered    = ImGui::IsItemHovered();
		// Asked now: the context menu below is a window of its own, and
		// "the last item" is not the row any more once it has been drawn.
		const bool tipWanted  = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) s_selectedId = line.id;

		// The context menu hangs off the row, so it is opened before
		// anything else can become the last item.
		if (ImGui::BeginPopupContextItem("##consoleLine"))
		{
			if (fromNode)
			{
				if (EditorWidgets::menuItem("Go to Node")) goTo();
				ImGui::Separator();
			}
			if (toLine)
			{
				if (EditorWidgets::menuItem("Go to Line")) goTo();
				ImGui::Separator();
			}
			if (EditorWidgets::menuItem("Copy Line")) ImGui::SetClipboardText(row.c_str());
			if (EditorWidgets::menuItem("Copy All Shown")) copyVisible();
			ImGui::Separator();
			// Deferred: clearing here would drop the vector this loop
			// is walking.
			if (EditorWidgets::menuItem("Clear")) clearRequested = true;
			ImGui::EndPopup();
		}

		bool linkHovered = false;
		if (leads && hovered)
		{
			linkHovered = ImGui::IsMouseHoveringRect(ImVec2(linkX - 4.0f, p.y),
			                                         ImVec2(linkX + linkW + 2.0f, p.y + rowH));
			if (linkHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			// One click on the link, or a double-click anywhere on the row.
			if ((linkHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
			    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				goTo();
		}
		if (tipWanted)
		{
			const std::string tip = tooltipFor(line, hooks, fromNode, toLine);
			ImGui::SetTooltip("%s", tip.c_str());
		}
		ImGui::PopID();

		// The content, over everything.
		const float ty = p.y + (rowH - lineH) * 0.5f;
		const bool  tinted = isError || isWarn;
		const auto  onRow  = [&](ImU32 c, const ImVec4& under) {
			return HE::Ed::Theme::u32(HE::Ed::Theme::readableOn(ImGui::ColorConvertU32ToFloat4(c), under));
		};
		if (!line.cont)
		{
			dl->AddText(ImVec2(p.x + xTime, ty), tinted ? onRow(dimCol, ground) : dimCol,
			            line.time.c_str(), line.time.c_str() + kClockChars);
			dl->AddText(ImVec2(p.x + xCat, ty), withAlpha(textCol, 170),
			            HE::Log::categoryName(static_cast<HE::Log::Cat>(line.cat)));
		}
		const ImU32 levelCol = colourFor(line.level);
		const ImU32 msgCol   = tinted ? onRow(levelCol, ground) : levelCol;
		dl->AddText(ImVec2(p.x + xMsg, ty), msgCol, line.msg.c_str());
		if (line.repeat > 1)
		{
			// The count's pill is the level's colour again, one layer
			// deeper: 2.4:1 on a critical row before it was measured.
			const ImVec2 lo(badgeX, p.y + 2.0f), hi(badgeX + badgeW, p.y + rowH - 1.0f);
			const ImU32 pill = withAlpha(levelCol, 56);
			dl->AddRectFilled(lo, hi, pill, (hi.y - lo.y) * 0.5f);
			const ImVec4 pillGround = HE::Ed::Theme::over(ImGui::ColorConvertU32ToFloat4(pill), ground);
			dl->AddText(ImVec2(badgeX + 6.0f, ty), onRow(levelCol, pillGround), badge);
		}
		if (leads)
		{
			const ImU32 col = tinted ? onRow(linkHovered ? linkHot : linkCol, ground)
			                         : (linkHovered ? linkHot : linkCol);
			dl->AddText(ImVec2(linkX, ty), col, linkText);
			const float underline = ty + lineH;
			if (linkHovered)
				dl->AddLine(ImVec2(linkX, underline), ImVec2(linkX + linkW - 16.0f, underline), col);
			drawChevron(dl, ImVec2(linkX + linkW - 5.0f, p.y + rowH * 0.5f), 3.5f, col);
		}
	}
}
#endif // HE_CONSOLEVIEW_IMGUI

void pump()
{
#ifdef HE_CONSOLEVIEW_IMGUI
	if (drainPending()) s_visibleDirty = true;
#endif
}

void draw(const Hooks& hooks)
{
	// Every control here is looked up as "Console/<its label>".
	HE::Ed::Help::Scope helpScope("Console");
#ifdef HE_CONSOLEVIEW_IMGUI
	pump();
	// Rebuilt before anything can read it: a trim inside the drain shifts every
	// index, and the Copy button below walks this list.
	if (s_visibleDirty) rebuildVisible();

	const ImGuiStyle& style = ImGui::GetStyle();
	// Only the context menu inside the row loop defers its clear — see there.
	bool clearRequested = false;

	// ── Toolbar ──────────────────────────────────────────────────────────────
	// Search on the left, what to show in the middle, what to do with it on the
	// right. The search is the one control that stretches, so everything else is
	// measured first and it takes what is left.
	{
		const auto buttonWidth = [&](const char* text) {
			return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f;
		};
		char probe[48];
		float middle = 0.0f;
		long  all    = 0;
		for (int i = 0; i < kLevelCount; ++i) all += s_countByLevel[i];
		std::snprintf(probe, sizeof(probe), "All %ld", all);
		middle += ImGui::CalcTextSize(probe).x + 20.0f + style.ItemSpacing.x;
		for (const LevelChip& c : kChips)
		{
			long n = 0;
			for (int i = 0; i < kLevelCount; ++i)
				if (c.bits & (1u << i)) n += s_countByLevel[i];
			std::snprintf(probe, sizeof(probe), "%s %ld", c.label, n);
			middle += ImGui::CalcTextSize(probe).x + 20.0f + style.ItemSpacing.x;
		}
		middle += buttonWidth("Category (44 hidden)") + style.ItemSpacing.x;
		const float rightW = buttonWidth("Copy") + style.ItemSpacing.x + buttonWidth("Clear");

		const float searchW = std::clamp(ImGui::GetContentRegionAvail().x - middle - rightW -
		                                 style.ItemSpacing.x, 90.0f, 240.0f);
		ImGui::SetNextItemWidth(searchW);
		if (ImGui::InputTextWithHint("##consoleSearch", "Search", &s_search)) s_visibleDirty = true;
		EditorWidgets::helpForKey("Console/Search");

		ImGui::SameLine();
		drawLevelChips();
		ImGui::SameLine();
		drawCategoryFilter();

		ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
		                         ImGui::GetWindowWidth() - style.WindowPadding.x - rightW));
		if (EditorWidgets::button("Copy")) copyVisible();
		ImGui::SameLine();
		if (EditorWidgets::button("Clear")) clearLines();
	}

	ImGui::Separator();

	// Again, in case the toolbar just changed the filter or the search: the list
	// below has to show the answer to the click that produced it, not the one
	// before it.
	if (s_visibleDirty) rebuildVisible();

	// The footer's height is taken off the list's, so the list is what scrolls
	// and the footer stays where it is.
	// HorizontalScrollbar rather than wrapping: wrapped rows are of different
	// heights, and the clipper below only works because every row is one line
	// tall. A long line scrolls sideways; it is never silently cut.
	if (ImGui::BeginChild("##consoleLines", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
	                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
	{
		// Pinned to the bottom only while the view IS at the bottom: the moment
		// the user scrolls up to read something, new lines stop yanking them back
		// down, and scrolling to the bottom again resumes it. GetScrollMaxY still
		// holds last frame's value here, which is what makes the comparison true
		// exactly while the view was pinned — and "Latest" is the way back.
		s_atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
		if (s_atBottom) s_pinnedShown = s_totalShown;

		// Monospace, and pushed BEFORE the clipper: a log is columns — time,
		// category — and the clipper measures a row from the font that is
		// current when it starts.
		if (hooks.codeFont) ImGui::PushFont(hooks.codeFont);

		if (s_visible.empty())
		{
			if (s_lines.empty())
			{
				ImGui::TextDisabled("Nothing logged yet.");
				ImGui::TextDisabled("Output from the engine, scripts and Print nodes appears here.");
			}
			else
			{
				ImGui::TextDisabled("No lines match the filter.");
				if (EditorWidgets::button("Reset filters"))
				{
					s_search.clear();
					s_levelMask    = kAllLevels;
					s_catHidden    = 0;
					s_visibleDirty = true;
				}
			}
		}
		else
		{
			std::string row;

			// A row is a Selectable with the content painted over it, so the hover,
			// the double-click and the context menu all belong to one item, and the
			// tint and stripe of a warning or an error can be drawn under it.
			// (Painted, not laid out: a second item after the Selectable would
			// hand IsItemHovered to the label instead of the row.)
			const float lineH  = ImGui::GetTextLineHeight();
			const float rowH   = lineH + 2.0f;
			const float pitch  = rowH + style.ItemSpacing.y;
			const float availW = ImGui::GetContentRegionAvail().x;

			// Columns. The category column is as wide as the widest category the
			// buffer has held, so the messages line up without a fixed width that
			// is wrong for "SourceControl" or wasteful for "Core".
			const float timeW = ImGui::CalcTextSize("00:00:00").x;
			float       catW  = 0.0f;
			for (int c = 0; c < static_cast<int>(HE::Log::Cat::Count); ++c)
				if (s_catSeen & (std::uint64_t(1) << c))
					catW = std::max(catW, ImGui::CalcTextSize(
						HE::Log::categoryName(static_cast<HE::Log::Cat>(c))).x);
			constexpr float kGap = 14.0f;
			const float xTime = kPadL;
			const float xCat  = xTime + timeW + kGap;
			const float xMsg  = xCat + catW + kGap;

			const ImU32 textCol = HE::Ed::Theme::u32(HE::Ed::Theme::Text);
			const ImU32 dimCol  = HE::Ed::Theme::u32(HE::Ed::Theme::TextDim);
			const ImU32 linkCol = HE::Ed::Theme::u32(HE::Ed::Theme::AccentHi);
			const ImU32 linkHot = HE::Ed::Theme::u32(HE::Ed::Theme::AccentBright);
			const ImVec4 surface = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
			ImDrawList* dl      = ImGui::GetWindowDrawList();
			const RowLayout lay{availW, lineH, rowH, pitch, xTime, xCat, xMsg,
			                    textCol, dimCol, linkCol, linkHot, surface};

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(s_visible.size()), pitch);
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
						drawRow(s_lines[static_cast<std::size_t>(s_visible[static_cast<std::size_t>(i)])], hooks,
						        lay, style, dl, row, clearRequested);
				}
			}
			clipper.End();
		}

		if (hooks.codeFont) ImGui::PopFont();

		if (s_atBottom || s_jumpToLatest)
		{
			ImGui::SetScrollHereY(1.0f);
			s_jumpToLatest = false;
			s_atBottom     = true;
			s_pinnedShown  = s_totalShown;
		}
	}
	ImGui::EndChild();

	// ── Footer ───────────────────────────────────────────────────────────────
	// How much there is, and — only while the view is scrolled away from the
	// newest line — the way back to it, with what arrived meanwhile.
	{
		char text[64];
		if (s_totalShown == s_totalAll)
			std::snprintf(text, sizeof(text), "%ld line%s", s_totalAll, s_totalAll == 1 ? "" : "s");
		else
			std::snprintf(text, sizeof(text), "%ld of %ld lines", s_totalShown, s_totalAll);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", text);

		if (!s_atBottom)
		{
			const long fresh = std::max(0L, s_totalShown - s_pinnedShown);
			char       label[48];
			if (fresh > 0) std::snprintf(label, sizeof(label), "Latest (%ld new)", fresh);
			else           std::snprintf(label, sizeof(label), "Latest");
			const float w = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
			ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - w);
			if (ImGui::Button(label)) s_jumpToLatest = true;
			EditorWidgets::helpForKey("Console/Latest");
		}
	}

	if (clearRequested) clearLines();
#else
	(void)hooks;
#endif // HE_CONSOLEVIEW_IMGUI
}

Stats stats()
{
	Stats s;
#ifdef HE_CONSOLEVIEW_IMGUI
	pump();
	if (s_visibleDirty) rebuildVisible();
	s.rows  = s_lines.size();
	s.total = s_totalAll;
	s.shown = s_totalShown;
	for (int i = 0; i < kLevelCount; ++i) s.byLevel[i] = s_countByLevel[i];
#endif
	return s;
}

void resetForTest()
{
#ifdef HE_CONSOLEVIEW_IMGUI
	{
		std::lock_guard<std::mutex> lock(s_pendingMutex);
		s_pending.clear();
	}
	s_lines.clear();
	s_visible.clear();
	s_visibleDirty = true;
	s_search.clear();
	s_levelMask    = kAllLevels;
	s_catHidden    = 0;
	s_catSeen      = 0;
	s_selectedId   = 0;
	s_atBottom     = true;
	s_jumpToLatest = false;
	s_pinnedShown  = 0;
#endif
}

} // namespace ConsoleView
