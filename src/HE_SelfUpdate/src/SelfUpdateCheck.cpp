#include "SelfUpdate/SelfUpdateCheck.h"
#include "SelfUpdate/SelfUpdateVersion.h"

#include <Crypto/Sha256.h>
#include <Diagnostics/GlobalState.h>
#include <Diagnostics/Log.h>

#include <HorizonVersion.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace HE::Su {

namespace {

constexpr const char* kPendingFile = "pending.json";
// Dropped into every staging directory this module creates. Pruning and
// discarding only ever delete directories that carry it, so handing a wrong
// path to discardStagedUpdates() cannot wipe a folder that was never ours.
constexpr const char* kMarkerFile = ".he-selfupdate";

SuResult fetchWithRetry(IUpdateTransport& transport, const std::string& remoteRel, const fs::path& local,
                        const SuProgressFn& progress, const CheckOptions& options)
{
	const int attempts = std::max(1, options.attempts);
	SuResult result;
	for (int attempt = 1; attempt <= attempts; ++attempt)
	{
		result = transport.fetchFile(remoteRel, local, progress);
		if (result.ok || !result.transient || attempt == attempts) break;
		HE_LOG_WARN(SelfUpdate, "connection attempt %d of %d failed (%s) — trying again",
		            attempt, attempts, result.error.c_str());
		if (options.backoffMs > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(options.backoffMs) * attempt));
	}
	return result;
}

std::string randomHex()
{
	std::random_device rd;
	const std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
	std::ostringstream os;
	os << std::hex << v;
	return os.str();
}

bool readWholeFile(const fs::path& path, std::string& out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream os;
	os << in.rdbuf();
	out = os.str();
	return true;
}

bool writeFileAtomically(const fs::path& path, const std::string& text)
{
	const fs::path tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << text;
		if (!out) return false;
	}
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (ec)
	{
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

bool hashMatches(const fs::path& file, const std::string& expectedSha256, std::uint64_t expectedSize,
                 std::string& error)
{
	std::error_code ec;
	const std::uint64_t actualSize = fs::file_size(file, ec);
	if (ec)
	{
		error = "staged file is missing: " + file.string();
		return false;
	}
	if (actualSize != expectedSize)
	{
		error = "staged file has " + std::to_string(actualSize) + " bytes, expected " + std::to_string(expectedSize);
		return false;
	}
	std::uint8_t digest[HE::Crypto::Sha256::kDigestSize];
	if (!HE::Crypto::Sha256::hashFile(file, digest))
	{
		error = "could not read the staged file to hash it: " + file.string();
		return false;
	}
	const std::string actual = HE::Crypto::Sha256::toHex(digest);
	if (actual != expectedSha256)
	{
		error = "SHA-256 mismatch: got " + actual + ", expected " + expectedSha256;
		return false;
	}
	return true;
}

// Delete every marked staging directory under `root`, except `keepDirName`.
void pruneStagingDirs(const fs::path& root, const std::string& keepDirName)
{
	std::error_code ec;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		if (!it->is_directory(ec)) continue;
		if (it->path().filename().string() == keepDirName) continue;
		if (!fs::exists(it->path() / kMarkerFile, ec)) continue;
		fs::remove_all(it->path(), ec);
		ec.clear();
	}
}

} // namespace

std::string currentPlatformKey()
{
#if defined(_WIN32)
	const char* os = "windows";
#elif defined(__APPLE__)
	const char* os = "macos";
#elif defined(__linux__)
	const char* os = "linux";
#else
	const char* os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
	const char* arch = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
	const char* arch = "arm64";
#else
	const char* arch = "unknown";
#endif
	return std::string(os) + "-" + arch;
}

LocalBuild runningBuild()
{
	return { HE_VERSION_STRING, currentPlatformKey() };
}

