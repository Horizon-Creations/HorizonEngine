#include "EditorRewards.h"
#include "EditorApplication.h"   // AppContext, EditorConfig
#include <HorizonScene/AudioEngine.h>

#include <Diagnostics/GlobalState.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iterator>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace HE::Ed::Rewards
{

// ── Core ─────────────────────────────────────────────────────────────────────

std::string lineFor(Moment m, int count)
{
	switch (m)
	{
	case Moment::Saved:          return "Saved";
	case Moment::BuildSucceeded: return "Build succeeded";
	case Moment::AssetsImported:
		return count == 1 ? std::string("Imported 1 asset")
		                  : "Imported " + std::to_string(std::max(count, 0)) + " assets";
	case Moment::CompiledClean:  return "Compiles clean";
	case Moment::Committed:
		if ((count & kSyncCommit) && (count & kSyncPush)) return "Committed and pushed";
		return (count & kSyncPush) ? "Pushed" : "Committed";
	case Moment::TourFinished:   return "Tutorial complete";
	}
	return {};
}

bool momentWanted(const EditorConfig& cfg, Moment m)
{
	switch (m)
	{
	case Moment::CompiledClean: return cfg.RewardsMomentCompile;
	case Moment::Committed:     return cfg.RewardsMomentCommit;
	case Moment::TourFinished:  return cfg.RewardsMomentTutorial;
	default:                    return true;
	}
}

float strengthAt(double age)
{
	if (age <= kHoldSec) return 1.0f;
	const double t = (age - kHoldSec) / kFadeSec;
	if (t >= 1.0) return 0.0f;
	// Smoothstep down: no visible kink where the hold ends or where it lands.
	const double s = t * t * (3.0 - 2.0 * t);
	return static_cast<float>(1.0 - s);
}

float gainFor(float volume)
{
	// !(v > 0) also catches NaN from a hand-edited config.
	const float v = !(volume > 0.0f) ? 0.0f : std::min(volume, 1.0f);
	return v * v;
}

int rankOf(Moment m)
{
	switch (m)
	{
	case Moment::Saved:          return 0;
	case Moment::CompiledClean:  return 1;
	case Moment::AssetsImported: return 2;
	case Moment::BuildSucceeded:
	case Moment::Committed:
	case Moment::TourFinished:   return 3;
	}
	return 0;
}

bool hasTone(Moment m)
{
	switch (m)
	{
	case Moment::Saved:
	case Moment::BuildSucceeded:
	case Moment::AssetsImported: return true;
	// Their tones are topic 140's next step — see "The tones" in the header.
	case Moment::CompiledClean:
	case Moment::Committed:
	case Moment::TourFinished:   return false;
	}
	return false;
}

Tone toneFor(Moment m)
{
	switch (m)
	{
	case Moment::Saved:          return Tone::SaveTick;
	case Moment::BuildSucceeded: return Tone::BuildChime;
	case Moment::AssetsImported: return Tone::ImportPop;
	default:                     break;   // no tone (hasTone false)
	}
	return Tone::SaveTick;
}

bool uiSoundPossible(const EditorConfig& cfg)
{
	return cfg.RewardsEnabled && cfg.RewardsSound && !cfg.EditorSoundsMuted;
}

bool toneWanted(const EditorConfig& cfg, Tone t, bool playing, bool appFocused)
{
	if (!uiSoundPossible(cfg) || !(gainFor(cfg.RewardsVolume) > 0.0f) || playing) return false;
	switch (t)
	{
	case Tone::SaveTick:    return cfg.RewardsSoundSave;
	case Tone::ImportPop:   return cfg.RewardsSoundImport;
	case Tone::BuildChime:  return cfg.RewardsSoundBuild && !appFocused;
	case Tone::BuildFailed: return cfg.RewardsSoundBuildFailed && !appFocused;
	}
	return false;
}

Feed::Taken Feed::take(Moment m, int count, double now, int frame, bool soundWanted)
{
	Taken r;
	// frame < 0: the caller has no frame clock (no ImGui context) — nothing to
	// fold against, every push counts.
	if (frame >= 0 && frame == m_lastFrame) return r;
	m_lastFrame = frame;
	r.taken = true;

	const bool showing = look(now).active;
	if (showing && m == m_moment)
	{
		// Rule 2: the same kind again — one line, counted up, held anew, silent.
		// Committed's count is flags: a push after the commit is "and pushed".
		if (m == Moment::Committed) m_count |= count;
		else                        m_count += std::max(count, 0);
		m_at     = now;
		r.shown  = true;
		return r;
	}
	// Rule 3: a lower moment leaves a higher line alone (the tally has it).
	// Equal rank, other kind, falls through: the newer one replaces the line.
	if (showing && rankOf(m) < rankOf(m_moment)) return r;

	m_has    = true;
	m_moment = m;
	m_count  = count;
	m_at     = now;
	m_since  = now;
	r.shown  = true;

	// A moment without a tone books no gap (soundWanted is false for it from
	// fire(); checked here too, so no caller can make it borrow one).
	r.sound = soundWanted && hasTone(m) && takeTone(toneFor(m), now);
	return r;
}

void SyncWatch::requested(int flags)
{
	m_flags |= flags & (kSyncCommit | kSyncPush);
}

int SyncWatch::poll(bool idleBeforePump, const std::string& lastError, const std::string& lastInfo)
{
	if (m_flags == 0 || !idleBeforePump) return 0;
	const int flags = m_flags;
	m_flags = 0;
	// A failed operation sets lastError and clears lastInfo; a status refresh
	// queued behind it can clear lastError again, but never sets lastInfo —
	// so both are asked. Known edge: a FETCH queued behind a failed commit
	// (only the auto-fetch timer can; the buttons are disabled while busy)
	// sets lastInfo again and would read as success.
	if (!lastError.empty() || lastInfo.empty()) return 0;
	return flags;
}

bool Feed::takeTone(Tone t, double now)
{
	// Rules 4 and 5: the gaps run from the last tone that played.
	const bool save = t == Tone::SaveTick;
	if (m_toned && now - m_toneAt < kToneGapSec) return false;
	if (save && m_saveToned && now - m_saveToneAt < kSaveToneGapSec) return false;
	m_toned  = true;
	m_toneAt = now;
	if (save)
	{
		m_saveToned  = true;
		m_saveToneAt = now;
	}
	return true;
}

Feed::BuildEnd Feed::buildEnded(unsigned long long run, bool finished, bool success)
{
	if (!finished || run == 0 || run == m_lastRun) return BuildEnd::None;
	m_lastRun = run;
	return success ? BuildEnd::Succeeded : BuildEnd::Failed;
}

Feed::Look Feed::look(double now) const
{
	Look lk;
	if (!m_has) return lk;
	const double age = now - m_at;
	if (age < 0.0 || age >= kHoldSec + kFadeSec) return lk;
	lk.active   = true;
	lk.line     = lineFor(m_moment, m_count);
	lk.strength = strengthAt(age);
	lk.bar      = static_cast<float>(1.0 - age / (kHoldSec + kFadeSec));
	lk.lineAge  = std::max(0.0, now - m_since);
	return lk;
}

// ── Visual cues ──────────────────────────────────────────────────────────────

namespace
{
double smooth01(double t)
{
	t = std::clamp(t, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}
} // namespace

float checkStroke(double age, bool reduced)
{
	if (reduced) return 1.0f;
	if (!(age > 0.0)) return 0.0f;
	// Eased out: the pen starts quick and settles into the end of the stroke.
	const double t = std::min(1.0, age / kCheckDrawSec);
	return static_cast<float>(1.0 - (1.0 - t) * (1.0 - t));
}

std::vector<Pt> checkPolyline(float stroke, float x, float y, float size)
{
	// The corners in the unit box, y down: in at the left, down to the knee,
	// up to the top right. The knee sits left of centre, as a hand writes it.
	constexpr float kPts[3][2] = { { 0.08f, 0.55f }, { 0.38f, 0.84f }, { 0.94f, 0.18f } };
	std::vector<Pt> out;
	if (!(stroke > 0.0f) || !(size > 0.0f)) return out;
	stroke = std::min(stroke, 1.0f);
	Pt p[3];
	for (int i = 0; i < 3; ++i) p[i] = { x + kPts[i][0] * size, y + kPts[i][1] * size };
	const float l1 = std::hypot(p[1].x - p[0].x, p[1].y - p[0].y);
	const float l2 = std::hypot(p[2].x - p[1].x, p[2].y - p[1].y);
	float left = stroke * (l1 + l2);
	out.push_back(p[0]);
	for (int s = 0; s < 2; ++s)
	{
		const Pt   a   = p[s], b = p[s + 1];
		const float len = s == 0 ? l1 : l2;
		if (left >= len) { out.push_back(b); left -= len; continue; }
		const float f = left / len;
		out.push_back({ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f });
		break;
	}
	return out;
}

Edge edgeAt(double age, bool reduced)
{
	Edge e;
	if (reduced || age < 0.0 || age >= kEdgeSec) return e;
	const double t = age / kEdgeSec;
	e.spread = static_cast<float>(1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t));
	// Up quick over the first sixth, then down: one soft pulse, well below
	// full strength — a 1-px line does not need more to be seen.
	const double up = std::min(1.0, t * 6.0);
	e.alpha = static_cast<float>(0.55 * up * (1.0 - smooth01(t)));
	return e;
}

void CounterTick::observe(int value)
{
	if (!m_known)
	{
		m_known = true;
		m_from = m_to = value;
		return;
	}
	if (value > m_to)
	{
		// A rise on top of a pending one keeps the number the screen last
		// showed as where the roll starts.
		if (!m_pending) m_from = m_to;
		m_to      = value;
		m_pending = true;
		m_at      = -1.0;
	}
	else if (value < m_to)
	{
		m_from = m_to = value;
		m_pending = false;
		m_at      = -1.0;
	}
}

void CounterTick::start(double now)
{
	if (!m_pending) return;
	m_pending = false;
	m_at      = now;
}

CounterTick::Look CounterTick::look(double now, bool reduced) const
{
	Look lk;
	lk.shown = m_pending ? m_from : m_to;
	lk.old   = m_from;
	if (m_pending || m_at < 0.0) return lk;
	const double age = now - m_at;
	if (age < 0.0 || age >= kTickSec) return lk;
	lk.glow = static_cast<float>(1.0 - smooth01(age / kTickSec));
	if (!reduced && m_from != m_to)
		lk.roll = static_cast<float>(smooth01(age / kRollSec));
	return lk;
}

double SaveMarks::update(const std::string& key, bool dirty, double now, double lastSaveAt)
{
	auto it = std::find_if(m_tabs.begin(), m_tabs.end(),
	                       [&](const auto& t) { return t.first == key; });
	if (it == m_tabs.end())
	{
		// First sight of a tab: whatever it is now is where it starts.
		m_tabs.push_back({ key, Tab{ dirty, -1.0 } });
		return -1.0;
	}
	Tab& t = it->second;
	if (dirty)
		t.checkAt = -1.0;   // edited again: the marker is back at once
	else if (t.dirty && lastSaveAt >= 0.0 && now - lastSaveAt <= kSaveMatchSec
	         && now >= lastSaveAt)
		t.checkAt = now;
	t.dirty = dirty;
	if (t.checkAt < 0.0) return -1.0;
	const double age = now - t.checkAt;
	if (age < 0.0 || age >= kTabCheckSec)
	{
		t.checkAt = -1.0;
		return -1.0;
	}
	return age;
}

std::string normalPath(const std::string& p)
{
	return std::filesystem::path(p).lexically_normal().generic_string();
}

DirSnapshot snapshotDir(const std::string& dir)
{
	DirSnapshot s;
	s.dir = dir;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code fe;
		if (!it->is_regular_file(fe)) continue;
		const auto t = it->last_write_time(fe);
		if (fe) continue;
		s.files.emplace_back(it->path().filename().string(),
		                     static_cast<long long>(t.time_since_epoch().count()));
	}
	return s;
}

