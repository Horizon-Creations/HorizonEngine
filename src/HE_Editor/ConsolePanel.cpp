#include "ConsolePanel.h"
#include "EditorApplication.h"
#include "EditorWidgets.h"       // the help-aware checkbox / button / menu item
#include "EditorHelp.h"          // "Console/<label>" scope for its controls
#include "EditorTheme.h"         // body / dimmed text
#include "EditorToolbar.h"       // the shared "needs attention" / "went wrong" colours

#include "HcExecTrace.h"         // a line's node → "go to node"
#include "LevelScriptPanel.h"    // kTabPath — naming the tab a line leads to
#include "GameInstancePanel.h"   // kTabPath
#include "ScriptEditorPanel.h"   // a line's `script:line:` → "go to line"

#include <Diagnostics/Log.h>
#include <HorizonCode/HorizonCode.h>   // currentExecSite — which node wrote a record
#include <Scripting/ScriptTypes.h>     // parseScriptErrorLocation — which line a Lua/Python error names
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/ScriptComponent.h>   // moduleName → the script asset

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <cfloat>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>   // InputTextWithHint over std::string
#endif

namespace ConsolePanel
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
		std::string  text;
		// The HorizonCode node that was executing when the record was written —
		// a Print's own line, a warning from inside a node, an engine row's
		// error — so the row can lead back to it (HcExecTrace::requestReveal).
		// Empty key / node 0 = written by nothing that was a node.
		std::string  hcKey;
		int          hcNode     = 0;
		uint32_t     hcInstance = 0;
		// The Lua/Python script and line a script error names (`mover:7: …`,
		// HE::parseScriptErrorLocation), so the row can lead to the line
		// (ScriptEditorPanel::requestReveal). Line 0 = names none. The name is
		// the module name the script runs under; the asset behind it is looked
		// up on the click, not here — the sink must stay short and lock-free.
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

		char prefix[96];
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
			std::snprintf(prefix, sizeof(prefix), "%s.%03d  %s  %-11s  ", timeBuf, ms,
			              kLevelTag[levelIndex(record.level)],
			              HE::Log::categoryName(record.category));
		}

		// Where it came from, for the severities where that is the next question.
		// Below Warning it is noise: a Trace line nobody is hunting does not need
		// half its width spent on a filename.
		std::string suffix;
		if (record.file && record.file[0] != '\0' && record.level >= HE::LogLevel::Warning)
		{
			char loc[160];
			std::snprintf(loc, sizeof(loc), "  (%s:%d)", shortFileName(record.file), record.line);
			suffix = loc;
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
			line.text  = prefix;
			line.text.append(at, static_cast<std::size_t>(end - at));
			if (end == msgEnd) line.text += suffix;   // the location belongs to the record, once
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

// ─── The panel ───────────────────────────────────────────────────────────────

#ifdef HE_IMGUI_ENABLED
namespace
{
	// Frame thread only, like every other panel's state.
	std::vector<Line> s_lines;                  // the ring the panel shows
	std::vector<int>  s_visible;                // indices into s_lines that pass the filter
	bool              s_visibleDirty = true;
	std::string       s_search;
	bool              s_autoScroll   = true;
	// Everything on by default: the log's own verbosity has already dropped what
	// the user did not ask for (Trace and Debug are off for most categories), so
	// a console that hides more on top of that would be lying twice.
	unsigned          s_levelMask    = (1u << kLevelCount) - 1u;

	constexpr const char* kLevelName[] = {
		"Trace", "Debug", "Info", "Warning", "Error", "Critical"
	};

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
			// A repeat is counted, not dropped. The notification bell drops one
			// (NotificationStore.h) because a bell that rings four thousand times
			// is a bell nobody reads; a console that drops lines misrepresents
			// what the program did. Counting bounds the same case — one error
			// logged every frame would otherwise flush the whole ring in a minute
			// and leave nothing but itself.
			// Same text from another NODE is not a repeat: two Prints saying
			// "here" are two places, and collapsing them would lose one.
			if (!s_lines.empty() && s_lines.back().level == line.level &&
			    s_lines.back().text == line.text &&
			    s_lines.back().hcNode == line.hcNode && s_lines.back().hcKey == line.hcKey)
			{
				++s_lines.back().repeat;
				continue;
			}
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

	void rebuildVisible()
	{
		s_visible.clear();
		s_visible.reserve(s_lines.size());
		for (int i = 0; i < static_cast<int>(s_lines.size()); ++i)
		{
			const Line& line = s_lines[static_cast<std::size_t>(i)];
			if ((s_levelMask & (1u << levelIndex(line.level))) == 0) continue;
			if (!s_search.empty() && !containsNoCase(line.text, s_search)) continue;
			s_visible.push_back(i);
		}
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
		s_visibleDirty = false;
	}

	void drawLevelFilter()
	{
		int hidden = 0;
		for (int i = 0; i < kLevelCount; ++i)
			if ((s_levelMask & (1u << i)) == 0) ++hidden;

		char label[32];
		if (hidden == 0) std::snprintf(label, sizeof(label), "Levels");
		else             std::snprintf(label, sizeof(label), "Levels (%d hidden)", hidden);

		if (ImGui::Button(label)) ImGui::OpenPopup("##consoleLevels");
		if (!ImGui::BeginPopup("##consoleLevels")) return;

		for (int i = 0; i < kLevelCount; ++i)
		{
			const unsigned bit = 1u << i;
			bool           on  = (s_levelMask & bit) != 0;
			ImGui::PushStyleColor(ImGuiCol_Text, colourFor(static_cast<HE::LogLevel>(i)));
			if (EditorWidgets::checkbox(kLevelName[i], &on))
			{
				s_levelMask    = on ? (s_levelMask | bit) : (s_levelMask & ~bit);
				s_visibleDirty = true;
			}
			ImGui::PopStyleColor();
		}
		ImGui::EndPopup();
	}

	// The asset behind a script's module name, as the FULL path the tab bar
	// keys on; empty when nothing in the world runs under that name. A Lua or
	// Python script has no name of its own in the log — it runs under the
	// moduleName of the ScriptComponent that started it
	// (ScriptContext::startEntityScript), so that component is the way back:
	// its scriptAssetId, the ContentManager's script for it, that script's
	// content-relative path, resolved. Two components may share a module name
	// across languages (ScriptContext.h), so the one whose asset is in the
	// language that logged wins; anything else with the name is the fallback.
	// A component holding a HorizonCode class under the same id has no
	// ScriptAsset and is skipped.
	//
	// The getter's pointer is only good until the next loadAsset, so the path
	// is copied out at once.
	std::string scriptAssetPathFor(AppContext& ctx, const std::string& moduleName, bool python)
	{
		if (!ctx.world || !ctx.contentManager || moduleName.empty()) return {};
		const HE::ScriptLanguage want = python ? HE::ScriptLanguage::Python : HE::ScriptLanguage::Lua;
		std::string fallback;
		for (auto [entity, sc] : ctx.world->registry().view<ScriptComponent>().each())
		{
			if (sc.moduleName != moduleName) continue;
			const ScriptAsset* asset = ctx.contentManager->getScript(sc.scriptAssetId);
			if (!asset || asset->path.empty()) continue;
			const std::string rel = asset->path;
			if (asset->language == want) return ctx.contentManager->resolveAbsolutePath(rel);
			if (fallback.empty()) fallback = ctx.contentManager->resolveAbsolutePath(rel);
		}
		return fallback;
	}
}
#endif // HE_IMGUI_ENABLED

void DrawConsoleWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Console/<its label>".
	HE::Ed::Help::Scope helpScope("Console");
#ifdef HE_IMGUI_ENABLED
	if (!open) return;

	// Drained before Begin, so a window the user rolled up still keeps its
	// history current instead of coming back with a gap in it.
	if (drainPending()) s_visibleDirty = true;
	// Rebuilt before anything can read it: a trim inside the drain shifts every
	// index, and the Copy button below walks this list.
	if (s_visibleDirty) rebuildVisible();

	ImGui::SetNextWindowSize(ImVec2(860.0f, 280.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Console", &open)) { ImGui::End(); return; }

	// Only the context menu inside the row loop defers its clear — see there.
	bool clearRequested = false;

	if (EditorWidgets::button("Clear")) clearLines();
	ImGui::SameLine();
	if (ImGui::Button("Copy")) copyVisible();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copy every line currently shown — the level filter and the search apply.");
	ImGui::SameLine();
	drawLevelFilter();
	ImGui::SameLine();
	EditorWidgets::checkbox("Auto-scroll", &s_autoScroll);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##consoleSearch", "Search", &s_search)) s_visibleDirty = true;

	ImGui::Separator();

	// Again, in case the toolbar just changed the filter or the search: the list
	// below has to show the answer to the click that produced it, not the one
	// before it.
	if (s_visibleDirty) rebuildVisible();

	// HorizontalScrollbar rather than wrapping: wrapped rows are of different
	// heights, and the clipper below only works because every row is one line
	// tall. A long line scrolls sideways; it is never silently cut.
	if (ImGui::BeginChild("##consoleLines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
	                      ImGuiWindowFlags_HorizontalScrollbar))
	{
		// Monospace, and pushed BEFORE the clipper: a log is columns — time,
		// level, category — and the clipper measures a row from the font that is
		// current when it starts.
		if (ctx.codeFont) ImGui::PushFont(ctx.codeFont);

		if (s_visible.empty())
		{
			ImGui::TextDisabled("%s", s_lines.empty() ? "Nothing logged yet."
			                                          : "No lines match the filter.");
		}
		else
		{
			std::string row;

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(s_visible.size()));
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
					const Line& line = s_lines[static_cast<std::size_t>(s_visible[static_cast<std::size_t>(i)])];
					row.clear();
					appendRow(line, row);

					// TextUnformatted and not Selectable: a Selectable's label is
					// also its id, and ImGui hides everything after a "##" in a
					// label — a logged shader name or window id containing one
					// would lose the rest of its line.
					ImGui::PushID(i);
					ImGui::PushStyleColor(ImGuiCol_Text, colourFor(line.level));
					ImGui::TextUnformatted(row.c_str());
					ImGui::PopStyleColor();
					// A line a HorizonCode node wrote leads back to that node: a
					// double-click on the row, or the context entry below. The
					// marker after the text says which node without becoming
					// part of the copied line (appendRow never sees it).
					const bool fromNode = line.hcNode != 0;
					if (fromNode && ImGui::IsItemHovered())
					{
						// The tab's name rather than the runtime's key: "level:<uuid>"
						// tells nobody anything, "Level Script" does.
						const std::string where = HcExecTrace::tabKeyFor(line.hcKey);
						const char* shown = where == LevelScriptPanel::kTabPath  ? "the Level Script"
						                  : where == GameInstancePanel::kTabPath ? "the Game Instance"
						                  : where.c_str();
						ImGui::SetTooltip("Written by node %d of %s\nDouble-click to show it.",
						                  line.hcNode, shown);
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
							HcExecTrace::requestReveal(line.hcKey, line.hcNode);
					}
					// A Lua/Python error that names its line leads to that line of
					// the script: same double-click, same menu, the script tab
					// instead of a graph. The node wins when a row has both (an
					// Engine Call node that ran a script) — the node is where the
					// user was. The asset is looked up on the click: the world can
					// have changed since the line was written, and a script nobody
					// runs any more simply has nowhere to go.
					const bool toLine = !fromNode && line.scriptLine > 0;
					auto goToLine = [&]()
					{
						const std::string path = scriptAssetPathFor(ctx, line.scriptName, line.python);
						if (!path.empty()) ScriptEditorPanel::requestReveal(path, line.scriptLine);
					};
					if (toLine && ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Line %d of script '%s'\nDouble-click to show it.",
						                  line.scriptLine, line.scriptName.c_str());
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) goToLine();
					}
					// The context menu hangs off the row text, so it is opened here,
					// before the marker becomes the last item.
					if (ImGui::BeginPopupContextItem("##consoleLine"))
					{
						if (fromNode)
						{
							if (EditorWidgets::menuItem("Go to Node"))
								HcExecTrace::requestReveal(line.hcKey, line.hcNode);
							ImGui::Separator();
						}
						if (toLine)
						{
							if (EditorWidgets::menuItem("Go to Line")) goToLine();
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
					if (fromNode)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("[node %d]", line.hcNode);
					}
					ImGui::PopID();
				}
			}
			clipper.End();
		}

		if (ctx.codeFont) ImGui::PopFont();

		// Pinned to the bottom only while the view IS at the bottom: the moment
		// the user scrolls up to read something, new lines stop yanking them back
		// down, and scrolling to the bottom again resumes it. GetScrollMaxY still
		// holds last frame's value here, which is what makes the comparison true
		// exactly while the view was pinned.
		if (s_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();

	if (clearRequested) clearLines();

	ImGui::End();
#endif // HE_IMGUI_ENABLED
}

} // namespace ConsolePanel
