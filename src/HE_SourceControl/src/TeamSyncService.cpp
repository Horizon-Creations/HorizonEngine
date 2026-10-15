#include "SourceControl/TeamSyncService.h"

#include "SourceControl/RepoConfig.h"
#include "ScLog.h"

#include <Crypto/Sha256.h>
#include <Net/ScClient.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace HE::Sc {

namespace fs = std::filesystem;
using HE::Net::ScClient;

namespace {

constexpr const char* kStateMagic = "heteamsync 1";

std::uint64_t steadyNowMs()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::int64_t unixNow()
{
	return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string lower(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool sameName(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t tab = line.find('\t', start);
		out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
		if (tab == std::string::npos) break;
		start = tab + 1;
	}
	return out;
}

std::vector<std::string> splitLines(const std::vector<std::uint8_t>& bytes)
{
	std::vector<std::string> out;
	std::string cur;
	for (std::uint8_t b : bytes)
	{
		if (b == '\n') { out.push_back(std::move(cur)); cur.clear(); }
		else cur.push_back(static_cast<char>(b));
	}
	if (!cur.empty()) out.push_back(std::move(cur));
	return out;
}

std::string shaOf(const std::vector<std::uint8_t>& bytes)
{
	std::uint8_t d[HE::Crypto::Sha256::kDigestSize];
	HE::Crypto::Sha256::digest(bytes.data(), bytes.size(), d);
	return HE::Crypto::Sha256::toHex(d);
}

// The hash of the file at `p` now, "" when it does not exist (or cannot be read).
std::string shaOfFile(const fs::path& p)
{
	std::error_code ec;
	if (!fs::is_regular_file(p, ec)) return {};
	std::uint8_t d[HE::Crypto::Sha256::kDigestSize];
	if (!HE::Crypto::Sha256::hashFile(p, d)) return {};
	return HE::Crypto::Sha256::toHex(d);
}

bool readFile(const fs::path& p, std::vector<std::uint8_t>& out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in) return false;
	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0) return false;
	in.seekg(0, std::ios::beg);
	out.resize(static_cast<std::size_t>(size));
	if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
	return static_cast<bool>(in) || in.eof();
}

// Temp file next to the target, then rename: a reader never sees half a file.
bool writeFileAtomic(const fs::path& p, const std::vector<std::uint8_t>& bytes)
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	fs::path tmp = p;
	tmp += ".heteamsync-tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		if (!bytes.empty())
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!out) return false;
	}
	fs::rename(tmp, p, ec);
	if (ec)
	{
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

// A project-relative path from the server, made into a path under `root`, or
// empty when it would leave it ("..", absolute, a drive letter).
fs::path containedPath(const fs::path& root, const std::string& rel)
{
	if (rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos ||
	    rel.find(':') != std::string::npos)
		return {};
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t slash = rel.find('/', start);
		const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (part.empty() || part == "." || part == "..") return {};
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	return root / fs::path(rel);
}

} // namespace

// ─── Ignore rules ────────────────────────────────────────────────────────────

bool TeamSyncService::IgnoreRules::glob(const char* pattern, const char* text)
{
	// '*' and '?' within one path segment; good enough for the generated
	// .gitignore and the usual hand-written lines.
	if (*pattern == '\0') return *text == '\0';
	if (*pattern == '*')
	{
		for (const char* t = text;; ++t)
		{
			if (glob(pattern + 1, t)) return true;
			if (*t == '\0' || *t == '/') return false;
		}
	}
	if (*text == '\0') return false;
	if (*pattern == '?' ? *text != '/' : *pattern == *text) return glob(pattern + 1, text + 1);
	return false;
}