std::vector<std::string> changedSince(const DirSnapshot& before)
{
	std::vector<std::string> out;
	if (before.dir.empty()) return out;
	for (const auto& [name, at] : snapshotDir(before.dir).files)
	{
		const auto was = std::find_if(before.files.begin(), before.files.end(),
		                              [&](const auto& f) { return f.first == name; });
		if (was != before.files.end() && was->second == at) continue;
		out.push_back(normalPath((std::filesystem::path(before.dir) / name).string()));
	}
	return out;
}

void FreshImports::prune(double now)
{
	m_marks.erase(std::remove_if(m_marks.begin(), m_marks.end(),
	                             [&](const Mark& m)
	                             { return m.seenAt < 0.0 && now - m.markedAt > kImportWindowSec; }),
	              m_marks.end());
}

void FreshImports::mark(const std::vector<std::string>& paths, double now)
{
	// Forget what never came on screen in time, then (re)mark: an asset
	// imported twice gets its frame from the second import.
	prune(now);
	for (const std::string& p : paths)
	{
		auto it = std::find_if(m_marks.begin(), m_marks.end(),
		                       [&](const Mark& m) { return m.path == p; });
		if (it == m_marks.end()) m_marks.push_back({ p, now, -1.0 });
		else                     *it = { p, now, -1.0 };
	}
}

