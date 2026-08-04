#include "SourceControl/GitService.h"

#include "SourceControl/GitCli.h"
#include "SourceControl/GitHubApi.h"
#include "SourceControl/RepoConfig.h"
#include "ScLog.h"

#include <filesystem>
#include <utility>

namespace HE::Sc {

GitService::GitService() = default;

GitService::~GitService() { close(); }

void GitService::push(Command c)
{
	{
		std::lock_guard<std::mutex> lock(m_inMutex);
		m_in.push_back(std::move(c));
	}
	m_busy.store(true, std::memory_order_release);
	m_inCv.notify_one();
}

void GitService::open(const std::filesystem::path& anyPathInside)
{
	close();

	m_quit.store(false, std::memory_order_release);
	m_status    = RepoStatus{};
	m_lastError.clear();

	m_worker = std::thread([this] { workerMain(); });
	push(Command{ Kind::Open, anyPathInside });
}

void GitService::close()
{
	if (!m_worker.joinable()) return;

	m_quit.store(true, std::memory_order_release);
	push(Command{ Kind::Quit, {} });
	// Bounded by the timeout on whichever command is already running — every
	// GitCli call carries one, so this cannot wait forever. Once mutating
	// commands arrive (push/pull, which legitimately run for minutes), this needs
	// a real kill handle on the child rather than a timeout; HE::Proc already
	// terminates the whole process group, so that is a matter of exposing the
	// handle, not of changing how the child dies.
	m_worker.join();

	m_busy.store(false, std::memory_order_release);
	m_statusPending.store(false, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(m_outMutex);
		m_out.clear();
	}
	{
		std::lock_guard<std::mutex> lock(m_inMutex);
		m_in.clear();
	}
	m_status = RepoStatus{};
}

void GitService::requestInit(const std::filesystem::path& projectRoot, bool lfsAvailable)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::Init, projectRoot };
	c.flag = lfsAvailable;
	push(std::move(c));
}

void GitService::requestCommitAll(const std::string& message, bool pushAfter)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::CommitAll, {} };
	c.text = message;
	c.flag = pushAfter;
	push(std::move(c));
}

void GitService::requestSetupGitHub(const std::string& repoName, bool isPrivate,
                                    std::string token)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::SetupGitHub, {} };
	c.text   = repoName;
	c.flag   = isPrivate;
	c.secret = std::move(token);
	push(std::move(c));
}

void GitService::requestStoreCredential(const std::string& host,
                                        const std::string& username,
                                        std::string token)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::StoreCredential, {} };
	c.text   = host;
	c.user   = username;
	c.secret = std::move(token);
	push(std::move(c));
}

void GitService::requestPush(bool upstreamConfigured)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::Push, {} };
	c.flag = upstreamConfigured;
	push(std::move(c));
}

void GitService::requestPull()
{
	if (!m_worker.joinable()) return;
	push(Command{ Kind::Pull, {} });
}

void GitService::requestSetRemote(const std::string& url)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::SetRemote, {} };
	c.text = url;
	push(std::move(c));
}

void GitService::requestStatus()
{
	if (!m_worker.joinable()) return;
	// Collapse a burst. Status is a whole-tree snapshot, so four queued refreshes
	// would produce the same answer four times and delay the one that matters.
	if (m_statusPending.exchange(true, std::memory_order_acq_rel)) return;
	push(Command{ Kind::Status, {} });
}