void TeamSyncService::IgnoreRules::addText(const std::string& gitignore)
{
	std::istringstream in(gitignore);
	std::string line;
	while (std::getline(in, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
		if (line.empty() || line[0] == '#' || line[0] == '!') continue;
		Rule r;
		if (line.back() == '/') { r.dirOnly = true; line.pop_back(); }
		if (!line.empty() && line[0] == '/') { r.anchored = true; line.erase(0, 1); }
		if (line.find('/') != std::string::npos) r.anchored = true;
		if (line.empty()) continue;
		r.glob = line;
		rules.push_back(std::move(r));
	}
}

bool TeamSyncService::IgnoreRules::ignored(const std::string& relPath, bool isDir) const
{
	const std::size_t slash = relPath.rfind('/');
	const std::string base  = slash == std::string::npos ? relPath : relPath.substr(slash + 1);
	if (isDir && (relPath == ".git" || relPath == "Saved")) return true;
	if (base.size() > 15 && base.compare(base.size() - 15, 15, ".heteamsync-tmp") == 0) return true;
	for (const Rule& r : rules)
	{
		if (r.dirOnly && !isDir) continue;
		if (r.anchored ? glob(r.glob.c_str(), relPath.c_str()) : glob(r.glob.c_str(), base.c_str()))
			return true;
	}
	return false;
}

// ─── Trailer parsing ─────────────────────────────────────────────────────────

bool TeamSyncService::parseMerge(const std::string& value, std::string& outcome, std::string& path,
                                 std::string& detail)
{
	const std::size_t sp = value.find(' ');
	if (sp == std::string::npos || sp == 0) return false;
	outcome = value.substr(0, sp);
	const std::string rest = value.substr(sp + 1);
	// The detail starts with one of the server's phrases (ScStore.cpp Commit).
	std::size_t cut = std::string::npos;
	for (const char* marker : { " against r", " changed in r", " (", " lost=" })
		cut = std::min(cut, rest.find(marker));
	path   = rest.substr(0, cut);
	detail = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
	return !path.empty();
}

bool TeamSyncService::parseAgainst(const std::string& detail, std::uint64_t& rev, std::string& member)
{
	std::size_t pos = std::string::npos;
	std::size_t skip = 0;
	if ((pos = detail.find("against r")) != std::string::npos) skip = 9;
	else if ((pos = detail.find("changed in r")) != std::string::npos) skip = 12;
	else return false;
	pos += skip;
	std::size_t end = pos;
	while (end < detail.size() && std::isdigit(static_cast<unsigned char>(detail[end]))) ++end;
	if (end == pos || detail.compare(end, 4, " by ") != 0) return false;
	rev = std::stoull(detail.substr(pos, end - pos));
	const std::size_t nameStart = end + 4;
	std::size_t nameEnd = detail.find(' ', nameStart);
	member = detail.substr(nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart);
	return !member.empty();
}

std::vector<TeamSyncService::Lost> TeamSyncService::lostFor(const Revision& revision, const std::string& self)
{
	std::vector<Lost> out;
	if (sameName(revision.member, self)) return out;
	for (const std::string& m : revision.merges)
	{
		Lost l;
		std::string against;
		if (!parseMerge(m, l.outcome, l.path, l.detail)) continue;
		if (l.outcome != "merged-arrival" && l.outcome != "replaced") continue;
		if (!parseAgainst(l.detail, l.ourRev, against) || !sameName(against, self)) continue;
		l.rev    = revision.rev;
		l.winner = revision.member;
		out.push_back(std::move(l));
	}
	return out;
}

// ─── Shared state ────────────────────────────────────────────────────────────

struct TeamSyncService::Impl
{
	Settings settings;

	std::mutex              mu;
	std::condition_variable cv;
	std::atomic<bool>       stopping{ false };

	// ── Commands (main → worker), under mu ──
	bool          wantSync = false;
	std::string   message;
	bool          wantReconcile = false;
	std::uint64_t announcedHead = 0;
	bool          paused = false;
	bool          adoptTeam = false;      // the next reconcile takes the team's version of changed files
	bool          holdIncoming = false;
	int           resolveFirst = -1;      // -1 none, 0 take the team's, 1 keep mine
	bool          wantCreate = false;
	std::deque<Lost> fetches;

	// ── Ticket exchange ──
	bool        needTicket = false;    // worker: please ask
	bool        ticketAsked = false;   // main: asked, waiting for the answer
	bool        ticketReady = false;
	std::string ticket, ticketError;
	bool        ticketProjectMissing = false;

	// ── Incoming files: worker hands over, main writes and answers ──
	std::vector<Incoming>                    incoming;
	bool                                     incomingReady = false;
	std::vector<std::pair<std::string, bool>> answers;   // path, written
	bool                                     answersReady = false;

	// ── Outputs (worker → main), under mu ──
	Status                  status;
	std::vector<FileResult> results;
	std::deque<Revision>    history;
	std::vector<Lost>       lost;
	std::deque<std::pair<std::string, bool>> notices;
	std::deque<fs::path>    opened;

	// ── Worker only ──
	struct Cache { std::uint64_t size = 0; std::int64_t mtime = 0; std::string sha; };
	std::uint64_t                      rev = 0;
	std::map<std::string, std::string> base;    // path → sha256 the server has
	std::map<std::string, Cache>       cache;   // path → what the local file hashed to
	bool                               firstSync = true;   // no state file yet
	std::unique_ptr<ScClient>          client;
	bool                               projectMissing = false;
	bool                               isAdmin = false;
	std::uint64_t                      loggedTo = 0;
	bool                               loggedOnce = false;
	// Reconnect after a failure (under mu, as the commands).
	static constexpr std::uint64_t     kRetryMinMs = 5'000;
	static constexpr std::uint64_t     kRetryMaxMs = 60'000;
	std::uint64_t                      retryAtMs = 0, backoffMs = 0;
	bool                               retrySync = false, retryReconcile = false;
	std::set<std::string>              lostSeen;   // "rev path", toasted once

	template <class F> void setStatus(F&& f)
	{
		std::lock_guard<std::mutex> lock(mu);
		f(status);
	}
	void notice(std::string text, bool error)
	{
		std::lock_guard<std::mutex> lock(mu);
		notices.emplace_back(std::move(text), error);
		while (notices.size() > 32) notices.pop_front();
	}

	// ── Worker steps ──
	void run();
	bool loadState();
	void saveState();
	std::map<std::string, std::string> scan();
	bool ensureConnected();
	bool call(const std::function<std::uint32_t(ScClient::ReplyFn)>& send, ScClient::Reply& out);
	bool pumpUntil(const std::function<bool()>& done);
	void dropConnection(const std::string& why);
	bool reconcile(bool adopt);
	bool upload(const std::string& msg);
	void fetchLog();
	void createProject();
	void resolveFirstSync(bool keepMine);
	void fetchLost(const Lost& l);
	bool getBlobs(const std::set<std::string>& shas, std::map<std::string, std::vector<std::uint8_t>>& out);
	void failed(const std::string& what, const ScClient::Reply& r);
};

// ─── Main-thread API ─────────────────────────────────────────────────────────

TeamSyncService::TeamSyncService() = default;

TeamSyncService::~TeamSyncService() { stop(); }

void TeamSyncService::start(const Settings& settings, TicketFn tickets)
{
	stop();
	m_settings = settings;
	if (m_settings.stateFile.empty())
		m_settings.stateFile = m_settings.projectRoot / "Saved" / "TeamSync" /
		                       (lower(settings.team) + "-" + lower(settings.project) + ".state");
	m_tickets = std::move(tickets);
	m_impl = std::make_shared<Impl>();
	m_impl->settings      = m_settings;
	m_impl->wantReconcile = true;
	m_impl->status.phase  = Phase::Idle;
	m_running = true;
	std::shared_ptr<Impl> impl = m_impl;
	m_thread = std::thread([impl] { impl->run(); });
	HE_SC_INFO("Team sync: started for project '%s' of team '%s' (%s)", settings.project.c_str(),
	           settings.team.c_str(), settings.projectRoot.string().c_str());
}

void TeamSyncService::stop()
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->stopping = true;
	}
	m_impl->cv.notify_all();
	if (m_thread.joinable()) m_thread.join();
	m_impl.reset();
	m_running = false;
}

