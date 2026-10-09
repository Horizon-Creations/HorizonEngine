#include "SourceControl/GitService.h"

#include "SourceControl/GitCli.h"
#include "SourceControl/GitHubApi.h"
#include "SourceControl/GitHubTokenStore.h"
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

void GitService::post(Event ev)
{
	std::lock_guard<std::mutex> lock(m_outMutex);
	m_out.push_back(std::move(ev));
}

void GitService::startWorker()
{
	m_quit.store(false, std::memory_order_release);
	m_status    = RepoStatus{};
	m_lastError.clear();

	m_worker = std::thread([this] { workerMain(); });
}

void GitService::open(const std::filesystem::path& anyPathInside)
{
	close();
	startWorker();
	push(Command{ Kind::Open, anyPathInside });
}

void GitService::requestClone(const std::string& cloneUrl,
                              const std::filesystem::path& targetDir, std::string token)
{
	// No early return without a worker, unlike every other request: nothing is
	// open yet when a project is about to be cloned.
	if (!m_worker.joinable()) startWorker();
	m_lastClonedRoot.clear();
	// Main-thread state, so cleared here: the previous clone's "Cloned … into …"
	// would otherwise stand as this clone's progress until its first phase lands,
	// which comes only after the credential helper has answered.
	m_lastInfo.clear();
	m_lastError.clear();

	Command c{ Kind::Clone, targetDir };
	c.text   = cloneUrl;
	c.secret = std::move(token);
	push(std::move(c));
}

void GitService::requestLfsPull()
{
	if (!m_worker.joinable()) return;
	push(Command{ Kind::LfsPull, {} });
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

void GitService::requestStage(std::vector<std::string> paths)
{
	if (!m_worker.joinable() || paths.empty()) return;
	Command c{ Kind::Stage, {} };
	c.paths = std::move(paths);
	push(std::move(c));
}

void GitService::requestUnstage(std::vector<std::string> paths)
{
	if (!m_worker.joinable() || paths.empty()) return;
	Command c{ Kind::Unstage, {} };
	c.paths = std::move(paths);
	push(std::move(c));
}

void GitService::requestDiscard(std::vector<std::string> paths)
{
	if (!m_worker.joinable() || paths.empty()) return;
	Command c{ Kind::Discard, {} };
	c.paths = std::move(paths);
	push(std::move(c));
}

void GitService::requestCommitStaged(const std::string& message, bool pushAfter, bool amend)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::CommitStaged, {} };
	c.text  = message;
	c.flag  = pushAfter;
	c.flag2 = amend;
	push(std::move(c));
}

void GitService::requestResolveConflict(const std::string& path, bool ours)
{
	if (!m_worker.joinable() || path.empty()) return;
	Command c{ Kind::ResolveConflict, {} };
	c.text = path;
	c.flag = ours;
	push(std::move(c));
}

void GitService::requestSwitchBranch(const std::string& name, bool stashFirst)
{
	if (!m_worker.joinable() || name.empty()) return;
	Command c{ Kind::SwitchBranch, {} };
	c.text = name;
	c.flag = stashFirst;
	push(std::move(c));
}

void GitService::requestStashPop()
{
	if (!m_worker.joinable()) return;
	push(Command{ Kind::StashPop, {} });
}

void GitService::requestCommitFiles(const std::string& commit)
{
	if (!m_worker.joinable() || commit.empty()) return;
	Command c{ Kind::CommitFiles, {} };
	c.text = commit;
	push(std::move(c));
}

const std::vector<GitCli::ChangedFile>*
GitService::commitFiles(const std::string& commit) const
{
	const auto it = m_commitFiles.find(commit);
	return it == m_commitFiles.end() ? nullptr : &it->second;
}

void GitService::requestRestoreTo(const std::string& commit, const std::string& shortOid)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::RestoreTo, {} };
	c.text = commit;
	// Reuses the secret field purely as a second string slot; nothing secret
	// here — it is what the generated commit message names.
	c.secret = shortOid;
	push(std::move(c));
}

void GitService::requestCreateBranch(const std::string& name,
                                    const std::string& startCommit, bool checkout)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::CreateBranch, {} };
	c.text   = name;
	c.secret = startCommit;   // second string slot; nothing secret here
	c.flag   = checkout;
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

