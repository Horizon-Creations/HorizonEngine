#include "SelfUpdate/SelfUpdateTransport.h"
#include "SelfUpdate/SelfUpdateEndpoint.h"
#include "SelfUpdate/SelfUpdateManifest.h"

#include <ContentSync/SftpClient.h>

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace HE::Su {

namespace {

class SftpTransport final : public IUpdateTransport
{
public:
	explicit SftpTransport(HE::Cs::SftpEndpoint endpoint) : m_endpoint(std::move(endpoint)) {}

	SuResult fetchFile(const std::string& remoteRelPath, const fs::path& localPath,
	                   const SuProgressFn& progress) override
	{
		// Checked here, not left to SftpClient: its own "not configured" message
		// names the EngineContent endpoint, which would send a reader of the log
		// to the wrong account.
		if (!m_endpoint.configured() || m_endpoint.password.empty())
			return { false, "the self-update SFTP account is not configured (none compiled in, HE_SELFUPDATE_HOST/USER/PASSWORD not set)", false };
		if (!isSafeRelativePath(remoteRelPath))
			return { false, "refusing to fetch an unsafe remote path: '" + remoteRelPath + "'", false };

		const HE::Cs::SftpResult r = HE::Cs::sftpGetFile(m_endpoint, remoteRelPath, localPath.string(),
		                                                 progress ? HE::Cs::SftpProgressFn(progress) : HE::Cs::SftpProgressFn());
		// SftpClient scrubs the EngineContent password only; this account's goes here.
		return { r.ok, scrubSecret(r.error, m_endpoint.password), r.transient };
	}

	std::string describe() const override
	{
		return "sftp://" + m_endpoint.username + "@" + m_endpoint.host + ":" + std::to_string(m_endpoint.port) +
		       (m_endpoint.remoteBasePath.empty() ? std::string("/") : m_endpoint.remoteBasePath);
	}

private:
	HE::Cs::SftpEndpoint m_endpoint;
};

class LocalDirTransport final : public IUpdateTransport
{
public:
	explicit LocalDirTransport(fs::path root) : m_root(std::move(root)) {}

	SuResult fetchFile(const std::string& remoteRelPath, const fs::path& localPath,
	                   const SuProgressFn& progress) override
	{
		if (!isSafeRelativePath(remoteRelPath))
			return { false, "refusing to fetch an unsafe remote path: '" + remoteRelPath + "'", false };

		const fs::path source = m_root / fs::path(remoteRelPath);
		std::error_code ec;
		if (!fs::is_regular_file(source, ec))
			return { false, "no such file on the update server: '" + remoteRelPath + "'", false };

		const std::uint64_t total = fs::file_size(source, ec);
		if (!localPath.parent_path().empty()) fs::create_directories(localPath.parent_path(), ec);

		const fs::path partial = localPath.string() + ".partial";
		{
			std::ifstream in(source, std::ios::binary);
			std::ofstream out(partial, std::ios::binary | std::ios::trunc);
			if (!in || !out)
				return { false, "could not open '" + remoteRelPath + "' or its destination", false };

			if (progress) progress(0, total);
			char buf[64 * 1024];
			std::uint64_t done = 0;
			while (in)
			{
				in.read(buf, sizeof(buf));
				const std::streamsize got = in.gcount();
				if (got <= 0) break;
				out.write(buf, got);
				if (!out)
				{
					out.close();
					fs::remove(partial, ec);
					return { false, "could not write '" + partial.string() + "'", false };
				}
				done += static_cast<std::uint64_t>(got);
				if (progress) progress(done, total);
			}
		}

		fs::rename(partial, localPath, ec);
		if (ec)
		{
			std::error_code rm;
			fs::remove(partial, rm);
			return { false, "could not finalize '" + localPath.string() + "': " + ec.message(), false };
		}
		return { true, {}, false };
	}

	std::string describe() const override { return "dir " + m_root.string(); }

private:
	fs::path m_root;
};

} // namespace

std::unique_ptr<IUpdateTransport> makeSftpTransport(const HE::Cs::SftpEndpoint& endpoint)
{
	return std::make_unique<SftpTransport>(endpoint);
}

std::unique_ptr<IUpdateTransport> makeLocalDirTransport(const fs::path& root)
{
	return std::make_unique<LocalDirTransport>(root);
}

} // namespace HE::Su