void GitService::workerMain()
{
	for (;;)
	{
		Command cmd;
		{
			std::unique_lock<std::mutex> lock(m_inMutex);
			m_inCv.wait(lock, [this] {
				return !m_in.empty() || m_quit.load(std::memory_order_acquire);
			});
			if (m_in.empty())
			{
				if (m_quit.load(std::memory_order_acquire)) return;
				continue;
			}
			cmd = std::move(m_in.front());
			m_in.pop_front();
		}

		if (cmd.kind == Kind::Quit) return;

		Event ev;
		bool wantStatus = false;   // mutating ops refresh afterwards
		switch (cmd.kind)
		{
		case Kind::Open:
		{
			m_root = GitCli::findRepoRoot(cmd.path);
			if (m_root.empty())
			{
				// Not an error — most projects are simply not in a repository yet,
				// and the UI offers to create one.
				HE_SC_INFO("No git repository found at or above %s", cmd.path.string().c_str());
				ev.statusValid = true;   // a valid answer: "there is no repo"
				break;
			}
			HE_SC_INFO("Repository: %s", m_root.string().c_str());
			wantStatus = true;
			break;
		}
		case Kind::Status:
			wantStatus = true;
			break;

		case Kind::Init:
		{
			std::string err;
			if (!GitCli::init(cmd.path, &err)) { ev.error = err; break; }
			m_root = cmd.path;
			// The generated files exist BEFORE the first status, so the fresh
			// repo immediately shows a sensible change list instead of the whole
			// Saved/ directory as untracked noise.
			if (!RepoConfig::writeInitialFiles(m_root, &err)) { ev.error = err; break; }
			if (cmd.flag)
			{
				// --local keeps the hooks in this repo instead of touching the
				// user's global git config.
				if (!GitCli::run(m_root, { "lfs", "install", "--local" }, 30000).ok)
				{
					// The repo still works; large files just will not be tracked
					// until LFS is sorted. Said out loud rather than swallowed.
					HE_SC_WARN("git lfs install failed — large-file tracking is not active");
				}
			}
			ev.info    = "Repository created.";
			wantStatus = true;
			break;
		}
		case Kind::CommitAll:
		{
			std::string err;

			// ── Size pass, BEFORE anything is staged ─────────────────────────
			// Two verdicts, both cheaper now than later: big media gets routed
			// into LFS per file (a blanket `*.png` glob would drag every icon
			// along — and LFS bandwidth is the resource that runs out on
			// GitHub), and an oversized NON-media file refuses the commit with
			// its name while the fix is still trivial. At push time the same
			// file is a rejected push and a history rewrite.
			{
				RepoStatus pre;
				if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }

				std::vector<std::string> toTrack;
				std::string blocked;
				for (const auto& [rel, entry] : pre.files)
				{
					if (!entry.dirty()) continue;
					std::error_code ec;
					const auto size = std::filesystem::file_size(m_root / rel, ec);
					if (ec) continue;   // deleted or unreadable — nothing to route

					if (RepoConfig::isAutoLfsCandidate(rel))
					{
						if (size >= RepoConfig::kAutoLfsThresholdBytes)
							toTrack.push_back(rel);
					}
					else if (size >= RepoConfig::kHardLimitBytes)
					{
						blocked += "\n  " + rel + " (" +
						           std::to_string(size / (1024 * 1024)) + " MB)";
					}
				}

				if (!blocked.empty())
				{
					ev.error = "Commit refused — these files exceed 100 MB and are "
					           "not media assets, so the push would be rejected:" +
					           blocked + "\nMove them out, or track them through "
					           "LFS by hand if they truly belong in the repository.";
					break;
				}

				if (!toTrack.empty())
				{
					if (!GitCli::lfsAvailable(m_root))
					{
						ev.error = "Commit refused — " + std::to_string(toTrack.size()) +
						           " large media file(s) need Git LFS, and git-lfs is "
						           "not installed. Install it and commit again.";
						break;
					}
					bool ok = true;
					for (const std::string& rel : toTrack)
					{
						if (!GitCli::lfsTrack(m_root, rel, &err)) { ok = false; break; }
						HE_SC_INFO("LFS: tracking %s (over the size threshold)", rel.c_str());
					}
					if (!ok) { ev.error = err; break; }
				}
			}

			if (!GitCli::addAll(m_root, &err))            { ev.error = err; break; }
			if (!GitCli::commit(m_root, cmd.text, &err))  { ev.error = err; break; }
			ev.info = "Committed.";
			// Auto-push, only where a push can even go. A failed push after a
			// successful commit is reported as exactly that — the commit stands.
			if (cmd.flag && !GitCli::remoteUrl(m_root).empty())
			{
				RepoStatus probe;
				const bool upstream = GitCli::status(m_root, probe) && !probe.upstream.empty();
				if (GitCli::push(m_root, upstream, &err)) ev.info = "Committed and pushed.";
				else ev.error = "Committed, but the push failed: " + err;
			}
			wantStatus = true;
			break;
		}
		case Kind::Push:
		{
			std::string err;
			if (!GitCli::push(m_root, cmd.flag, &err)) { ev.error = err; break; }
			ev.info    = "Pushed.";
			wantStatus = true;
			break;
		}
		case Kind::Pull:
		{
			std::string err;
			if (!GitCli::pull(m_root, &err)) { ev.error = err; break; }
			ev.info    = "Pulled.";
			wantStatus = true;
			break;
		}
		case Kind::SetupGitHub:
		{
			std::string err;
			CreatedRepo repo;
			const bool created = GitHubApi::createRepo(cmd.secret, cmd.text, cmd.flag,
			                                           repo, &err);
			// The steps after creation reuse the token once (credential approve)
			// and then it is gone — wiped whether the flow succeeded or not.
			bool ok = created;
			if (ok) ok = GitCli::setRemote(m_root, repo.cloneUrl, &err);
			std::string helperChosen;
			if (ok) ok = GitCli::ensureCredentialHelper(m_root, &helperChosen, &err);
			if (ok)
			{
				// x-access-token is the username GitHub expects when the password
				// field carries a PAT.
				ok = GitCli::approveCredential(m_root, "github.com", "x-access-token",
				                               cmd.secret, &err);
			}
			std::fill(cmd.secret.begin(), cmd.secret.end(), '\0');
			cmd.secret.clear();

			if (ok) ok = GitCli::push(m_root, /*upstreamConfigured=*/false, &err);

			if (!ok) { ev.error = err; wantStatus = true; break; }
			ev.info = "Created " + repo.fullName + " on GitHub and pushed.";
			if (!helperChosen.empty())
				ev.info += " (Credential helper \"" + helperChosen + "\" was configured "
				           "for this repository; the token is stored there.)";
			wantStatus = true;
			break;
		}
		case Kind::StoreCredential:
		{
			std::string err;
			std::string helperChosen;
			bool ok = GitCli::ensureCredentialHelper(m_root, &helperChosen, &err);
			if (ok)
			{
				ok = GitCli::approveCredential(m_root, cmd.text, cmd.user,
				                               cmd.secret, &err);
			}
			// Gone either way — a rejected token must not linger in the queue.
			std::fill(cmd.secret.begin(), cmd.secret.end(), '\0');
			cmd.secret.clear();

			if (!ok) { ev.error = err; break; }
			ev.info = "Token stored for " + cmd.text + ".";
			if (!helperChosen.empty())
				ev.info += " (Credential helper \"" + helperChosen + "\" was configured "
				           "for this repository.)";
			break;
		}
		case Kind::SetRemote:
		{
			std::string err;
			if (!GitCli::setRemote(m_root, cmd.text, &err)) { ev.error = err; break; }
			ev.info    = "Remote configured.";
			wantStatus = true;
			break;
		}
		case Kind::Quit:
			return;
		}

		if (wantStatus)
		{
			m_statusPending.store(false, std::memory_order_release);
			if (m_root.empty())
			{
				ev.statusValid = true;
			}
			else
			{
				std::string err;
				RepoStatus  s;
				if (GitCli::status(m_root, s, &err))
				{
					ev.statusValid = true;
					ev.status      = std::move(s);
					// Piggybacked on every refresh: one cheap invocation, and the
					// panel always knows whether push/pull have anywhere to go.
					ev.remoteUrl      = GitCli::remoteUrl(m_root);
					ev.remoteUrlValid = true;
					// Recent history rides along too — it only changes when a
					// refresh was warranted anyway (commit, pull, poll).
					GitCli::log(m_root, 20, ev.commits);
				}
				else
				{
					ev.error = err;
				}
			}
		}

		{
			std::lock_guard<std::mutex> lock(m_outMutex);
			m_out.push_back(std::move(ev));
		}

		// Only idle once the queue is genuinely empty, or a burst of commands
		// would flicker the UI's busy indicator between each one.
		std::lock_guard<std::mutex> lock(m_inMutex);
		if (m_in.empty()) m_busy.store(false, std::memory_order_release);
	}
}

void GitService::pump(std::size_t maxEvents)
{
	for (std::size_t handled = 0; handled < maxEvents; ++handled)
	{
		Event ev;
		{
			std::lock_guard<std::mutex> lock(m_outMutex);
			if (m_out.empty()) return;
			ev = std::move(m_out.front());
			m_out.pop_front();
		}

		if (!ev.error.empty())
		{
			m_lastError = ev.error;
			m_lastInfo.clear();
			HE_SC_WARN("Operation failed: %s", ev.error.c_str());
			continue;
		}

		m_lastError.clear();
		if (!ev.info.empty())      m_lastInfo  = ev.info;
		if (ev.remoteUrlValid)
		{
			m_remoteUrl = ev.remoteUrl;
			m_commits   = std::move(ev.commits);
		}
		if (!ev.statusValid)       continue;
		// Swapped in whole. A partially updated snapshot is the bug class this
		// design exists to remove.
		ev.status.generation = ++m_generation;
		m_status = std::move(ev.status);

		if (m_onStatus) m_onStatus(m_status);
	}
}

} // namespace HE::Sc
