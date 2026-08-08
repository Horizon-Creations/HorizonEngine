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
		for (const Job& j : m_pending)
			if (j.relativePath == relativePath)
			{
				// Already queued — piggy-back this caller's callback isn't
				// supported (single-callback-per-job, like ContentManager's
				// coalescing); the caller can poll status()/re-check residency.
				return;
			}
		if (m_status.active && m_status.currentRelativePath == relativePath)
			return; // already downloading right now

		m_pending.push_back(Job{ relativePath, uuid, trigger, std::move(onComplete) });
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
		Job job;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_pending.empty())
			{
				m_draining              = false;
				m_status.active         = false;
				m_status.currentRelativePath.clear();
				m_status.completedInBatch = 0;
				m_status.totalInBatch     = 0;
				return;
			}
			job = std::move(m_pending.front());
			m_pending.erase(m_pending.begin());
			m_status.active              = true;
			m_status.currentRelativePath = job.relativePath;
		}

		const SftpEndpoint& endpoint  = engineContentEndpoint();
		const fs::path      localPath = GlobalState::engineContentCacheDir() / job.relativePath;
		const SftpResult    result    = sftpGetFile(endpoint, job.relativePath, localPath.string());

		if (!result.ok)
			HE_LOG_WARN(ContentSync, "Download of '%s' failed: %s",
			            job.relativePath.c_str(), HE::Cs::detail::scrub(result.error).c_str());

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_status.completedInBatch;
		}
		if (job.onComplete) job.onComplete(result.ok);
	}
}

DownloadQueueStatus EngineContentSync::status() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_status;
}

} // namespace HE::Cs