CheckResult checkForUpdate(IUpdateTransport& transport, const std::string& manifestRemotePath,
                           const LocalBuild& local, const CheckOptions& options)
{
	CheckResult result;
	result.localVersion = local.version;
	result.platform     = local.platform;

	Version localVersion;
	if (!parseVersion(local.version, localVersion))
	{
		result.error = "the running build's version is not a version: '" + local.version + "'";
		return result;
	}

	// The manifest comes down into a file of its own and is read from there — the
	// transport only knows files — then removed whatever the outcome.
	std::error_code ec;
	fs::path scratchDir = options.scratchDir.empty() ? fs::temp_directory_path(ec) : options.scratchDir;
	const fs::path scratch = scratchDir / ("he-selfupdate-manifest-" + randomHex() + ".json");

	const SuResult fetched = fetchWithRetry(transport, manifestRemotePath, scratch, {}, options);
	std::string text;
	const bool readOk = fetched.ok && readWholeFile(scratch, text);
	fs::remove(scratch, ec);
	if (!fetched.ok)
	{
		result.error = "could not fetch '" + manifestRemotePath + "' from " + transport.describe() + ": " + fetched.error;
		HE_LOG_WARN(SelfUpdate, "%s", result.error.c_str());
		return result;
	}
	if (!readOk)
	{
		result.error = "could not read the downloaded manifest";
		HE_LOG_WARN(SelfUpdate, "%s", result.error.c_str());
		return result;
	}

	std::string parseError;
	if (!parseUpdateManifest(text, result.manifest, parseError))
	{
		result.error = "the update manifest is not usable: " + parseError;
		HE_LOG_WARN(SelfUpdate, "%s", result.error.c_str());
		return result;
	}

	Version remoteVersion;
	parseVersion(result.manifest.version, remoteVersion);   // parseUpdateManifest already vouched for it
	result.remoteVersion = remoteVersion.toString();

	const int order = compareVersions(remoteVersion, localVersion);
	if (order == 0)
	{
		result.status = CheckStatus::UpToDate;
		HE_LOG_INFO(SelfUpdate, "up to date (%s)", localVersion.toString().c_str());
		return result;
	}
	if (order < 0)
	{
		result.status = CheckStatus::RemoteOlder;
		HE_LOG_INFO(SelfUpdate, "the server offers %s, older than the running %s — ignoring it",
		            result.remoteVersion.c_str(), localVersion.toString().c_str());
		return result;
	}

	const UpdateArtifact* artifact = result.manifest.findArtifact(local.platform);
	if (!artifact)
	{
		result.status = CheckStatus::NoBuildForPlatform;
		result.error  = "version " + result.remoteVersion + " has no build for '" + local.platform + "'";
		HE_LOG_WARN(SelfUpdate, "%s", result.error.c_str());
		return result;
	}

	result.artifact = *artifact;
	// The artifact's path is relative to the directory the manifest sits in.
	const std::size_t slash = manifestRemotePath.rfind('/');
	result.artifactRemotePath = (slash == std::string::npos ? std::string() : manifestRemotePath.substr(0, slash + 1)) +
	                            artifact->path;
	result.status = CheckStatus::UpdateAvailable;
	HE_LOG_INFO(SelfUpdate, "update available: %s -> %s (%s, %llu bytes)",
	            localVersion.toString().c_str(), result.remoteVersion.c_str(), local.platform.c_str(),
	            static_cast<unsigned long long>(artifact->size));
	return result;
}

CheckResult checkForUpdate(const SelfUpdateConfig& config, const LocalBuild& local, const CheckOptions& options)
{
	if (!config.ready())
	{
		CheckResult result;
		result.status       = CheckStatus::NotConfigured;
		result.localVersion = local.version;
		result.platform     = local.platform;
		result.error        = config.problem.empty()
		                          ? std::string("self-update is not configured (HE_SELFUPDATE_* is not set)")
		                          : config.problem;
		return result;
	}
	const std::unique_ptr<IUpdateTransport> transport = makeSftpTransport(config.endpoint);
	return checkForUpdate(*transport, config.manifestRemotePath(), local, options);
}

fs::path defaultStagingDir()
{
	const fs::path base = GlobalState::userDataDir();
	return base.empty() ? fs::path() : base / "SelfUpdate";
}

bool readPendingUpdate(const fs::path& stagingRoot, StagedUpdate& out, std::string& error)
{
	error.clear();
	const fs::path pendingPath = stagingRoot / kPendingFile;
	std::error_code ec;
	if (!fs::exists(pendingPath, ec)) return false;   // nothing staged — not an error

	std::string text;
	if (!readWholeFile(pendingPath, text))
	{
		error = "could not read " + pendingPath.string();
		return false;
	}
	json root;
	try
	{
		root = json::parse(text);
	}
	catch (const json::parse_error& e)
	{
		error = std::string("pending.json is not valid JSON: ") + e.what();
		return false;
	}
	if (!root.is_object() || root.value("schema", 0) != 1)
	{
		error = "pending.json has an unknown schema";
		return false;
	}

	StagedUpdate s;
	s.version  = root.value("version", std::string());
	s.platform = root.value("platform", std::string());
	s.sha256   = root.value("sha256", std::string());
	s.size     = root.value("size", std::uint64_t{ 0 });
	const std::string file = root.value("file", std::string());
	if (s.version.empty() || s.platform.empty() || s.sha256.size() != 64 || s.size == 0 || !isSafeRelativePath(file))
	{
		error = "pending.json is incomplete or names an unsafe file";
		return false;
	}
	s.filePath = stagingRoot / fs::path(file);

	if (!fs::is_regular_file(s.filePath, ec))
	{
		error = "the staged file is gone: " + s.filePath.string();
		return false;
	}
	if (fs::file_size(s.filePath, ec) != s.size)
	{
		error = "the staged file's size no longer matches pending.json";
		return false;
	}
	out = std::move(s);
	return true;
}

bool verifyStagedUpdate(const StagedUpdate& staged, std::string& error)
{
	error.clear();
	return hashMatches(staged.filePath, staged.sha256, staged.size, error);
}