void TeamSyncService::requestSync(const std::string& message)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->wantSync = true;
		if (!message.empty()) m_impl->message = message;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::notifyHead(std::uint64_t rev)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		if (rev <= m_impl->status.rev) return;   // ours, or one we already have
		m_impl->announcedHead = std::max(m_impl->announcedHead, rev);
		m_impl->wantReconcile = true;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::refresh()
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->wantReconcile = true;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::setPaused(bool paused, bool adoptTeam)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		if (m_impl->paused == paused) return;
		m_impl->paused = paused;
		m_impl->status.paused = paused;
		if (!paused)
		{
			m_impl->wantReconcile = true;
			m_impl->adoptTeam     = adoptTeam;
		}
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::setHoldIncoming(bool hold)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		if (m_impl->holdIncoming == hold) return;
		m_impl->holdIncoming = hold;
		if (!hold) m_impl->wantReconcile = true;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::resolveFirstSync(bool keepMine)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->resolveFirst = keepMine ? 1 : 0;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::createProject()
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->wantCreate = true;
	}
	m_impl->cv.notify_all();
}

void TeamSyncService::fetchLost(const Lost& lost)
{
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lock(m_impl->mu);
		m_impl->fetches.push_back(lost);
	}
	m_impl->cv.notify_all();
}

std::vector<std::string> TeamSyncService::pump(bool mayWrite, const KeepLocalFn& keepLocal)
{
	std::vector<std::string> written;
	if (!m_impl) return written;
	std::shared_ptr<Impl> impl = m_impl;

	bool ask = false;
	{
		std::lock_guard<std::mutex> lock(impl->mu);
		ask = impl->needTicket && !impl->ticketAsked;
		if (ask) impl->ticketAsked = true;
	}
	if (ask)
	{
		std::weak_ptr<Impl> weak = impl;
		Deliver deliver = [weak](const std::string& ticket, const std::string& error,
		                         bool projectMissing, bool isAdmin) {
			std::shared_ptr<Impl> i = weak.lock();
			if (!i) return;
			{
				std::lock_guard<std::mutex> lock(i->mu);
				i->ticket               = ticket;
				i->ticketError          = error;
				i->ticketProjectMissing = projectMissing;
				i->status.isAdmin       = isAdmin;
				i->ticketReady          = true;
				i->needTicket           = false;
				i->ticketAsked          = false;
			}
			i->cv.notify_all();
		};
		if (!m_tickets || !m_tickets(m_settings.project, std::move(deliver)))
		{
			// Cannot even ask (not signed in to the team): asked again next frame.
			std::lock_guard<std::mutex> lock(impl->mu);
			impl->ticketAsked = false;
			if (impl->status.phase != Phase::WaitingTicket) impl->status.phase = Phase::WaitingTicket;
		}
	}

	std::vector<Incoming> items;
	{
		std::lock_guard<std::mutex> lock(impl->mu);
		if (impl->incomingReady && mayWrite)
		{
			items = std::move(impl->incoming);
			impl->incoming.clear();
			impl->incomingReady = false;
		}
	}
	if (!items.empty())
	{
		std::vector<std::pair<std::string, bool>> answers;
		for (Incoming& in : items)
		{
			const fs::path full = containedPath(m_settings.projectRoot, in.path);
			bool ok = false;
			// The decision was made on `expect`: a save since then wins, and
			// the server merges it at the next upload.
			if (!full.empty() && !(keepLocal && keepLocal(in.path)) && shaOfFile(full) == in.expect)
			{
				std::error_code ec;
				ok = in.remove ? (fs::remove(full, ec), !ec) : writeFileAtomic(full, in.bytes);
			}
			if (ok) written.push_back(in.path);
			answers.emplace_back(in.path, ok);
		}
		{
			std::lock_guard<std::mutex> lock(impl->mu);
			impl->answers      = std::move(answers);
			impl->answersReady = true;
		}
		impl->cv.notify_all();
	}
	return written;
}

TeamSyncService::Status TeamSyncService::status() const
{
	if (!m_impl) return {};
	std::lock_guard<std::mutex> lock(m_impl->mu);
	return m_impl->status;
}

std::vector<TeamSyncService::FileResult> TeamSyncService::lastResults() const
{
	if (!m_impl) return {};
	std::lock_guard<std::mutex> lock(m_impl->mu);
	return m_impl->results;
}

std::vector<TeamSyncService::Revision> TeamSyncService::history() const
{
	if (!m_impl) return {};
	std::lock_guard<std::mutex> lock(m_impl->mu);
	return { m_impl->history.begin(), m_impl->history.end() };
}

std::vector<TeamSyncService::Lost> TeamSyncService::lost() const
{
	if (!m_impl) return {};
	std::lock_guard<std::mutex> lock(m_impl->mu);
	return m_impl->lost;
}

bool TeamSyncService::pollNotice(std::string& text, bool& error)
{
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lock(m_impl->mu);
	if (m_impl->notices.empty()) return false;
	text  = std::move(m_impl->notices.front().first);
	error = m_impl->notices.front().second;
	m_impl->notices.pop_front();
	return true;
}

bool TeamSyncService::takeOpened(fs::path& out)
{
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lock(m_impl->mu);
	if (m_impl->opened.empty()) return false;
	out = std::move(m_impl->opened.front());
	m_impl->opened.pop_front();
	return true;
}