void GitService::requestFetch(bool quiet)
{
	if (!m_worker.joinable()) return;
	Command c{ Kind::Fetch, {} };
	c.flag = quiet;
	push(std::move(c));
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

namespace {

// The size pass every path into the index goes through, BEFORE anything is staged:
// two verdicts, both cheaper now than later. Big media is routed into LFS per file
// (a blanket `*.png` glob would drag every icon along - and LFS bandwidth is the
// resource that runs out on GitHub), and an oversized NON-media file refuses the
// operation with its name while the fix is still trivial. At push time the same
// file is a rejected push and a history rewrite.
struct SizeVerdict
{
	std::string              error;     // non-empty = refuse
	std::vector<std::string> tracked;   // newly routed into LFS by this pass
};

SizeVerdict routeLargeFiles(const std::filesystem::path& root,
                            const std::vector<std::string>& rels)
{
	SizeVerdict v;
	std::vector<std::string> toTrack;
	std::string blocked;
	for (const std::string& rel : rels)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(root / rel, ec);
		if (ec) continue;   // deleted or unreadable - nothing to route

		if (RepoConfig::isAutoLfsCandidate(rel))
		{
			if (size >= RepoConfig::kAutoLfsThresholdBytes) toTrack.push_back(rel);
		}
		else if (size >= RepoConfig::kHardLimitBytes)
		{
			blocked += "\n  " + rel + " (" + std::to_string(size / (1024 * 1024)) + " MB)";
		}
	}

	if (!blocked.empty())
	{
		v.error = "these files exceed 100 MB and are not media assets, so the "
		          "push would be rejected:" + blocked + "\nMove them out, or track them "
		          "through LFS by hand if they truly belong in the repository.";
		return v;
	}
	if (toTrack.empty()) return v;

	if (!GitCli::lfsAvailable(root))
	{
		v.error = std::to_string(toTrack.size()) + " large media file(s) "
		          "need Git LFS, and git-lfs is not installed. Install it and try again.";
		return v;
	}
	for (const std::string& rel : toTrack)
	{
		std::string err;
		if (!GitCli::lfsTrack(root, rel, &err)) { v.error = err; return v; }
		HE_SC_INFO("LFS: tracking %s (over the size threshold)", rel.c_str());
		v.tracked.push_back(rel);
	}
	return v;
}

std::vector<std::string> dirtyPaths(const RepoStatus& st)
{
	std::vector<std::string> out;
	for (const auto& [rel, entry] : st.files)
		if (entry.dirty()) out.push_back(rel);
	return out;
}

std::vector<std::string> stagedPaths(const RepoStatus& st)
{
	std::vector<std::string> out;
	for (const auto& [rel, entry] : st.files)
		if (entry.staged()) out.push_back(rel);
	return out;
}

} // namespace

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

			// Size pass (routeLargeFiles) over everything this commit would take in.
			{
				RepoStatus pre;
				if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }
				const SizeVerdict v = routeLargeFiles(m_root, dirtyPaths(pre));
				if (!v.error.empty()) { ev.error = "Commit refused - " + v.error; break; }
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
		case Kind::Stage:
		{
			std::string err;
			const SizeVerdict v = routeLargeFiles(m_root, cmd.paths);
			if (!v.error.empty()) { ev.error = "Staging refused - " + v.error; break; }
			std::vector<std::string> paths = cmd.paths;
			// `lfs track` edits .gitattributes; it has to travel with the files it routes.
			if (!v.tracked.empty()) paths.push_back(".gitattributes");
			if (!GitCli::stage(m_root, paths, &err)) { ev.error = err; wantStatus = true; break; }
			wantStatus = true;
			break;
		}
		case Kind::Unstage:
		{
			std::string err;
			if (!GitCli::unstage(m_root, cmd.paths, &err)) ev.error = err;
			wantStatus = true;
			break;
		}
		case Kind::Discard:
		{
			std::string err;
			if (!GitCli::discard(m_root, cmd.paths, &err)) ev.error = err;
			else ev.info = cmd.paths.size() == 1 ? "Change discarded."
			                                     : std::to_string(cmd.paths.size()) + " changes discarded.";
			wantStatus = true;
			break;
		}
		case Kind::CommitStaged:
		{
			std::string err;
			RepoStatus pre;
			if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }
			if (pre.hasConflicts())
			{
				ev.error = "Resolve the conflicts first.";
				break;
			}
			const std::vector<std::string> staged = stagedPaths(pre);
			if (staged.empty() && !cmd.flag2)
			{
				ev.error = "Nothing is staged - tick the files this commit should contain.";
				break;
			}
			// Files can reach the index without the panel (the command line, an older
			// version of the editor), so the size pass runs here too. Anything it
			// routes into LFS now was staged as a plain blob: add it again, filtered.
			const SizeVerdict v = routeLargeFiles(m_root, staged);
			if (!v.error.empty()) { ev.error = "Commit refused - " + v.error; break; }
			if (!v.tracked.empty())
			{
				std::vector<std::string> args = { "--literal-pathspecs", "add", "--renormalize", "--" };
				args.insert(args.end(), v.tracked.begin(), v.tracked.end());
				args.push_back(".gitattributes");
				const GitResult r = GitCli::run(m_root, args, 60000);
				if (!r.ok) { ev.error = r.err; break; }
			}
			if (!GitCli::commit(m_root, cmd.text, &err, cmd.flag2)) { ev.error = err; break; }
			ev.info = cmd.flag2 ? "Amended." : "Committed.";
			if (cmd.flag && !GitCli::remoteUrl(m_root).empty())
			{
				RepoStatus probe;
				const bool upstream = GitCli::status(m_root, probe) && !probe.upstream.empty();
				if (GitCli::push(m_root, upstream, &err)) ev.info += " Pushed.";
				else ev.error = std::string(cmd.flag2 ? "Amended" : "Committed") +
				                ", but the push failed: " + err;
			}
			wantStatus = true;
			break;
		}
		case Kind::ResolveConflict:
		{
			std::string err;
			if (!GitCli::resolveConflict(m_root, cmd.text, cmd.flag, &err)) ev.error = err;
			wantStatus = true;
			break;
		}
		case Kind::SwitchBranch:
		{
			std::string err;
			bool stashed = false;
			if (cmd.flag)
			{
				RepoStatus pre;
				if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }
				if (pre.dirtyCount() != 0)
				{
					if (!GitCli::stashPush(m_root, "Before switching to " + cmd.text, &err))
					{
						ev.error = err;
						break;
					}
					stashed = true;
				}
			}
			if (!GitCli::switchBranch(m_root, cmd.text, &err))
			{
				ev.error = err;
				// Put the work back where it was: a failed switch must not leave it
				// hiding in a stash the user never asked for.
				if (stashed)
				{
					std::string popErr;
					if (!GitCli::stashPop(m_root, &popErr))
						ev.error += "\nYour changes are still in the stash (" + popErr + ")";
				}
				wantStatus = true;
				break;
			}
			ev.info = "Switched to " + cmd.text + ".";
			if (stashed) ev.info += " Your changes are stashed - use \"Bring back stashed changes\" to restore them.";
			wantStatus = true;
			break;
		}
		case Kind::StashPop:
		{
			std::string err;
			if (!GitCli::stashPop(m_root, &err)) ev.error = err;
			else ev.info = "Stashed changes restored.";
			wantStatus = true;
			break;
		}
		case Kind::CommitFiles:
		{
			std::string err;
			if (!GitCli::commitFiles(m_root, cmd.text, ev.commitFiles, &err))
			{
				ev.commitFiles.clear();
				break;   // not worth an error banner: the row just shows nothing
			}
			ev.commitFilesFor = cmd.text;
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
		case Kind::Fetch:
		{
			std::string err;
			if (!GitCli::fetch(m_root, &err)) { ev.error = err; break; }
			// cmd.flag = quiet: a timer-driven fetch reports nothing on success.
			// The refresh still runs — the whole point is that ahead/behind stop
			// being stale, and that shows up in the numbers, not in a sentence.
			if (!cmd.flag) ev.info = "Fetched.";
			wantStatus = true;
			break;
		}
		case Kind::SetupGitHub:
		{
			std::string err;
			// No token typed: the GitHub sign-in, if there is one. Read here, on
			// the worker — it is a git subprocess — and wiped below like a typed one.
			if (cmd.secret.empty()) GitHubTokenStore::load(m_root, cmd.secret);
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
		case Kind::RestoreTo:
		{
			std::string err;

			// Clean tree required. Restoring overwrites the working tree the
			// way git does — silently — and uncommitted work has no commit to
			// come back from. Committing first is the user's decision, not
			// something to do on their behalf behind a destructive action.
			RepoStatus pre;
			if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }
			if (pre.dirtyCount() != 0)
			{
				ev.error = "Restore refused — there are " +
				           std::to_string(pre.dirtyCount()) +
				           " uncommitted change(s). Commit or discard them first, "
				           "or they would be overwritten with no way back.";
				break;
			}

			if (!GitCli::restoreWorktreeTo(m_root, cmd.text, &err)) { ev.error = err; break; }

			// The difference is staged now; recording it keeps the restore in
			// the history like any other change — which is exactly what makes
			// it undoable rather than a one-way door.
			if (!GitCli::commit(m_root, "Restore project to " + cmd.secret, &err))
			{
				// Nothing to commit means the tree was already in that state.
				// Not a failure, and saying so beats git's raw wording.
				RepoStatus after;
				if (GitCli::status(m_root, after, &err) && after.dirtyCount() == 0)
				{
					ev.info    = "Already at that state — nothing to restore.";
					wantStatus = true;
					break;
				}
				ev.error = err;
				break;
			}
			ev.info    = "Project restored to " + cmd.secret + ".";
			wantStatus = true;
			break;
		}
		case Kind::CreateBranch:
		{
			std::string err;

			// Only the checkout half is destructive: it swaps the working tree
			// for the branch's contents. Writing a ref changes nothing on disk,
			// so a dirty tree is perfectly fine there — and refusing it would
			// block the most useful moment, "park what I have on a branch".
			if (cmd.flag)
			{
				RepoStatus pre;
				if (!GitCli::status(m_root, pre, &err)) { ev.error = err; break; }
				if (pre.dirtyCount() != 0)
				{
					ev.error = "Cannot switch to the new branch — there are " +
					           std::to_string(pre.dirtyCount()) +
					           " uncommitted change(s) that switching would carry "
					           "over or overwrite. Commit them first, or create the "
					           "branch without switching to it.";
					break;
				}
			}

			if (!GitCli::createBranch(m_root, cmd.text, cmd.secret, cmd.flag, &err))
			{
				ev.error = err;
				break;
			}
			ev.info = cmd.flag
				? "Created branch \"" + cmd.text + "\" and switched to it."
				: "Created branch \"" + cmd.text + "\".";
			if (!cmd.secret.empty()) ev.info += " (from " + cmd.secret + ")";
			wantStatus = true;
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
		case Kind::Clone:
		{
			namespace fs = std::filesystem;
			std::string err;
			const std::string&    url    = cmd.text;
			const fs::path&       target = cmd.path;
			const auto wipeToken = [&cmd] {
				std::fill(cmd.secret.begin(), cmd.secret.end(), '\0');
				cmd.secret.clear();
			};

			// Before anything is stored: a token must not reach the helper for a
			// URL that is about to be refused.
			if (!GitCli::isSafeCloneUrl(url, &err)) { wipeToken(); ev.error = err; break; }

			// Which helper holds the credential. Asked from the nearest folder
			// that exists (the target usually does not yet), which is outside
			// any repository — so the answer is system + global config. When
			// that is empty, the platform default rides along on each command
			// as `-c`, because there is no repository for a --local setting.
			fs::path probeDir = target;
			std::error_code ec;
			while (!probeDir.empty() && !fs::exists(probeDir, ec))
			{
				const fs::path parent = probeDir.parent_path();
				if (parent == probeDir) break;
				probeDir = parent;
			}
			const std::string helperOverride = GitCli::credentialHelper(probeDir).empty()
			                                 ? GitCli::defaultCredentialHelper()
			                                 : std::string{};

			// x-access-token is the username GitHub expects when the password
			// field carries a PAT. No host (a file:// URL) means nothing to store.
			bool ok = true;
			const std::string host = GitCli::urlHost(url);
			if (!cmd.secret.empty() && !host.empty())
			{
				ok = GitCli::approveCredential(probeDir, host, "x-access-token", cmd.secret,
				                               &err, helperOverride);
			}
			wipeToken();
			if (!ok) { ev.error = "The access token could not be stored: " + err; break; }

			{
				Event phase;
				phase.info = "Cloning " + url + " ...";
				post(std::move(phase));
			}
			if (!GitCli::clone(url, target, helperOverride, &err)) { ev.error = err; break; }

			// From here on the repository exists, and every outcome says so:
			// clonedRoot travels with errors too, and status is refreshed.
			m_root        = target;
			ev.clonedRoot = target;
			wantStatus    = true;

			// Pin the helper the credential went into, so push/pull from this
			// repository find it without the per-command override.
			std::string helperChosen;
			if (!GitCli::ensureCredentialHelper(m_root, &helperChosen, &err))
			{
				ev.error = "Cloned, but no credential helper could be configured for the "
				           "repository, so pushing may ask for a token again: " + err;
				break;
			}

			if (GitCli::usesLfs(m_root))
			{
				if (!GitCli::lfsAvailable(m_root))
				{
					ev.error = "Cloned, but this repository stores assets in Git LFS and "
					           "git-lfs is not installed — those files are placeholders "
					           "until it is. Install git-lfs, then download the LFS assets.";
					break;
				}
				{
					Event phase;
					phase.info = "Cloned. Downloading LFS assets ...";
					post(std::move(phase));
				}
				// --local like requestInit: the filter config lives in this
				// repository, so later pulls check out real files even where
				// LFS was never installed globally.
				const GitResult install = GitCli::run(m_root, { "lfs", "install", "--local" },
				                                      30000);
				if (!install.ok)
				{
					ev.error = "Cloned, but git lfs install failed, so the LFS assets were "
					           "not downloaded: " + install.err;
					break;
				}
				if (!GitCli::lfsPull(m_root, &err))
				{
					ev.error = "Cloned, but downloading the LFS assets failed — download "
					           "them again to finish: " + err;
					break;
				}
			}

			ev.info = "Cloned " + url + " into " + target.string() + ".";
			if (!helperChosen.empty())
				ev.info += " (Credential helper \"" + helperChosen + "\" was configured "
				           "for this repository.)";
			break;
		}
		case Kind::LfsPull:
		{
			std::string err;
			if (m_root.empty()) { ev.error = "No repository is open."; break; }
			if (!GitCli::lfsAvailable(m_root))
			{
				ev.error = "git-lfs is not installed — install it, then download the "
				           "LFS assets again.";
				break;
			}
			// Idempotent; repairs a clone whose install step failed earlier.
			GitCli::run(m_root, { "lfs", "install", "--local" }, 30000);
			if (!GitCli::lfsPull(m_root, &err)) { ev.error = err; wantStatus = true; break; }
			ev.info    = "LFS assets downloaded.";
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
					std::string ignoredCurrent;
					GitCli::listBranches(m_root, ev.branches, ignoredCurrent);
					GitCli::listRemoteBranches(m_root, ev.remoteBranches);
					GitCli::stashList(m_root, ev.stashes);
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

		if (!ev.clonedRoot.empty()) m_lastClonedRoot = ev.clonedRoot;
		if (!ev.commitFilesFor.empty())
		{
			// Bounded: a long session of expanding commits must not grow this forever.
			if (m_commitFiles.size() >= 64) m_commitFiles.clear();
			m_commitFiles[ev.commitFilesFor] = std::move(ev.commitFiles);
		}

		if (!ev.error.empty())
		{
			m_lastError = ev.error;
			m_lastInfo.clear();
			HE_SC_WARN("Operation failed: %s", ev.error.c_str());
			// A clone that failed AFTER the repository came into existence (LFS,
			// helper) still delivers its status: the working tree is real, and
			// the panel should show it rather than "no repository".
			if (ev.clonedRoot.empty()) continue;
		}
		else
		{
			m_lastError.clear();
			if (!ev.info.empty()) m_lastInfo = ev.info;
		}
		if (ev.remoteUrlValid)
		{
			m_remoteUrl = ev.remoteUrl;
			m_commits   = std::move(ev.commits);
			m_branches  = std::move(ev.branches);
			m_remoteBranches = std::move(ev.remoteBranches);
			m_stashes        = std::move(ev.stashes);
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