void discardStagedUpdates(const fs::path& stagingRoot)
{
	std::error_code ec;
	fs::remove(stagingRoot / kPendingFile, ec);
	fs::remove(stagingRoot / (std::string(kPendingFile) + ".tmp"), ec);
	pruneStagingDirs(stagingRoot, std::string());
}

DownloadResult downloadUpdate(IUpdateTransport& transport, const CheckResult& check, const fs::path& stagingRoot,
                              const SuProgressFn& progress, const CheckOptions& options)
{
	DownloadResult result;
	if (!check.updateAvailable())
	{
		result.error = "there is no update to download";
		return result;
	}
	if (stagingRoot.empty())
	{
		result.error = "there is no staging directory (the platform gave no per-user data directory)";
		return result;
	}

	const UpdateArtifact& artifact = check.artifact;
	const std::string dirName  = check.remoteVersion + "-" + check.platform;
	const std::string fileName = artifact.fileName();
	if (dirName.find('/') != std::string::npos || !isSafeRelativePath(dirName) || !isSafeRelativePath(fileName))
	{
		result.error = "the version or file name is not safe to stage: '" + dirName + "' / '" + fileName + "'";
		return result;
	}

	std::error_code ec;
	fs::create_directories(stagingRoot, ec);
	if (ec)
	{
		result.error = "could not create the staging directory " + stagingRoot.string() + ": " + ec.message();
		return result;
	}

	// The same build may already be waiting from an earlier run.
	{
		StagedUpdate pending;
		std::string ignored;
		if (readPendingUpdate(stagingRoot, pending, ignored) && pending.version == check.remoteVersion &&
		    pending.platform == check.platform && pending.sha256 == artifact.sha256 && pending.size == artifact.size)
		{
			std::string why;
			if (verifyStagedUpdate(pending, why))
			{
				HE_LOG_INFO(SelfUpdate, "%s is already staged and verifies — nothing to download", pending.version.c_str());
				result.ok = true;
				result.alreadyStaged = true;
				result.staged = std::move(pending);
				return result;
			}
			HE_LOG_WARN(SelfUpdate, "a staged copy of %s no longer verifies (%s) — fetching it again",
			            pending.version.c_str(), why.c_str());
		}
	}

	// Room for the file, with a margin, before a long transfer rather than after
	// it. A failed query (some network file systems) does not block the download.
	const fs::space_info space = fs::space(stagingRoot, ec);
	if (!ec && space.available < artifact.size + artifact.size / 20)
	{
		result.error = "not enough free disk space in " + stagingRoot.string() + " for " +
		               std::to_string(artifact.size) + " bytes";
		return result;
	}

	const fs::path dir  = stagingRoot / dirName;
	const fs::path file = dir / fileName;
	fs::remove_all(dir, ec);   // a leftover of an interrupted run
	fs::create_directories(dir, ec);
	if (ec)
	{
		result.error = "could not create " + dir.string() + ": " + ec.message();
		return result;
	}
	{
		std::ofstream marker(dir / kMarkerFile, std::ios::binary | std::ios::trunc);
		marker << "staged by HorizonSelfUpdate\n";
	}

	auto fail = [&](std::string message)
	{
		std::error_code rm;
		fs::remove_all(dir, rm);
		result.error = std::move(message);
		HE_LOG_WARN(SelfUpdate, "%s", result.error.c_str());
		return result;
	};

	const SuResult fetched = fetchWithRetry(transport, check.artifactRemotePath, file, progress, options);
	if (!fetched.ok)
		return fail("could not download '" + check.artifactRemotePath + "' from " + transport.describe() + ": " + fetched.error);

	std::string why;
	if (!hashMatches(file, artifact.sha256, artifact.size, why))
		return fail("the downloaded build is not the one the manifest named — " + why);

	StagedUpdate staged;
	staged.version  = check.remoteVersion;
	staged.platform = check.platform;
	staged.filePath = file;
	staged.sha256   = artifact.sha256;
	staged.size     = artifact.size;

	json pending;
	pending["schema"]   = 1;
	pending["version"]  = staged.version;
	pending["platform"] = staged.platform;
	pending["file"]     = dirName + "/" + fileName;
	pending["sha256"]   = staged.sha256;
	pending["size"]     = staged.size;
	if (!writeFileAtomically(stagingRoot / kPendingFile, pending.dump(2)))
		return fail("could not record the staged build in " + (stagingRoot / kPendingFile).string());

	pruneStagingDirs(stagingRoot, dirName);

	HE_LOG_INFO(SelfUpdate, "staged %s for %s (%llu bytes, sha256 %s) in %s", staged.version.c_str(),
	            staged.platform.c_str(), static_cast<unsigned long long>(staged.size), staged.sha256.c_str(),
	            dir.string().c_str());
	result.ok     = true;
	result.staged = std::move(staged);
	return result;
}

} // namespace HE::Su