// ─── Worker ──────────────────────────────────────────────────────────────────

void TeamSyncService::Impl::run()
{
	firstSync = !loadState();
	setStatus([&](Status& s) { s.rev = rev; });

	while (!stopping)
	{
		bool doCreate = false, doReconcile = false, doSync = false, adopt = false;
		int  resolve = -1;
		std::string msg;
		std::deque<Lost> todo;
		{
			std::unique_lock<std::mutex> lock(mu);
			// Work waits while a ticket is being asked for (needTicket), or the
			// loop would spin until the team server answers.
			// A save during an open first-sync question waits for the answer
			// (it is uploaded after it), it does not wake the loop.
			cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
				const bool work = (wantSync && status.firstSyncDiffers.empty()) || wantReconcile ||
				                  wantCreate || resolveFirst >= 0 || !fetches.empty();
				return stopping || ticketReady || (!paused && work && !needTicket);
			});
			if (stopping) break;
			if (paused)
			{
				status.phase = Phase::Waiting;
				continue;
			}
			// A failed connection is tried again after a pause that grows.
			if (retryAtMs != 0 && steadyNowMs() >= retryAtMs)
			{
				retryAtMs = 0;
				wantSync      = wantSync || retrySync;
				wantReconcile = wantReconcile || retryReconcile;
				retrySync = retryReconcile = false;
			}
			doCreate    = wantCreate;
			doReconcile = wantReconcile;
			doSync      = wantSync;
			resolve     = resolveFirst;
			adopt       = adoptTeam;
			todo        = fetches;
			msg         = message;
		}
		if (!doCreate && !doReconcile && !doSync && resolve < 0 && todo.empty())
		{
			// A ticket that came in with nothing to do (the work was dropped).
			std::lock_guard<std::mutex> lock(mu);
			ticketReady = false;
			continue;
		}
		if (!ensureConnected()) continue;

		if (doCreate)
		{
			{ std::lock_guard<std::mutex> lock(mu); wantCreate = false; }
			createProject();
			continue;   // reconnects with a fresh ticket for the project that now exists
		}
		if (projectMissing)
		{
			// Only CREATE_PROJECT works on this connection; nothing to do until
			// an admin presses it.
			std::lock_guard<std::mutex> lock(mu);
			wantSync = wantReconcile = false;
			resolveFirst = -1;
			fetches.clear();
			status.phase = Phase::Idle;
			continue;
		}
		if (resolve >= 0)
		{
			{ std::lock_guard<std::mutex> lock(mu); resolveFirst = -1; }
			resolveFirstSync(resolve == 1);
		}
		if (doReconcile || (doSync && firstSync))
		{
			{ std::lock_guard<std::mutex> lock(mu); wantReconcile = false; adoptTeam = false; }
			if (!reconcile(adopt))
			{
				std::lock_guard<std::mutex> lock(mu);
				if (!client) wantReconcile = true;   // lost the connection: again at the next trigger
			}
		}
		if (doSync && !firstSync)
		{
			{ std::lock_guard<std::mutex> lock(mu); wantSync = false; message.clear(); }
			upload(msg);
		}
		for (const Lost& l : todo) fetchLost(l);
		if (!todo.empty())
		{
			std::lock_guard<std::mutex> lock(mu);
			for (std::size_t i = 0; i < todo.size() && !fetches.empty(); ++i) fetches.pop_front();
		}
		if (client && client->ready()) fetchLog();
		setStatus([&](Status& s) {
			if (s.phase == Phase::Working || s.phase == Phase::Connecting) s.phase = firstSync && !s.firstSyncDiffers.empty() ? Phase::Waiting : Phase::Idle;
			s.activity.clear();
		});
	}
	if (client) client->close();
	client.reset();
}

bool TeamSyncService::Impl::loadState()
{
	std::ifstream in(settings.stateFile, std::ios::binary);
	if (!in) return false;
	std::string line;
	if (!std::getline(in, line) || line != kStateMagic) return false;
	while (std::getline(in, line))
	{
		const std::vector<std::string> f = splitTabs(line);
		if (f.size() == 2 && f[0] == "rev") rev = std::stoull(f[1]);
		else if (f.size() == 3 && f[0] == "B") base[f[1]] = f[2];
		else if (f.size() == 5 && f[0] == "C")
			cache[f[1]] = Cache{ std::stoull(f[3]), std::stoll(f[4]), f[2] };
	}
	HE_SC_DEBUG("Team sync: state r%llu, %zu files", static_cast<unsigned long long>(rev), base.size());
	return true;
}

void TeamSyncService::Impl::saveState()
{
	std::string out = std::string(kStateMagic) + "\n";
	out += "rev\t" + std::to_string(rev) + "\n";
	for (const auto& [p, sha] : base) out += "B\t" + p + "\t" + sha + "\n";
	for (const auto& [p, c] : cache)
		out += "C\t" + p + "\t" + c.sha + "\t" + std::to_string(c.size) + "\t" + std::to_string(c.mtime) + "\n";
	if (!writeFileAtomic(settings.stateFile, std::vector<std::uint8_t>(out.begin(), out.end())))
		HE_SC_WARN("Team sync: could not write %s", settings.stateFile.string().c_str());
}

