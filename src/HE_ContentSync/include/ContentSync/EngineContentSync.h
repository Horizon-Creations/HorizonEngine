#pragma once

// ─── EngineContent download queue + manifest cache ────────────────────────────
// The single place that turns "an EngineContent asset is needed but not local
// yet" into an actual SFTP download, regardless of what triggered the need — a
// confirmed Content Browser double-click, or a scene silently referencing a
// default asset (see ContentManager's materialization hook). Both funnel
// through enqueueDownload() and both show up in the same status() the Editor
// footer polls, so there is exactly one download experience, not two.
//
// Downloads run one at a time on the global thread pool: simpler progress
// reporting (one "current asset" instead of N interleaved ones), and gentler on
// whatever the SFTP account's connection limits are. A queue of unresolved
// EngineContent references is not latency-sensitive — nothing here blocks the
// frame thread.

#include "ContentSync/CsCommon.h"
#include "ContentSync/EngineContentManifest.h"

#include <Types/UUID.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace HE::Cs {

enum class DownloadTrigger : std::uint8_t
{
	Explicit,   // user confirmed a Content Browser double-click
	Passive,    // a scene/asset reference resolved it automatically
};

// Snapshot for UI polling (the Editor footer). Safe to call every frame — it is
// a mutex-guarded copy of a handful of small fields, not a walk of the queue.
struct DownloadQueueStatus
{
	bool        active = false;    // a download is in flight right now
	std::string currentRelativePath;
	std::size_t completedInBatch = 0; // finished since the queue was last empty
	std::size_t totalInBatch     = 0; // completedInBatch + still queued (+ the one in flight)

	// Byte progress of the file currently in flight. Without these a progress
	// bar can only ever step per COMPLETED file — which for the common case of a
	// single-file download means it sits at 0% for the whole transfer and then
	// vanishes, never showing motion. currentBytesTotal is 0 when the server did
	// not report a size (rare, but SFTP does not guarantee it): treat that as
	// "indeterminate", not as "0% done".
	std::uint64_t currentBytesDone  = 0;
	std::uint64_t currentBytesTotal = 0;

	// completedInBatch, plus the fraction of the in-flight file already on disk.
	// This is what a progress bar should fill to; totalInBatch is the divisor.
	double progressFiles() const
	{
		double p = static_cast<double>(completedInBatch);
		if (active && currentBytesTotal > 0)
			p += static_cast<double>(currentBytesDone) / static_cast<double>(currentBytesTotal);
		return p;
	}
};

class HE_CS_API EngineContentSync
{
public:
	static EngineContentSync& instance();

	// Fetches manifest.json from the configured SFTP endpoint. Blocking — call
	// on a worker thread (mirrors probeSftp/probeGit). Leaves the previously
	// held manifest untouched on failure, so a transient network blip does not
	// erase an Editor session's already-known remote catalogue.
	bool refreshManifestBlocking();

	// The last successfully fetched manifest. Empty (not an error) before the
	// first successful refreshManifestBlocking() or when the endpoint is not
	// configured.
	EngineContentManifest manifest() const;

	// ── The manifest, offline ─────────────────────────────────────────────────
	// Every successful refresh also writes the catalogue next to the downloads it
	// describes (engineContentCacheDir()/manifest.json), and this reads it back.
	// Without it the Content Browser's Engine tree is empty of EngineContent the
	// moment the server is unreachable — not just of the assets that are still
	// remote, but of the ones ALREADY DOWNLOADED: they only ever reach the tree
	// through the manifest merge (GlobalState::mergeManifestInto), never through
	// the directory walk. So a user working offline could neither see nor manage
	// their own local copies.
	//
	// Returns true when a cached manifest was read and adopted. Never overwrites a
	// manifest that was fetched from the server this session — a live catalogue
	// always wins over yesterday's copy.
	bool loadCachedManifest();

	// Record that the local copy of `relativePath` is gone, so the catalogue keeps
	// describing it as available-on-the-server rather than forgetting it exists.
	// Called by the Editor's "Remove Local Copy": the entry is what turns the tile
	// back into a remote-only placeholder instead of making it vanish, and it has
	// to survive a restart, so the cached manifest is rewritten too. Adds the
	// entry when the manifest does not have one (a copy downloaded by an older
	// build, or a catalogue that was never fetched this session).
	void noteLocalCopyRemoved(const std::string& relativePath, HE::UUID uuid);

	// Queues a download of `relativePath` (also the SFTP-side relative path —
	// the remote layout mirrors the local EngineContent structure 1:1). `uuid`
	// is carried through only so callers can correlate; this function does not
	// consult the manifest.
	//
	// EVERY caller's onComplete fires exactly once, including when the same path
	// is already queued or already in flight — the new callback is appended to
	// the existing job rather than dropped. This is load-bearing, not politeness:
	// ContentManager marks a UUID as "materializing" before calling in here and
	// only clears that mark when the completion callback fires, so one swallowed
	// callback wedges that asset as permanently-pending for the rest of the
	// session.
	//
	// onComplete fires on a WORKER thread, never the main/frame thread — a
	// caller touching Editor/UI state from it must marshal back itself (the
	// existing ContentManager pollAsyncResults main-thread-drain pattern is the
	// model to follow for that).
	void enqueueDownload(const std::string& relativePath, HE::UUID uuid,
	                      DownloadTrigger trigger, std::function<void(bool success)> onComplete = {});

	DownloadQueueStatus status() const;

private:
	EngineContentSync()                                     = default;
	EngineContentSync(const EngineContentSync&)              = delete;
	EngineContentSync& operator=(const EngineContentSync&)   = delete;

	struct Job
	{
		std::string                            relativePath;
		HE::UUID                               uuid;
		DownloadTrigger                        trigger = DownloadTrigger::Passive;
		// Plural: several callers can await the same file (a scene reference and
		// a Content Browser double-click, say). All of them get their answer.
		std::vector<std::function<void(bool)>> callbacks;
	};

	void drainQueue(); // runs on a worker thread; pops+downloads until empty

	mutable std::mutex     m_mutex;
	std::vector<Job>       m_pending;
	// The job drainQueue is working on right now, so a late enqueue for the same
	// path can attach its callback instead of being dropped. Empty relativePath
	// means "nothing in flight".
	Job                    m_current;
	bool                   m_draining = false;
	DownloadQueueStatus    m_status;

	mutable std::mutex     m_manifestMutex;
	EngineContentManifest  m_manifest;
	// True once the server answered this session. Guards loadCachedManifest()
	// against replacing a live catalogue with the stale one on disk — the probe
	// and a manual refresh can both land after startup restored the cache.
	bool                   m_manifestIsLive = false;
};

} // namespace HE::Cs