float FreshImports::strength(const std::string& path, double now)
{
	auto it = std::find_if(m_marks.begin(), m_marks.end(),
	                       [&](const Mark& m) { return m.path == path; });
	if (it == m_marks.end()) return 0.0f;
	if (it->seenAt < 0.0)
	{
		if (now - it->markedAt > kImportWindowSec) { m_marks.erase(it); return 0.0f; }
		it->seenAt = now;
	}
	const double age = now - it->seenAt;
	if (age >= kImportHoldSec + kImportFadeSec) { m_marks.erase(it); return 0.0f; }
	if (age <= kImportHoldSec) return 1.0f;
	return static_cast<float>(1.0 - smooth01((age - kImportHoldSec) / kImportFadeSec));
}

namespace
{
struct Ymd { int y = 0, m = 0, d = 0; };

int daysIn(int y, int m)
{
	static constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	return (m == 2 && leap) ? 29 : kDays[m - 1];
}

// Strict YYYY-MM-DD: exactly ten characters, a real calendar date.
bool parseDay(const std::string& s, Ymd& out)
{
	if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
	for (int i : { 0, 1, 2, 3, 5, 6, 8, 9 })
		if (s[i] < '0' || s[i] > '9') return false;
	const auto num = [&](int at, int len) { return std::stoi(s.substr(at, len)); };
	Ymd v{ num(0, 4), num(5, 2), num(8, 2) };
	if (v.y < 1 || v.m < 1 || v.m > 12 || v.d < 1 || v.d > daysIn(v.y, v.m)) return false;
	out = v;
	return true;
}

std::string formatDay(const Ymd& v)
{
	char buf[16];
	std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", v.y, v.m, v.d);
	return buf;
}
} // namespace

std::string dayBefore(const std::string& ymd)
{
	Ymd v;
	if (!parseDay(ymd, v)) return {};
	if (--v.d < 1)
	{
		if (--v.m < 1) { v.m = 12; --v.y; }
		if (v.y < 1) return {};
		v.d = daysIn(v.y, v.m);
	}
	return formatDay(v);
}

namespace
{
// recent's entry for `day`, appended (and the oldest dropped) if missing.
DayUse& recentEntry(std::vector<DayUse>& recent, const std::string& day)
{
	if (recent.empty() || recent.back().day != day)
	{
		recent.push_back({ day, 0 });
		if (recent.size() > static_cast<size_t>(kRecentDays))
			recent.erase(recent.begin(), recent.end() - kRecentDays);
	}
	return recent.back();
}
} // namespace

bool recordMoment(Tally& t, const std::string& today, Moment m, int count)
{
	const bool build  = m == Moment::BuildSucceeded;
	const bool commit = m == Moment::Committed && (count & kSyncCommit);
	Ymd now;
	if (!parseDay(today, now)) return false;
	if (t.day == today)
	{
		if (!build && !commit) return false;   // the day is already counted
		if (build)  ++t.buildsToday;
		if (commit) ++t.commitsToday;
		DayUse& e = recentEntry(t.recent, today);
		e.builds  = t.buildsToday;
		e.commits = t.commitsToday;
		return true;
	}
	// The clock is behind the stored day. YYYY-MM-DD orders as text.
	Ymd stored;
	if (parseDay(t.day, stored) && today < t.day) return false;

	t.streakDays   = (t.day == dayBefore(today) && t.streakDays > 0) ? t.streakDays + 1 : 1;
	t.day          = today;
	t.buildsToday  = build ? 1 : 0;
	t.commitsToday = commit ? 1 : 0;
	DayUse& e = recentEntry(t.recent, today);
	e.builds  = t.buildsToday;
	e.commits = t.commitsToday;
	return true;
}

bool recordUse(Tally& t, const std::string& today, bool build)
{
	return recordMoment(t, today, build ? Moment::BuildSucceeded : Moment::Saved);
}

std::string commitsPhrase(int commits)
{
	if (commits <= 0) return {};
	return commits == 1 ? std::string("1 commit today")
	                    : std::to_string(commits) + " commits today";
}

std::string formatRecent(const std::vector<DayUse>& recent)
{
	std::string s;
	for (const DayUse& d : recent)
	{
		if (!s.empty()) s += ',';
		s += d.day + ':' + std::to_string(std::max(d.builds, 0));
		// Only where there were commits: an editor from before the field
		// drops an entry it cannot parse, so the field costs it nothing else.
		if (d.commits > 0) s += ':' + std::to_string(d.commits);
	}
	return s;
}

std::vector<DayUse> parseRecent(const std::string& text)
{
	std::vector<DayUse> out;
	size_t at = 0;
	while (at <= text.size())
	{
		const size_t comma = std::min(text.find(',', at), text.size());
		const std::string item = text.substr(at, comma - at);
		at = comma + 1;
		// "YYYY-MM-DD:n" or "YYYY-MM-DD:n:c" — anything else (a hand edit) is
		// skipped, not guessed.
		Ymd v;
		if (item.size() < 12 || item[10] != ':' || !parseDay(item.substr(0, 10), v)) continue;
		const std::string rest  = item.substr(11);
		const size_t      colon = rest.find(':');
		const std::string n     = rest.substr(0, colon);
		const std::string c     = colon == std::string::npos ? std::string("0") : rest.substr(colon + 1);
		const auto number = [](const std::string& s)
		{
			return !s.empty() && s.size() <= 6
			    && std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
		};
		if (!number(n) || !number(c)) continue;
		// Oldest first and one entry per day, whatever the file says.
		const std::string day = item.substr(0, 10);
		if (!out.empty() && day <= out.back().day) continue;
		out.push_back({ day, std::stoi(n), std::stoi(c) });
	}
	if (out.size() > static_cast<size_t>(kRecentDays))
		out.erase(out.begin(), out.end() - kRecentDays);
	return out;
}

void seedRecent(Tally& t)
{
	Ymd v;
	if (!t.recent.empty() || !parseDay(t.day, v)) return;
	const int days = std::clamp(t.streakDays, 1, kRecentDays);
	std::string d = t.day;
	for (int i = 0; i < days && !d.empty(); ++i)
	{
		t.recent.insert(t.recent.begin(),
		                DayUse{ d, i == 0 ? std::max(t.buildsToday, 0) : 0,
		                        i == 0 ? std::max(t.commitsToday, 0) : 0 });
		d = dayBefore(d);
	}
}