std::map<std::string, std::string> TeamSyncService::Impl::scan()
{
	IgnoreRules rules;
	rules.addText(RepoConfig::gitignoreText());
	{
		std::vector<std::uint8_t> own;
		if (readFile(settings.projectRoot / ".gitignore", own))
			rules.addText(std::string(own.begin(), own.end()));
	}
	std::map<std::string, std::string> local;
	std::map<std::string, Cache> seen;
	std::error_code ec;
	fs::recursive_directory_iterator it(settings.projectRoot, fs::directory_options::skip_permission_denied, ec);
	for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
	{
		if (stopping) break;
		const fs::directory_entry& e = *it;
		std::error_code ec2;
		const std::string rel = fs::relative(e.path(), settings.projectRoot, ec2).generic_string();
		if (ec2 || rel.empty()) continue;
		if (e.is_symlink(ec2))
		{
			if (e.is_directory(ec2)) it.disable_recursion_pending();
			continue;
		}
		if (e.is_directory(ec2))
		{
			if (rules.ignored(rel, true)) it.disable_recursion_pending();
			continue;
		}
		if (!e.is_regular_file(ec2) || rules.ignored(rel, false)) continue;
		const std::uint64_t size  = e.file_size(ec2);
		const std::int64_t  mtime = static_cast<std::int64_t>(e.last_write_time(ec2).time_since_epoch().count());
		auto c = cache.find(rel);
		if (c != cache.end() && c->second.size == size && c->second.mtime == mtime && !c->second.sha.empty())
		{
			local[rel] = c->second.sha;
			seen[rel]  = c->second;
			continue;
		}
		const std::string sha = shaOfFile(e.path());
		if (sha.empty()) continue;
		local[rel] = sha;
		seen[rel]  = Cache{ size, mtime, sha };
	}
	cache = std::move(seen);
	return local;
}

void TeamSyncService::Impl::dropConnection(const std::string& why)
{
	if (client) client->close();
	client.reset();
	setStatus([&](Status& s) {
		s.connected = false;
		s.phase     = Phase::Offline;
		s.error     = why;
		s.activity.clear();
	});
}

bool TeamSyncService::Impl::pumpUntil(const std::function<bool()>& done)
{
	int idle = 0;
	while (!done())
	{
		if (stopping || !client) return false;
		client->update();
		if (client->state() == ScClient::State::Closed)
		{
			client->update();   // run the ConnectionLost callbacks
			return done();
		}
		if (done()) break;
		// A few spins first (replies often come back within one), then sleep.
		if (++idle > 32) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		else std::this_thread::yield();
	}
	return true;
}

bool TeamSyncService::Impl::call(const std::function<std::uint32_t(ScClient::ReplyFn)>& send,
                                 ScClient::Reply& out)
{
	if (!client) { out.result = ScClient::Result::ConnectionLost; return false; }
	bool got = false;
	const std::uint32_t id = send([&](ScClient::Reply& r) { out = std::move(r); got = true; });
	if (id == 0)
	{
		out.result = ScClient::Result::ConnectionLost;
		out.text   = client->lastError();
		return false;
	}
	if (!pumpUntil([&] { return got; }) || !got)
	{
		out.result = ScClient::Result::ConnectionLost;
		if (client) out.text = client->lastError();
		return false;
	}
	if (out.result == ScClient::Result::ConnectionLost) dropConnection(ScClient::describeResult(out.result, out.text));
	return out.ok();
}

void TeamSyncService::Impl::failed(const std::string& what, const ScClient::Reply& r)
{
	const std::string why = ScClient::describeResult(r.result, r.text);
	HE_SC_WARN("Team sync: %s failed: %s", what.c_str(), why.c_str());
	if (r.result == ScClient::Result::ConnectionLost) return;   // dropConnection said it
	setStatus([&](Status& s) { s.error = why; });
	notice(what + ": " + why, true);
}

bool TeamSyncService::Impl::ensureConnected()
{
	if (client && client->ready()) return true;
	client.reset();

	std::string t, error;
	bool missing = false;
	{
		std::lock_guard<std::mutex> lock(mu);
		if (!ticketReady)
		{
			needTicket   = true;
			status.phase = Phase::WaitingTicket;
			return false;
		}
		t       = std::move(ticket);
		error   = ticketError;
		missing = ticketProjectMissing;
		isAdmin = status.isAdmin;
		ticket.clear();
		ticketReady = false;
	}
	if (t.empty())
	{
		// Refused at the ticket (no such project, not signed in, an old server):
		// say so and wait for the next trigger instead of asking in a loop.
		std::lock_guard<std::mutex> lock(mu);
		status.phase          = Phase::Offline;
		status.error          = error;
		status.projectMissing = missing;
		wantSync = wantReconcile = false;
		resolveFirst = -1;
		fetches.clear();
		return false;
	}
	setStatus([&](Status& s) { s.phase = Phase::Connecting; s.activity = "Connecting to the team server…"; });
	client = ScClient::connect(HE::Net::RelayAddress{ settings.host, settings.port }, settings.team,
	                           settings.member, settings.project, t);
	std::fill(t.begin(), t.end(), '\0');
	if (!client)
	{
		dropConnection("The team server address or names cannot be used for a sync connection.");
		return false;
	}
	pumpUntil([&] { return client->state() == ScClient::State::Ready ||
	                       client->state() == ScClient::State::Closed; });
	if (!client || client->state() != ScClient::State::Ready)
	{
		const std::string why = client ? client->lastError() : std::string("Stopped.");
		dropConnection(why);
		std::lock_guard<std::mutex> lock(mu);
		// Not in a loop: the work is parked and tried again after 5 s, 10 s,
		// … at most a minute (or at the next save, whichever comes first).
		retrySync      = retrySync || wantSync;
		retryReconcile = retryReconcile || wantReconcile;
		wantSync = wantReconcile = false;
		backoffMs = backoffMs ? std::min<std::uint64_t>(backoffMs * 2, kRetryMaxMs) : kRetryMinMs;
		retryAtMs = steadyNowMs() + backoffMs;
		return false;
	}
	backoffMs      = 0;
	projectMissing = client->projectMissing();
	setStatus([&](Status& s) {
		s.connected      = true;
		s.projectMissing = projectMissing;
		s.phase          = Phase::Idle;
		s.error.clear();
		s.activity.clear();
	});
	return true;
}

