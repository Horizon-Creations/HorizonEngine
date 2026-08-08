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

	{
		std::lock_guard<std::mutex> lock(m_manifestMutex);
		m_manifest = std::move(parsed);
	}
	HE_LOG_INFO(ContentSync, "EngineContent manifest refreshed (%zu entries)", m_manifest.entries.size());
	return true;
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
		globalPool().submit([this] { drainQueue(); });
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