int weekdayOf(const std::string& ymd)
{
	Ymd v;
	if (!parseDay(ymd, v)) return -1;
	// Sakamoto: 0 = Sunday. Shifted so the week starts on Monday.
	static constexpr int kT[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	const int y   = v.m < 3 ? v.y - 1 : v.y;
	const int dow = (y + y / 4 - y / 100 + y / 400 + kT[v.m - 1] + v.d) % 7;
	return (dow + 6) % 7;
}

std::vector<DayCell> recentDays(const Tally& t, const std::string& today, int n)
{
	std::vector<DayCell> out;
	Ymd v;
	if (n <= 0 || !parseDay(today, v)) return out;
	std::string d = today;
	for (int i = 0; i < n && !d.empty(); ++i)
	{
		DayCell c;
		c.day     = d;
		c.weekday = weekdayOf(d);
		for (const DayUse& u : t.recent)
			if (u.day == d)
			{
				c.used    = true;
				c.builds  = std::max(u.builds, 0);
				c.commits = std::max(u.commits, 0);
			}
		// The tally's own day is used whatever the history says (a tally
		// seeded before its first write, a hand-edited list).
		if (d == t.day)
		{
			c.used    = true;
			c.builds  = std::max(t.buildsToday, 0);
			c.commits = std::max(t.commitsToday, 0);
		}
		out.insert(out.begin(), c);
		d = dayBefore(d);
	}
	return out;
}

std::string buildsPhrase(int builds)
{
	return builds == 1 ? std::string("1 build today")
	                   : std::to_string(std::max(builds, 0)) + " builds today";
}

std::string streakPhrase(int days)
{
	return std::to_string(days) + " days in a row";
}

Progress progressOf(const Tally& t, const std::string& today)
{
	Progress p;
	Ymd stored;
	if (!parseDay(t.day, stored)) return p;
	const bool isToday = t.day == today;
	const bool alive   = isToday || t.day == dayBefore(today);
	p.any    = true;
	p.builds = isToday ? std::max(t.buildsToday, 0) : 0;
	p.streak = (alive && t.streakDays >= 2) ? t.streakDays : 0;
	return p;
}

std::string progressText(const Tally& t, const std::string& today)
{
	const Progress p = progressOf(t, today);
	if (!p.any) return {};
	std::string s = buildsPhrase(p.builds);
	if (p.streak > 0) s += " · " + streakPhrase(p.streak);
	return s;
}

std::string localDay()
{
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	return formatDay({ tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday });
}

namespace
{
// Own constant: M_PI needs _USE_MATH_DEFINES on MSVC.
constexpr double kTwoPi = 6.283185307179586;

void putSample(std::vector<uint8_t>& out, size_t i, double v)
{
	const int s = static_cast<int>(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
	out[i * 2]     = static_cast<uint8_t>(s & 0xFF);
	out[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
}

// v scaled so its loudest sample sits exactly at `peak`, after the last
// tailSec were faded to land on exactly zero — so the level of a tone is a
// number here and not a side effect of how its parts happen to add up.
std::vector<uint8_t> toPcm16(std::vector<double> v, int sampleRate, double peak, double tailSec)
{
	const size_t n          = v.size();
	const double tailFrames = std::max(1.0, tailSec * sampleRate);
	double       loudest    = 0.0;
	for (size_t i = 0; i < n; ++i)
	{
		v[i] *= std::min(1.0, static_cast<double>(n - 1 - i) / tailFrames);
		loudest = std::max(loudest, std::abs(v[i]));
	}
	const double k = loudest > 0.0 ? peak / loudest : 0.0;
	std::vector<uint8_t> out(n * 2);
	for (size_t i = 0; i < n; ++i) putSample(out, i, v[i] * k);
	return out;
}

// Struck notes: each rises over attackSec and rings out with e-folding time
// decaySec. What the chime is made of, and the failed-build tone.
struct Note { double hz, startSec, level; };
std::vector<double> ringNotes(const Note* notes, size_t count, int sampleRate,
                              double lengthSec, double attackSec, double decaySec)
{
	std::vector<double> v(static_cast<size_t>(lengthSec * sampleRate));
	for (size_t i = 0; i < v.size(); ++i)
	{
		const double t = static_cast<double>(i) / sampleRate;
		for (size_t k = 0; k < count; ++k)
		{
			const double lt = t - notes[k].startSec;
			if (lt < 0.0) continue;
			const double env = std::min(1.0, lt / attackSec) * std::exp(-lt / decaySec);
			v[i] += std::sin(kTwoPi * notes[k].hz * lt) * env * notes[k].level;
		}
	}
	return v;
}
} // namespace

std::vector<uint8_t> saveTickPcm16(int sampleRate)
{
	// S1: E6 struck and damped almost at once, with a breath of band-passed
	// noise on the attack — a switch's click in the tones' key, not a note.
	// The quietest tone: it is the one heard most often.
	constexpr double kLengthSec  = 0.06;
	constexpr double kAttackSec  = 0.004;
	constexpr double kToneHz     = 1318.51;   // E6
	constexpr double kToneDecay  = 0.012;
	constexpr double kNoiseDecay = 0.005;
	constexpr double kNoiseLevel = 0.35;      // against the tone, both at unit peak
	constexpr double kPeak       = 0.16;      // ≈ −16 dBFS
	if (sampleRate <= 0) return {};

	const size_t n = static_cast<size_t>(kLengthSec * sampleRate);
	std::vector<double> tone(n), noise(n);
	// The noise band-passed to about 1.2–3 kHz: the difference of two one-pole
	// low-passes, so nothing of it reaches the bass. A fixed seed: the tick is
	// the same every time, and the tests can pin it.
	const double aHi = 1.0 - std::exp(-kTwoPi * 3000.0 / sampleRate);
	const double aLo = 1.0 - std::exp(-kTwoPi * 1200.0 / sampleRate);
	uint32_t     rng = 0x9E3779B9u;
	double       lpHi = 0.0, lpLo = 0.0, toneMax = 0.0, noiseMax = 0.0;
	for (size_t i = 0; i < n; ++i)
	{
		const double t   = static_cast<double>(i) / sampleRate;
		const double rise = std::min(1.0, t / kAttackSec);
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		const double white = static_cast<double>(rng) / 4294967295.0 * 2.0 - 1.0;
		lpHi += aHi * (white - lpHi);
		lpLo += aLo * (white - lpLo);
		tone[i]  = std::sin(kTwoPi * kToneHz * t) * rise * std::exp(-t / kToneDecay);
		noise[i] = (lpHi - lpLo) * rise * std::exp(-t / kNoiseDecay);
		toneMax  = std::max(toneMax, std::abs(tone[i]));
		noiseMax = std::max(noiseMax, std::abs(noise[i]));
	}
	for (size_t i = 0; i < n; ++i)
		tone[i] = tone[i] / std::max(toneMax, 1e-9) + kNoiseLevel * noise[i] / std::max(noiseMax, 1e-9);
	return toPcm16(std::move(tone), sampleRate, kPeak, 0.01);
}

std::vector<uint8_t> importPopPcm16(int sampleRate)
{
	// I1: a sine that drops from 1.4 to 0.9 kHz in 70 ms — "something landed".
	// The glide is exponential (even in pitch) and integrated as phase: sin of
	// f(t)·t would sweep twice as far as it says.
	constexpr double kLengthSec = 0.10;
	constexpr double kAttackSec = 0.004;
	constexpr double kFromHz    = 1400.0;
	constexpr double kToHz      = 900.0;
	constexpr double kGlideSec  = 0.07;
	constexpr double kDecaySec  = 0.03;
	constexpr double kPeak      = 0.22;       // ≈ −13 dBFS
	if (sampleRate <= 0) return {};

	std::vector<double> v(static_cast<size_t>(kLengthSec * sampleRate));
	double phase = 0.0;
	for (size_t i = 0; i < v.size(); ++i)
	{
		const double t  = static_cast<double>(i) / sampleRate;
		const double hz = kFromHz * std::pow(kToHz / kFromHz, std::min(t, kGlideSec) / kGlideSec);
		v[i]   = std::sin(phase) * std::min(1.0, t / kAttackSec) * std::exp(-t / kDecaySec);
		phase += kTwoPi * hz / sampleRate;
	}
	return toPcm16(std::move(v), sampleRate, kPeak, 0.015);
}

std::vector<uint8_t> buildFailedPcm16(int sampleRate)
{
	// E6 then B5, a fourth DOWN — the chime turned downwards, on a note that
	// does not resolve: "look at the build", not "wrong". A softer attack and
	// a slower step than the chime, and quieter than it. No buzz, no low note.
	constexpr Note   kNotes[]   = { { 1318.51, 0.0, 1.0 }, { 987.77, 0.11, 0.85 } };
	constexpr double kLengthSec = 0.45;
	constexpr double kPeak      = 0.25;       // ≈ −12 dBFS
	if (sampleRate <= 0) return {};
	return toPcm16(ringNotes(kNotes, std::size(kNotes), sampleRate, kLengthSec, 0.008, 0.10),
	               sampleRate, kPeak, 0.03);
}

std::vector<uint8_t> tonePcm16(Tone t, int sampleRate)
{
	switch (t)
	{
	case Tone::SaveTick:    return saveTickPcm16(sampleRate);
	case Tone::BuildChime:  return chimePcm16(sampleRate);
	case Tone::BuildFailed: return buildFailedPcm16(sampleRate);
	case Tone::ImportPop:   return importPopPcm16(sampleRate);
	}
	return {};
}

std::vector<uint8_t> chimePcm16(int sampleRate)
{
	// A5 then E6 70 ms later, each struck and left to ring out. Quiet on
	// purpose — the peak stays well below full scale even where both overlap.
	// Topic 75's numbers, unchanged: kPeak scales the sum, it is not
	// normalised like the newer tones.
	constexpr Note   kNotes[]   = { { 880.0, 0.0, 1.0 }, { 1318.51, 0.07, 1.0 } };
	constexpr double kLengthSec = 0.42;
	constexpr double kAttackSec = 0.004;   // a ramp in, or the first sample clicks
	constexpr double kDecaySec  = 0.09;    // e-folding time of each note
	constexpr double kPeak      = 0.30;

	if (sampleRate <= 0) return {};
	const std::vector<double> v =
		ringNotes(kNotes, std::size(kNotes), sampleRate, kLengthSec, kAttackSec, kDecaySec);
	std::vector<uint8_t> out(v.size() * 2);
	for (size_t i = 0; i < v.size(); ++i)
	{
		// The tail fades to exactly zero at the end, so the voice stops silent.
		const double t    = static_cast<double>(i) / sampleRate;
		const double tail = std::min(1.0, (kLengthSec - t) / 0.02);
		putSample(out, i, v[i] * kPeak * tail);
	}
	return out;
}

// ── Editor side ──────────────────────────────────────────────────────────────

namespace
{
Feed  s_feed;
Tally s_tally;
bool  s_tallyLoaded = false;

void loadTally(AppContext& ctx)
{
	if (s_tallyLoaded) return;
	s_tallyLoaded = true;
	if (!ctx.globalState) return;
	const GlobalState& gs = *ctx.globalState;
	s_tally.day         = gs.getCustomConfigString("RewardsDay", "");
	s_tally.buildsToday = std::max(0, gs.getCustomConfigInt("RewardsBuildsToday", 0));
	s_tally.streakDays  = std::max(0, gs.getCustomConfigInt("RewardsStreakDays", 0));
	s_tally.commitsToday = std::max(0, gs.getCustomConfigInt("RewardsCommitsToday", 0));
	s_tally.recent      = parseRecent(gs.getCustomConfigString("RewardsRecent", ""));
	seedRecent(s_tally);   // a tally from before the history: its streak
}

// Before fire()'s once-per-frame fold — see "Rules" in the header.
void tallyMoment(AppContext& ctx, Moment m, int count)
{
	loadTally(ctx);
	if (!recordMoment(s_tally, localDay(), m, count) || !ctx.globalState) return;
	ctx.globalState->setCustomConfigEntry("RewardsDay",          s_tally.day);
	ctx.globalState->setCustomConfigEntry("RewardsBuildsToday",  s_tally.buildsToday);
	ctx.globalState->setCustomConfigEntry("RewardsStreakDays",   s_tally.streakDays);
	ctx.globalState->setCustomConfigEntry("RewardsCommitsToday", s_tally.commitsToday);
	ctx.globalState->setCustomConfigEntry("RewardsRecent",       formatRecent(s_tally.recent));
	ctx.globalState->writeConfig();
}

// post()'s queue, fired by the next pollBuild.
std::vector<std::pair<Moment, int>> s_posted;

// V3's two counters, V4's tabs and when the last Saved moment fired, V5's
// fresh files — see "The visual cues" in the header.
CounterTick  s_buildsTick;
CounterTick  s_streakTick;
SaveMarks    s_saveMarks;
double       s_lastSaveAt = -1.0;
FreshImports s_fresh;

// The system's reduce-motion answer, asked at most once a second.
bool (*s_motionQuery)() = nullptr;
bool  s_motionCached    = false;
bool  s_motionAsked     = false;
std::chrono::steady_clock::time_point s_motionAt;

bool s_appFocused   = true;    // the last pollBuild's word; focused = no build tone
bool s_uiAudioFailed = false;  // its device would not open — see keepUiAudio

// The UI-sound engine ("Routing" in the header): open while a tone is
// possible, closed while none is. Called every frame from pollBuild, so the
// device opens in a frame of its own and never in the frame of a save.
void keepUiAudio(AppContext& ctx)
{
	AudioEngine* a = ctx.uiAudioEngine;
	if (!a) return;
	if (!uiSoundPossible(ctx.editorConfig))
	{
		s_uiAudioFailed = false;   // switching sound back on tries again
		a->shutdown();             // no-op when it is not open
		return;
	}
	// AudioEngine::init logs its own failure; once is enough.
	if (!a->isInitialized() && !s_uiAudioFailed && !a->init())
		s_uiAudioFailed = true;
}

void playTone(AppContext& ctx, Tone t, float gain)
{
	AudioEngine* a = ctx.uiAudioEngine;
	if (!a || !a->isInitialized() || !(gain > 0.0f)) return;
	constexpr int kRate = 44100;
	// Indexed by Tone.
	static const std::vector<uint8_t> pcm[] = {
		tonePcm16(Tone::SaveTick, kRate),    tonePcm16(Tone::BuildChime, kRate),
		tonePcm16(Tone::BuildFailed, kRate), tonePcm16(Tone::ImportPop, kRate),
	};
	a->play(pcm[static_cast<int>(t)], kRate, 1, gain);
}

double nowSec()
{
#ifdef HE_IMGUI_ENABLED
	if (ImGui::GetCurrentContext()) return ImGui::GetTime();
#endif
	return 0.0;
}

int frameNo()
{
#ifdef HE_IMGUI_ENABLED
	if (ImGui::GetCurrentContext()) return ImGui::GetFrameCount();
#endif
	return -1;
}
} // namespace

void fire(AppContext& ctx, Moment m, int count)
{
	const EditorConfig& cfg = ctx.editorConfig;
	if (!cfg.RewardsEnabled) return;
	tallyMoment(ctx, m, count);
	// V4 matches a tab's dirty → clean against this — before the Feed's
	// once-per-frame fold, so a save that shares its frame still marks its tab.
	if (m == Moment::Saved) s_lastSaveAt = nowSec();
	// The moment's own switch (4–6): counted above, neither shown nor heard.
	if (!momentWanted(cfg, m)) return;
	// Rule 6: the switches say whether this moment may sound at all; the Feed
	// says whether it does. Visual off still goes through the Feed, so what
	// is heard follows the same merging and rank either way. hasTone first: a
	// moment without a tone of its own must not ask another tone's switch.
	const Tone tone        = toneFor(m);
	const bool soundWanted = hasTone(m) && toneWanted(cfg, tone, ctx.isPlaying, s_appFocused);
	if (s_feed.take(m, count, nowSec(), frameNo(), soundWanted).sound)
		playTone(ctx, tone, gainFor(cfg.RewardsVolume));
}

void post(Moment m, int count)
{
	// A handful a frame at most (one per click); a cap so a caller in a loop
	// cannot grow it without bound before the next pollBuild.
	if (s_posted.size() < 16) s_posted.emplace_back(m, count);
}

void preview(AppContext& ctx, Tone t)
{
	// The button is a click, not a save: opening the device in this frame (if
	// Sound was switched on in this very frame, before pollBuild saw it) is
	// fine here. keepUiAudio also answers "muted" by leaving it closed.
	keepUiAudio(ctx);
	playTone(ctx, t, gainFor(ctx.editorConfig.RewardsVolume));
}

bool systemReducesMotion()
{
	if (!s_motionQuery) return false;
	// Asked every footer frame; the answer changes when someone opens System
	// Settings, so once a second is plenty and keeps the Cocoa/Win32 call off
	// the per-frame path.
	const auto now = std::chrono::steady_clock::now();
	if (!s_motionAsked || now - s_motionAt >= std::chrono::seconds(1))
	{
		s_motionCached = s_motionQuery();
		s_motionAsked  = true;
		s_motionAt     = now;
	}
	return s_motionCached;
}

void setSystemMotionQuery(bool (*query)())
{
	s_motionQuery = query;
	s_motionAsked = false;
}

TabMark tabMark(AppContext& ctx, const std::string& key, bool dirty)
{
	TabMark tm;
	const double age = s_saveMarks.update(key, dirty, nowSec(), s_lastSaveAt);
	const EditorConfig& cfg = ctx.editorConfig;
	// The edges are tracked whatever the switches say, so switching V4 on
	// never shows a check for a save from before.
	if (age < 0.0 || !cfg.RewardsEnabled || !cfg.RewardsTabCheck) return tm;
	tm.check  = true;
	tm.stroke = checkStroke(age, reducedMotion(ctx));
	// Full for the first half, then out.
	const double t = (age - kTabCheckSec * 0.5) / (kTabCheckSec * 0.5);
	tm.alpha = static_cast<float>(1.0 - smooth01(t));
	return tm;
}

DirSnapshot importSnapshot(const AppContext& ctx, const std::string& dir)
{
	const EditorConfig& cfg = ctx.editorConfig;
	if (!cfg.RewardsEnabled || !cfg.RewardsImportHighlight || dir.empty()) return {};
	return snapshotDir(dir);
}

void markImported(AppContext&, const DirSnapshot& before)
{
	if (before.dir.empty()) return;   // taken while the highlight was off
	s_fresh.mark(changedSince(before), nowSec());
}

float importHighlight(AppContext& ctx, const std::string& fullPath)
{
	if (s_fresh.empty()) return 0.0f;   // the common case: one branch per tile
	const EditorConfig& cfg = ctx.editorConfig;
	if (!cfg.RewardsEnabled || !cfg.RewardsImportHighlight) return 0.0f;
	const double now = nowSec();
	s_fresh.prune(now);                 // a handful of marks at most
	if (s_fresh.empty()) return 0.0f;
	return s_fresh.strength(normalPath(fullPath), now);
}

bool reducedMotion(const AppContext& ctx)
{
	return ctx.editorConfig.RewardsReducedMotion == 0 && systemReducesMotion();
}

void pollBuild(AppContext& ctx, unsigned long long run, bool finished, bool success,
               bool appFocused)
{
	s_appFocused = appFocused;
	keepUiAudio(ctx);
	// post()'s moments from the last frame, in order. Swapped out first: fire()
	// never posts, but nothing here should depend on that.
	if (!s_posted.empty())
	{
		std::vector<std::pair<Moment, int>> posted;
		posted.swap(s_posted);
		for (const auto& [m, count] : posted) fire(ctx, m, count);
	}
	// Consumed whether or not the feature is on: switching it on later must not
	// reward a build that finished while it was off.
	switch (s_feed.buildEnded(run, finished, success))
	{
	case Feed::BuildEnd::Succeeded:
		fire(ctx, Moment::BuildSucceeded);
		break;
	case Feed::BuildEnd::Failed:
	{
		// Not a moment (no line, nothing counted) — only its tone, under the
		// same switches and the same gap as every other.
		const EditorConfig& cfg = ctx.editorConfig;
		if (toneWanted(cfg, Tone::BuildFailed, ctx.isPlaying, appFocused)
		    && s_feed.takeTone(Tone::BuildFailed, nowSec()))
			playTone(ctx, Tone::BuildFailed, gainFor(cfg.RewardsVolume));
		break;
	}
	case Feed::BuildEnd::None:
		break;
	}
}

#ifdef HE_IMGUI_ENABLED
namespace
{
// localDay() once a second rather than every frame; a new day still shows
// within a second of midnight.
const std::string& todayCached()
{
	static std::string day;
	static double      at = -1.0;
	const double now = ImGui::GetTime();
	if (at < 0.0 || now - at >= 1.0 || now < at) { day = localDay(); at = now; }
	return day;
}

// The build window's "done" ring colour: the one green the editor already
// uses for "this worked".
constexpr ImVec4 kDone(90.0f / 255.0f, 215.0f / 255.0f, 90.0f / 255.0f, 1.0f);

ImVec4 withAlpha(ImVec4 c, float a) { c.w *= a; return c; }

// The idle text in pieces, so V3 can light up and roll one number of it. The
// pieces are drawn, never laid out: the footer's layout is one plain text item
// whatever happens inside it.
struct IdleText
{
	struct Piece { std::string text; int tick = -1; };   // tick: 0 builds, 1 streak
	std::vector<Piece> pieces;
	std::string        whole;       // what the pieces say, for measuring
	bool               counters = false;
};

IdleText composeIdle(const char* idleText, const Progress& p, const CounterTick::Look& builds,
                     const CounterTick::Look& streak)
{
	IdleText t;
	t.pieces.push_back({ idleText, -1 });
	if (p.any)
	{
		t.counters = true;
		t.pieces.push_back({ " · ", -1 });
		t.pieces.push_back({ std::to_string(builds.shown), 0 });
		t.pieces.push_back({ builds.shown == 1 ? " build today" : " builds today", -1 });
		if (streak.shown >= 2)
		{
			t.pieces.push_back({ " · ", -1 });
			t.pieces.push_back({ std::to_string(streak.shown), 1 });
			t.pieces.push_back({ " days in a row", -1 });
		}
	}
	for (const auto& pc : t.pieces) t.whole += pc.text;
	return t;
}

void drawIdle(ImDrawList* dl, ImVec2 at, const IdleText& t, float alpha,
              const CounterTick::Look* ticks, bool tickOn)
{
	const float  lineH = ImGui::GetTextLineHeight();
	const ImVec4 grey  = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	float x = at.x;
	for (const auto& pc : t.pieces)
	{
		const float w = ImGui::CalcTextSize(pc.text.c_str()).x;
		const CounterTick::Look* lk = (tickOn && pc.tick >= 0) ? &ticks[pc.tick] : nullptr;
		if (!lk || (lk->glow <= 0.0f && lk->roll >= 1.0f))
		{
			dl->AddText(ImVec2(x, at.y), ImGui::GetColorU32(withAlpha(grey, alpha)), pc.text.c_str());
			x += w;
			continue;
		}
		// Only this number: grey → green by its glow; the old one rolls up and
		// out while the new one comes up from below, clipped to the line.
		const float  g = lk->glow;
		const ImVec4 c(grey.x + (kDone.x - grey.x) * g, grey.y + (kDone.y - grey.y) * g,
		               grey.z + (kDone.z - grey.z) * g, grey.w + (kDone.w - grey.w) * g);
		if (lk->roll >= 1.0f)
			dl->AddText(ImVec2(x, at.y), ImGui::GetColorU32(withAlpha(c, alpha)), pc.text.c_str());
		else
		{
			const std::string old = std::to_string(lk->old);
			const float oldW = ImGui::CalcTextSize(old.c_str()).x;
			const float e    = lk->roll;
			// Half a line of travel, not a whole one: with the cross-fade that
			// reads as a roll, and neither digit is ever mostly cut off.
			const float dy   = lineH * 0.5f;
			dl->PushClipRect(ImVec2(x, at.y), ImVec2(x + std::max(w, oldW), at.y + lineH), true);
			dl->AddText(ImVec2(x, at.y - e * dy),
			            ImGui::GetColorU32(withAlpha(grey, alpha * (1.0f - e))), old.c_str());
			dl->AddText(ImVec2(x, at.y + (1.0f - e) * dy),
			            ImGui::GetColorU32(withAlpha(c, alpha * e)), pc.text.c_str());
			dl->PopClipRect();
		}
		x += w;
	}
}

// The recent-days tooltip: seven small columns (weekday, a dot for a day with
// a moment, its builds, and — only if any of the seven had one — its commits
// in a row of their own), then the counters in words. Neutral on purpose — a
// record of what was, nothing that asks for tomorrow.
void drawRecentDays(const std::string& today)
{
	static const char* kDays[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	const std::vector<DayCell> cells = recentDays(s_tally, today);
	const float  lineH = ImGui::GetTextLineHeight();
	const float  cellW = ImGui::CalcTextSize("Wed").x + 10.0f;
	const ImVec4 grey  = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	const bool   anyCommits = std::any_of(cells.begin(), cells.end(),
	                                      [](const DayCell& c) { return c.commits > 0; });
	const float  rows = anyCommits ? 4.0f : 3.0f;

	ImGui::TextDisabled("Last %d days", kRecentDays);
	const ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(cellW * static_cast<float>(cells.size()), lineH * rows + 2.0f));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (size_t i = 0; i < cells.size(); ++i)
	{
		const DayCell& c  = cells[i];
		const float    cx = o.x + cellW * (static_cast<float>(i) + 0.5f);
		const char* name = c.weekday >= 0 ? kDays[c.weekday] : "?";
		const bool  isToday = i + 1 == cells.size();
		dl->AddText(ImVec2(cx - ImGui::CalcTextSize(name).x * 0.5f, o.y),
		            ImGui::GetColorU32(isToday ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : grey), name);
		const ImVec2 dot(cx, o.y + lineH * 1.5f);
		const float  r = std::max(2.5f, lineH * 0.2f);
		if (c.used) dl->AddCircleFilled(dot, r, ImGui::GetColorU32(kDone));
		else        dl->AddCircle(dot, r, ImGui::GetColorU32(grey), 0, 1.0f);
		if (c.builds > 0)
		{
			const std::string n = std::to_string(c.builds);
			dl->AddText(ImVec2(cx - ImGui::CalcTextSize(n.c_str()).x * 0.5f, o.y + lineH * 2.0f + 2.0f),
			            ImGui::GetColorU32(grey), n.c_str());
		}
		// Commits: a row of their own, in the done green at half strength so
		// it does not read as a second builds row.
		if (c.commits > 0)
		{
			const std::string n = std::to_string(c.commits);
			dl->AddText(ImVec2(cx - ImGui::CalcTextSize(n.c_str()).x * 0.5f, o.y + lineH * 3.0f + 2.0f),
			            ImGui::GetColorU32(withAlpha(kDone, 0.7f)), n.c_str());
		}
	}
	const std::string words = progressText(s_tally, today);
	if (!words.empty()) ImGui::TextUnformatted(words.c_str());
	const std::string commits =
		commitsPhrase(s_tally.day == today ? s_tally.commitsToday : 0);
	if (!commits.empty()) ImGui::TextUnformatted(commits.c_str());
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(cellW * 7.0f, 220.0f));
	ImGui::TextDisabled(anyCommits
		? "A dot is a day you saved, built, imported, compiled, committed or "
		  "finished the tutorial; the number under it, that day's successful "
		  "builds, and below that in green, its commits. Kept on this computer "
		  "only."
		: "A dot is a day you saved, built, imported, compiled, committed or "
		  "finished the tutorial; the number under it, that day's successful "
		  "builds. Kept on this computer only.");
	ImGui::PopTextWrapPos();
}
} // namespace

void drawCheckMark(float x, float y, float size, float stroke, float alpha)
{
	const std::vector<Pt> pts = checkPolyline(stroke, x, y, size);
	if (pts.size() < 2 || !(alpha > 0.0f)) return;
	ImVec2 v[3];
	for (size_t i = 0; i < pts.size() && i < 3; ++i) v[i] = ImVec2(pts[i].x, pts[i].y);
	ImGui::GetWindowDrawList()->AddPolyline(v, static_cast<int>(std::min<size_t>(pts.size(), 3)),
	                                        ImGui::GetColorU32(withAlpha(kDone, alpha)),
	                                        ImDrawFlags_None, std::max(1.5f, size * 0.14f));
}

void drawFooterStatus(AppContext& ctx, const char* idleTextIn)
{
	const EditorConfig& cfg = ctx.editorConfig;
	const float  winW    = ImGui::GetWindowWidth();
	const double now     = ImGui::GetTime();
	const bool   reduced = reducedMotion(ctx);
	const bool   visual  = cfg.RewardsEnabled && cfg.RewardsVisual;
	const Feed::Look lk  = visual ? s_feed.look(now) : Feed::Look{};

	// The counters, as numbers. Observed whatever is shown, so switching the
	// display or the tick on later never ticks a change from before — but not
	// with the master off: nothing counts then, and observing the zeros it
	// shows would make switching it back on a "rise" to the real numbers.
	Progress pr;
	if (cfg.RewardsEnabled)
	{
		loadTally(ctx);
		pr = progressOf(s_tally, todayCached());
		s_buildsTick.observe(pr.builds);
		s_streakTick.observe(pr.streak);
	}
	// V3 waits for the line to be gone: the counters are behind it until then.
	if (!lk.active)
	{
		s_buildsTick.start(now);
		s_streakTick.start(now);
	}
	CounterTick::Look ticks[2] = { s_buildsTick.look(now, reduced),
	                               s_streakTick.look(now, reduced) };
	// A streak below 2 is not shown: one that just reached 2 appears and
	// lights up, it has no old number to roll away.
	if (ticks[1].old < 2) ticks[1].roll = 1.0f;
	const bool tickOn = cfg.RewardsCounterTick;
	// With the tick off the numbers are simply today's.
	const CounterTick::Look plain[2] = { { pr.builds, pr.builds, 0.0f, 1.0f },
	                                     { pr.streak, pr.streak, 0.0f, 1.0f } };

	// "Ready · 3 builds today · 5 days in a row" — or plain "Ready" when the
	// counters are off, empty, or would not fit in the middle third.
	const bool showCounters = cfg.RewardsEnabled && cfg.RewardsShowProgress && pr.any;
	IdleText idle = composeIdle(idleTextIn, showCounters ? pr : Progress{},
	                            tickOn ? ticks[0] : plain[0], tickOn ? ticks[1] : plain[1]);
	if (idle.counters && ImGui::CalcTextSize(idle.whole.c_str()).x > winW / 3.0f)
		idle = composeIdle(idleTextIn, Progress{}, plain[0], plain[1]);
	const float idleW = ImGui::CalcTextSize(idle.whole.c_str()).x;
	ImDrawList* dl    = ImGui::GetWindowDrawList();

	if (!lk.active)
	{
		// Laid out as one text item in an invisible colour — the same place,
		// baseline and hover rectangle "Ready" always had — and drawn in pieces.
		ImGui::SameLine((winW - idleW) * 0.5f);
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0, 0, 0, 0));
		ImGui::TextDisabled("%s", idle.whole.c_str());
		ImGui::PopStyleColor();
		const ImVec2 at = ImGui::GetItemRectMin();
		drawIdle(dl, at, idle, 1.0f, ticks, tickOn);
		// Hover only, never on its own. Last thing in the footer before its
		// End(), so the tooltip's Begin cannot take another item's LastItemData.
		if (idle.counters && cfg.RewardsStreakTooltip
		    && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip())
		{
			drawRecentDays(todayCached());
			ImGui::EndTooltip();
		}
		return;
	}

	const float lineW = ImGui::CalcTextSize(lk.line.c_str()).x;
	ImGui::SameLine((winW - lineW) * 0.5f);
	ImGui::TextColored(withAlpha(kDone, lk.strength), "%s", lk.line.c_str());
	const ImVec2 pos   = ImGui::GetItemRectMin();
	const float  lineH = ImGui::GetTextLineHeight();

	// The idle text fades in where it will stand, on top of the fading line —
	// drawn, not laid out, so the footer's layout is the same every frame.
	if (lk.strength < 1.0f)
		drawIdle(dl, ImVec2(pos.x + (lineW - idleW) * 0.5f, pos.y), idle, 1.0f - lk.strength,
		         ticks, tickOn);

	// V1: the check left of the line, written once per line (not per merge).
	if (cfg.RewardsCheckMark)
	{
		const float size = std::floor(lineH * 0.72f);
		drawCheckMark(pos.x - size - 5.0f, pos.y + (lineH - size) * 0.5f, size,
		              checkStroke(lk.lineAge, reduced), lk.strength);
	}

	// V2b: one pulse of light along the footer's top edge, from the middle out.
	// The window's own clip rect starts below its border; this is its edge.
	if (cfg.RewardsLightEdge)
	{
		const Edge e = edgeAt(lk.lineAge, reduced);
		if (e.alpha > 0.0f && e.spread > 0.0f)
		{
			const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
			const float  mid  = wp.x + ws.x * 0.5f;
			const float  half = ws.x * 0.5f * e.spread;
			dl->PushClipRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false);
			dl->AddLine(ImVec2(mid - half, wp.y + 0.5f), ImVec2(mid + half, wp.y + 0.5f),
			            ImGui::GetColorU32(withAlpha(kDone, e.alpha)), 1.0f);
			dl->PopClipRect();
		}
	}

	// The underline shrinks towards the centre as the moment runs out — it
	// moves, so reduced motion leaves it out.
	if (reduced) return;
	const float y    = pos.y + lineH + 1.0f;
	const float half = lineW * 0.5f * lk.bar;
	const float mid  = pos.x + lineW * 0.5f;
	if (half > 0.5f)
		dl->AddLine(ImVec2(mid - half, y), ImVec2(mid + half, y),
		            ImGui::GetColorU32(withAlpha(kDone, 0.8f * lk.strength)), 1.0f);
}
#else
void drawFooterStatus(AppContext&, const char*) {}
void drawCheckMark(float, float, float, float, float) {}
#endif

} // namespace HE::Ed::Rewards