bool TeamSyncService::Impl::getBlobs(const std::set<std::string>& shas,
                                     std::map<std::string, std::vector<std::uint8_t>>& out)
{
	// Pipelined: every GET goes out, then the replies are collected.
	std::size_t pending = 0;
	bool        bad     = false;
	std::string why;
	for (const std::string& sha : shas)
	{
		const std::uint32_t id = client->get(sha, [&, sha](ScClient::Reply& r) {
			--pending;
			if (!r.ok() || shaOf(r.stream) != sha)
			{
				bad = true;
				why = r.ok() ? std::string("a download did not match its hash") : ScClient::describeResult(r.result, r.text);
				return;
			}
			out[sha] = std::move(r.stream);
		});
		if (id == 0) return false;
		++pending;
	}
	pumpUntil([&] { return pending == 0; });
	if (pending != 0 || bad)
	{
		if (client && client->state() == ScClient::State::Closed) dropConnection(client->lastError());
		else
		{
			setStatus([&](Status& s) { s.error = "Download from the team server failed: " + why; });
			HE_SC_WARN("Team sync: download failed: %s", why.c_str());
		}
		return false;
	}
	return true;
}

bool TeamSyncService::Impl::reconcile(bool adopt)
{
	setStatus([&](Status& s) { s.phase = Phase::Working; s.activity = "Checking the team's state…"; });
	ScClient::Reply r;
	if (!call([&](ScClient::ReplyFn fn) { return client->manifest(0, std::move(fn)); }, r))
	{
		failed("Reading the team's state", r);
		return false;
	}
	std::uint64_t headRev = 0;
	std::size_t   pos     = 0;
	ScClient::readU64(r.body, pos, headRev);
	std::map<std::string, std::string> team;   // path → sha
	for (const std::string& line : splitLines(r.stream))
	{
		const std::vector<std::string> f = splitTabs(line);
		if (f.size() >= 2 && !f[0].empty()) team[f[0]] = f[1];
	}
	std::map<std::string, std::string> local = scan();

	bool hold = false;
	{
		std::lock_guard<std::mutex> lock(mu);
		hold = holdIncoming;
	}

	std::set<std::string> paths;
	for (const auto& [p, s] : team) paths.insert(p);
	for (const auto& [p, s] : local) paths.insert(p);
	for (const auto& [p, s] : base) paths.insert(p);

	struct Want { std::string path, sha, expect; bool remove = false; };
	std::vector<Want>        wants;
	std::vector<std::string> differs;
	for (const std::string& p : paths)
	{
		const auto mi = team.find(p);
		const auto li = local.find(p);
		const auto bi = base.find(p);
		const std::string m = mi == team.end() ? std::string() : mi->second;
		const std::string l = li == local.end() ? std::string() : li->second;
		const std::string b = bi == base.end() ? std::string() : bi->second;
		if (m == l)
		{
			// In sync: the team's content is what is here.
			if (m.empty()) base.erase(p);
			else base[p] = m;
			continue;
		}
		if (firstSync && b.empty() && !l.empty() && !m.empty())
		{
			differs.push_back(p);   // the person decides (resolveFirstSync)
			continue;
		}
		const bool unchanged = l == b;
		const bool take = adopt ? (!m.empty() || !b.empty()) : unchanged;
		if (!take) continue;   // a local change: the next upload merges it on the server
		if (m.empty() && l.empty()) { base.erase(p); continue; }
		wants.push_back(Want{ p, m, l, m.empty() });
	}

	if (!hold && !wants.empty())
	{
		std::set<std::string> shas;
		for (const Want& w : wants)
			if (!w.remove) shas.insert(w.sha);
		setStatus([&](Status& s) {
			s.activity = "Downloading " + std::to_string(shas.size()) + " file" + (shas.size() == 1 ? "" : "s") + "…";
		});
		std::map<std::string, std::vector<std::uint8_t>> blobs;
		if (!getBlobs(shas, blobs)) return false;
		std::vector<Incoming> items;
		for (const Want& w : wants)
		{
			Incoming in;
			in.path   = w.path;
			in.remove = w.remove;
			in.sha    = w.sha;
			in.expect = w.expect;
			if (!w.remove) in.bytes = blobs[w.sha];
			items.push_back(std::move(in));
		}
		std::vector<std::pair<std::string, bool>> got;
		{
			std::unique_lock<std::mutex> lock(mu);
			incoming        = std::move(items);
			incomingReady   = true;
			answersReady    = false;
			status.activity = "Writing files from the team server…";
			cv.wait(lock, [&] { return stopping || answersReady; });
			if (stopping) return false;
			got = std::move(answers);
			answers.clear();
			answersReady = false;
		}
		std::map<std::string, std::string> wantSha;
		for (const Want& w : wants) wantSha[w.path] = w.sha;
		std::size_t skipped = 0;
		for (const auto& [p, ok] : got)
		{
			if (!ok) { ++skipped; continue; }   // saved meanwhile: merged at the next upload
			const std::string& m = wantSha[p];
			if (m.empty()) { base.erase(p); local.erase(p); }
			else { base[p] = m; local[p] = m; }
			cache.erase(p);   // rehashed at the next scan
		}
		HE_SC_INFO("Team sync: r%llu: %zu file(s) from the team written, %zu kept (changed here)",
		           static_cast<unsigned long long>(headRev), got.size() - skipped, skipped);
	}

	rev = headRev;
	if (firstSync && differs.empty())
	{
		firstSync = false;
		HE_SC_INFO("Team sync: first sync done at r%llu", static_cast<unsigned long long>(rev));
	}
	saveState();

	// What is waiting to go up, for the status line.
	std::size_t waiting = 0;
	for (const std::string& p : paths)
	{
		const auto li = local.find(p);
		const auto bi = base.find(p);
		const std::string l = li == local.end() ? std::string() : li->second;
		const std::string b = bi == base.end() ? std::string() : bi->second;
		if (l != b) ++waiting;
	}
	setStatus([&](Status& s) {
		s.rev              = rev;
		s.waiting          = firstSync ? 0 : waiting;
		s.firstSyncDiffers = firstSync ? differs : std::vector<std::string>{};
		s.lastSyncUnix     = static_cast<std::uint64_t>(unixNow());
		s.phase            = firstSync && !differs.empty() ? Phase::Waiting : Phase::Idle;
	});
	return true;
}

