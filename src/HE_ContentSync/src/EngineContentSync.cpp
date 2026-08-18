#include "ContentSync/EngineContentSync.h"
#include "ContentSync/CsLog.h"
#include "ContentSync/SftpClient.h"
#include "ContentSync/SftpCredentials.h"

#include <Diagnostics/GlobalState.h>
#include <JobSystem/JobSystem.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace HE::Cs {

namespace {
constexpr const char* kManifestRemoteName = "manifest.json";

// The on-disk copy of the catalogue, kept beside the downloads it describes so
// both are equally per-machine and equally survivable.
fs::path cachedManifestPath()
{
	return GlobalState::engineContentCacheDir() / kManifestRemoteName;
}

// Written temp-then-rename: a manifest half-overwritten by a crash or a full disk
// would make every EngineContent asset invisible on the next launch, which is a
// far worse failure than simply keeping the previous one.
bool writeCachedManifest(const EngineContentManifest& manifest)
{
	const fs::path finalPath = cachedManifestPath();
	const fs::path tmpPath   = finalPath.string() + ".tmp";
	{
		std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		const std::string text = serializeManifest(manifest);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		// Checked AFTER the flush, not after the write: write() only fills the
		// stream buffer, so a full disk shows up when the buffer is drained — and
		// the destructor swallows that failure silently. Renaming an unflushed
		// temp file over the real one is how a truncated manifest gets promoted,
		// which would make every EngineContent asset invisible on the next launch.
		out.flush();
		if (!out.good())
		{
			out.close();
			std::error_code rmEc;
			fs::remove(tmpPath, rmEc);
			return false;
		}
	}
	std::error_code ec;
	fs::rename(tmpPath, finalPath, ec);
	if (ec)
	{
		fs::remove(tmpPath, ec);
		return false;
	}
	return true;
}
}

EngineContentSync& EngineContentSync::instance()
{
	static EngineContentSync s;
	return s;
}

bool EngineContentSync::refreshManifestBlocking()
{
	const SftpEndpoint& endpoint = engineContentEndpoint();
	if (!endpoint.configured())
	{
		HE_LOG_WARN(ContentSync, "%s", "EngineContent manifest refresh skipped — SFTP endpoint not configured");
		return false;
	}

	const fs::path tmpPath = GlobalState::engineContentCacheDir() / ".manifest.json.tmp";
	const SftpResult dl = sftpGetFile(endpoint, kManifestRemoteName, tmpPath.string());
	if (!dl.ok)
	{
		HE_LOG_WARN(ContentSync, "Could not fetch EngineContent manifest: %s",
		            HE::Cs::detail::scrub(dl.error).c_str());
		return false;
	}

	std::ifstream in(tmpPath, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	in.close();
	std::error_code ec;
	fs::remove(tmpPath, ec);

	EngineContentManifest parsed;
	if (!parseManifest(ss.str(), parsed))
	{
		HE_LOG_WARN(ContentSync, "%s", "EngineContent manifest failed to parse — keeping previous manifest");
		return false;
	}

	size_t entryCount = 0;
	{
		std::lock_guard<std::mutex> lock(m_manifestMutex);
		m_manifest   = std::move(parsed);
		m_manifestIsLive = true;
		entryCount   = m_manifest.entries.size();
		// Persisted under the same lock as the adoption, so a concurrent
		// noteLocalCopyRemoved() can never interleave and write a manifest that
		// contradicts the one in memory.
		if (!writeCachedManifest(m_manifest))
			HE_LOG_WARN(ContentSync, "%s", "Could not cache the EngineContent manifest for offline use");
	}
	HE_LOG_INFO(ContentSync, "EngineContent manifest refreshed (%zu entries)", entryCount);
	return true;
}

bool EngineContentSync::loadCachedManifest()
{
	std::lock_guard<std::mutex> lock(m_manifestMutex);
	if (m_manifestIsLive) return false;   // this session already has the real thing

	std::ifstream in(cachedManifestPath(), std::ios::binary);
	if (!in) return false;
	std::ostringstream ss;
	ss << in.rdbuf();

	EngineContentManifest parsed;
	if (!parseManifest(ss.str(), parsed)) return false;

	m_manifest = std::move(parsed);
	HE_LOG_INFO(ContentSync, "EngineContent manifest loaded from the local cache (%zu entries)",
	            m_manifest.entries.size());
	return true;
}

void EngineContentSync::noteLocalCopyRemoved(const std::string& relativePath, HE::UUID uuid)
{
	if (relativePath.empty()) return;
	std::lock_guard<std::mutex> lock(m_manifestMutex);

	EngineContentManifestEntry* existing = nullptr;
	for (auto& e : m_manifest.entries)
		if (e.relativePath == relativePath) { existing = &e; break; }

	if (!existing)
	{
		// The file was downloaded, so the server has it — the catalogue simply does
		// not say so right now (no refresh this session, or an older build fetched
		// it). Recording it is what keeps the asset visible as remote-only instead
		// of disappearing from the tree along with its local copy.
		EngineContentManifestEntry entry;
		entry.relativePath = relativePath;
		entry.uuid         = uuid;
		m_manifest.entries.push_back(std::move(entry));
	}
	else if (existing->uuid == HE::UUID{} && uuid != HE::UUID{})
		existing->uuid = uuid;

	if (!writeCachedManifest(m_manifest))
		HE_LOG_WARN(ContentSync, "%s", "Could not update the cached EngineContent manifest");
}

EngineContentManifest EngineContentSync::manifest() const
{
	std::lock_guard<std::mutex> lock(m_manifestMutex);
	return m_manifest;
}

void EngineContentSync::enqueueDownload(const std::string& relativePath, HE::UUID uuid,
                                         DownloadTrigger trigger, std::function<void(bool)> onComplete)
{
	bool startDraining = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		// Already queued, or already the job in flight: attach this caller's
		// callback to that job rather than returning empty-handed. Dropping it
		// would leave the caller waiting forever — see enqueueDownload's contract.
		Job* existing = nullptr;
		for (Job& j : m_pending)
			if (j.relativePath == relativePath) { existing = &j; break; }
		if (!existing && !m_current.relativePath.empty() && m_current.relativePath == relativePath)
			existing = &m_current;

		if (existing)
		{
			if (onComplete) existing->callbacks.push_back(std::move(onComplete));
			return;
		}

		Job job;
		job.relativePath = relativePath;
		job.uuid         = uuid;
		job.trigger      = trigger;
		if (onComplete) job.callbacks.push_back(std::move(onComplete));
		m_pending.push_back(std::move(job));

		m_status.totalInBatch = m_status.completedInBatch + m_pending.size() + (m_status.active ? 1 : 0);

		if (!m_draining)
		{
			m_draining     = true;
			startDraining  = true;
		}
	}
	if (startDraining)
		globalPool().submit([this] { drainQueue(); }, "ContentSyncDrain");
}

