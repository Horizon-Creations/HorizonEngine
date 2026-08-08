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

	// Queues a download of `relativePath` (also the SFTP-side relative path —
	// the remote layout mirrors the local EngineContent structure 1:1) if it is
	// not already queued or in flight. `uuid` is carried through only so
	// callers can correlate; this function does not consult the manifest.
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
		std::string                       relativePath;
		HE::UUID                          uuid;
		DownloadTrigger                   trigger = DownloadTrigger::Passive;
		std::function<void(bool)>         onComplete;
	};

	void drainQueue(); // runs on a worker thread; pops+downloads until empty

	mutable std::mutex     m_mutex;
	std::vector<Job>       m_pending;
	bool                   m_draining = false;
	DownloadQueueStatus    m_status;

	mutable std::mutex     m_manifestMutex;
	EngineContentManifest  m_manifest;
};

} // namespace HE::Cs