bool TeamSyncService::Impl::upload(const std::string& msg)
{
	setStatus([&](Status& s) { s.phase = Phase::Working; s.activity = "Looking for changes…"; });
	const std::map<std::string, std::string> local = scan();

	struct Entry { char op; std::string path, base, sha; };
	std::vector<Entry> entries;
	std::set<std::string> paths;
	for (const auto& [p, s] : local) paths.insert(p);
	for (const auto& [p, s] : base) paths.insert(p);
	for (const std::string& p : paths)
	{
		const auto li = local.find(p);
		const auto bi = base.find(p);
		const std::string l = li == local.end() ? std::string() : li->second;
		const std::string b = bi == base.end() ? std::string() : bi->second;
		if (l == b) continue;
		entries.push_back(Entry{ l.empty() ? 'D' : (b.empty() ? 'A' : 'M'), p, b, l });
	}
	if (entries.empty())
	{
		setStatus([&](Status& s) { s.waiting = 0; });
		return true;
	}

	// HAVE: which of the new contents the server lacks.
	std::vector<std::string> shas;
	{
		std::set<std::string> uniq;
		for (const Entry& e : entries)
			if (!e.sha.empty() && uniq.insert(e.sha).second) shas.push_back(e.sha);
	}
	ScClient::Reply r;
	if (!shas.empty() && !call([&](ScClient::ReplyFn fn) { return client->have(shas, std::move(fn)); }, r))
	{
		failed("Saving to the team server", r);
		return false;
	}
	const std::vector<std::string> missing = shas.empty() ? std::vector<std::string>{} : splitLines(r.stream);
	std::size_t done = 0;
	for (const std::string& sha : missing)
	{
		const Entry* e = nullptr;
		for (const Entry& x : entries)
			if (x.sha == sha) { e = &x; break; }
		if (!e) continue;
		setStatus([&](Status& s) {
			s.activity = "Uploading " + e->path + " (" + std::to_string(++done) + "/" + std::to_string(missing.size()) + ")…";
		});
		std::vector<std::uint8_t> bytes;
		if (!readFile(settings.projectRoot / fs::path(e->path), bytes) || shaOf(bytes) != sha)
		{
			// Changed again since the scan: start over with the newer content.
			std::lock_guard<std::mutex> lock(mu);
			wantSync = true;
			return false;
		}
		if (!call([&](ScClient::ReplyFn fn) { return client->put(sha, bytes, std::move(fn)); }, r))
		{
			failed("Uploading " + e->path, r);
			return false;
		}
	}

	std::string lines;
	for (const Entry& e : entries)
		lines += std::string(1, e.op) + "\t" + e.path + "\t" + (e.base.empty() ? "-" : e.base) + "\t" +
		         (e.sha.empty() ? "-" : e.sha) + "\n";
	setStatus([&](Status& s) {
		s.activity = "Saving " + std::to_string(entries.size()) + " change" + (entries.size() == 1 ? "" : "s") + " to the team…";
	});
	if (!call([&](ScClient::ReplyFn fn) { return client->commit(rev, msg, lines, std::move(fn)); }, r))
	{
		if (r.result == ScClient::Result::Busy)
		{
			std::lock_guard<std::mutex> lock(mu);
			wantSync = true;
		}
		failed("Saving to the team server", r);
		return false;
	}
	std::uint64_t newRev = 0;
	std::uint8_t  changed = 0;
	std::size_t   pos = 0;
	ScClient::readU64(r.body, pos, newRev);
	ScClient::readU8(r.body, pos, changed);

	std::map<std::string, const Entry*> byPath;
	for (const Entry& e : entries) byPath[e.path] = &e;
	std::vector<FileResult> results;
	for (const std::string& line : splitLines(r.stream))
	{
		const std::vector<std::string> f = splitTabs(line);
		if (f.size() < 3) continue;
		FileResult fr{ f[0], f[1], f.size() >= 4 ? f[3] : std::string() };
		const std::string resultSha = f[2] == "-" ? std::string() : f[2];
		auto it = byPath.find(fr.path);
		if (it != byPath.end())
		{
			const Entry& e = *it->second;
			if (resultSha == e.sha)
			{
				if (e.sha.empty()) base.erase(e.path);
				else base[e.path] = e.sha;
			}
			else if (fr.outcome == "delete-dropped")
			{
				// Somebody changed it since: it stays, and comes back here.
				base.erase(e.path);
			}
			else
			{
				// The server's merge differs from what went up. The base is
				// what went up (the server has it), so the reconcile below
				// fetches the merge — or, if the file was saved again in the
				// meantime, the next upload merges against the right base.
				if (e.sha.empty()) base.erase(e.path);
				else base[e.path] = e.sha;
			}
		}
		results.push_back(std::move(fr));
	}
	if (changed) rev = newRev;
	saveState();
	HE_SC_INFO("Team sync: %zu change(s) saved%s", entries.size(),
	           changed ? (" as r" + std::to_string(newRev)).c_str() : " (nothing new for the server)");
	{
		std::lock_guard<std::mutex> lock(mu);
		this->results = std::move(results);
		status.rev    = rev;
		status.error.clear();
	}
	// Fetch what the merge made and what others saved since our base.
	return reconcile(false);
}