void EngineContentSync::drainQueue()
{
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_pending.empty())
			{
				// Batch over. m_draining stays TRUE until the very end of this
				// scope-exit path is irrelevant — we return immediately — but note
				// it must never be cleared while this loop can still run, or a
				// concurrent enqueue would start a second drainQueue.
				m_draining                = false;
				m_current                 = Job{};
				m_status                  = DownloadQueueStatus{};
				return;
			}
			m_current = std::move(m_pending.front());
			m_pending.erase(m_pending.begin());
			m_status.active              = true;
			m_status.currentRelativePath = m_current.relativePath;
			m_status.currentBytesDone    = 0;
			m_status.currentBytesTotal   = 0;
		}

		const std::string relativePath = m_current.relativePath;
		const SftpEndpoint& endpoint   = engineContentEndpoint();
		const fs::path      localPath  = GlobalState::engineContentCacheDir() / relativePath;

		// Byte-level progress, published as the read loop advances. Captures
		// `this` deliberately: EngineContentSync is a function-local static
		// (instance()), so it outlives every job by construction.
		const SftpResult result = sftpGetFile(endpoint, relativePath, localPath.string(),
			[this](std::uint64_t done, std::uint64_t total)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_status.currentBytesDone  = done;
				m_status.currentBytesTotal = total;
			});

		if (!result.ok)
			HE_LOG_WARN(ContentSync, "Download of '%s' failed: %s",
			            relativePath.c_str(), HE::Cs::detail::scrub(result.error).c_str());

		// Take the callbacks and settle the status under ONE lock, before firing
		// anything. Two reasons this order matters:
		//  • A caller's callback can take a long time (the Content Browser's runs
		//    a full recursive content refresh). Leaving `active` set with this
		//    file's name in it for that whole window makes the footer claim it is
		//    still downloading a file that is already on disk — and lets a
		//    concurrent enqueue count a phantom in-flight job into totalInBatch,
		//    so the bar can never reach the end.
		//  • Firing callbacks while holding m_mutex would deadlock the moment one
		//    of them enqueues another download.
		std::vector<std::function<void(bool)>> callbacks;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			callbacks.swap(m_current.callbacks);
			m_current = Job{};
			++m_status.completedInBatch;
			m_status.currentBytesDone  = 0;
			m_status.currentBytesTotal = 0;
			// This file is done, so it must stop being advertised as the one in
			// flight for the whole callback window below — otherwise the footer
			// reads "Downloading <file that already landed>".
			m_status.currentRelativePath.clear();
			// Nothing queued behind it: settle to idle now. (The counters
			// themselves are zeroed by the next iteration's early-out branch,
			// which runs immediately after the callbacks.)
			if (m_pending.empty()) m_status.active = false;
		}
		for (auto& cb : callbacks) cb(result.ok);
	}
}

DownloadQueueStatus EngineContentSync::status() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_status;
}

} // namespace HE::Cs