void TeamSyncService::Impl::fetchLog()
{
	if (rev == 0 || loggedTo >= rev) return;
	const std::uint64_t from  = loggedTo ? loggedTo + 1 : (rev > TeamSyncService::kHistory ? rev - TeamSyncService::kHistory + 1 : 1);
	const std::uint64_t count = std::min<std::uint64_t>(rev - from + 1, 1000);
	ScClient::Reply r;
	if (!call([&](ScClient::ReplyFn fn) {
		    return client->log(from, static_cast<std::uint16_t>(count), std::move(fn));
	    }, r))
	{
		failed("Reading the team's history", r);
		return;
	}
	std::vector<Revision> revs;
	for (const std::string& line : splitLines(r.stream))
	{
		const std::vector<std::string> f = splitTabs(line);
		if (f.size() >= 5 && f[0] == "R")
		{
			Revision v;
			v.rev     = std::strtoull(f[1].c_str(), nullptr, 10);
			v.member  = f[2];
			v.time    = std::strtoll(f[3].c_str(), nullptr, 10);
			v.subject = f[4];
			revs.push_back(std::move(v));
		}
		else if (f.size() >= 2 && f[0] == "M" && !revs.empty())
		{
			revs.back().merges.push_back(f[1]);
		}
	}
	std::lock_guard<std::mutex> lock(mu);
	for (const Revision& v : revs)
	{
		for (Lost& l : lostFor(v, settings.member))
		{
			const std::string key = std::to_string(l.rev) + " " + l.path;
			if (!lostSeen.insert(key).second) continue;
			// Only what happened while this editor was running is news.
			if (loggedOnce)
			{
				const std::string what = l.outcome == "replaced"
				    ? l.winner + "'s later save replaced your version"
				    : l.winner + "'s later save won where you both changed the same thing";
				notices.emplace_back(l.path + ": " + what + " (r" + std::to_string(l.rev) +
				                         "). Your version is kept on the team server.", false);
			}
			lost.insert(lost.begin(), std::move(l));
		}
		history.push_front(v);
	}
	while (history.size() > TeamSyncService::kHistory) history.pop_back();
	if (lost.size() > TeamSyncService::kHistory) lost.resize(TeamSyncService::kHistory);
	loggedTo   = rev;
	loggedOnce = true;
}

void TeamSyncService::Impl::createProject()
{
	setStatus([&](Status& s) { s.phase = Phase::Working; s.activity = "Creating the project on the team server…"; });
	ScClient::Reply r;
	if (!call([&](ScClient::ReplyFn fn) { return client->createProject(std::move(fn)); }, r) &&
	    r.result != ScClient::Result::ProjectExists)
	{
		failed("Creating the project", r);
		return;
	}
	HE_SC_INFO("Team sync: project '%s' created on the team server", settings.project.c_str());
	notice("Project \"" + settings.project + "\" created on the team server; uploading it.", false);
	// This connection was only good for creating; a fresh ticket opens it for real.
	client->close();
	client.reset();
	projectMissing = false;
	std::lock_guard<std::mutex> lock(mu);
	status.projectMissing = false;
	status.connected      = false;
	wantReconcile = true;
	wantSync      = true;
}

void TeamSyncService::Impl::resolveFirstSync(bool keepMine)
{
	if (!firstSync) return;
	std::vector<std::string> differs;
	{
		std::lock_guard<std::mutex> lock(mu);
		differs = status.firstSyncDiffers;
	}
	if (keepMine)
	{
		// Uploaded as additions: the server merges each with the team's file
		// (scenes and JSON by entity/key, the rest whole-file, theirs kept).
		firstSync = false;
		saveState();
		{
			std::lock_guard<std::mutex> lock(mu);
			status.firstSyncDiffers.clear();
		}
		upload("First sync of " + settings.member + "'s copy");
		return;
	}
	// The team's version replaces each: a reconcile with them treated as unchanged.
	for (const std::string& p : differs)
	{
		const fs::path full = containedPath(settings.projectRoot, p);
		if (full.empty()) continue;
		base[p] = shaOfFile(full);   // = local: the reconcile then takes the team's
	}
	firstSync = false;
	{
		std::lock_guard<std::mutex> lock(mu);
		status.firstSyncDiffers.clear();
	}
	reconcile(false);
}

void TeamSyncService::Impl::fetchLost(const Lost& l)
{
	ScClient::Reply r;
	if (!call([&](ScClient::ReplyFn fn) { return client->manifest(l.ourRev, std::move(fn)); }, r))
	{
		failed("Finding your version of " + l.path, r);
		return;
	}
	std::string sha;
	for (const std::string& line : splitLines(r.stream))
	{
		const std::vector<std::string> f = splitTabs(line);
		if (f.size() >= 2 && f[0] == l.path) { sha = f[1]; break; }
	}
	if (sha.empty())
	{
		notice("Your version of " + l.path + " is not in r" + std::to_string(l.ourRev) + " on the team server.", true);
		return;
	}
	std::map<std::string, std::vector<std::uint8_t>> blobs;
	if (!getBlobs({ sha }, blobs)) return;
	const fs::path dir  = settings.projectRoot / "Saved" / "TeamSync" / "Lost" / ("r" + std::to_string(l.rev));
	const fs::path full = containedPath(dir, l.path);
	if (full.empty() || !writeFileAtomic(full, blobs[sha]))
	{
		notice("Could not write your version of " + l.path + ".", true);
		return;
	}
	std::lock_guard<std::mutex> lock(mu);
	opened.push_back(full);
}

} // namespace HE::Sc
