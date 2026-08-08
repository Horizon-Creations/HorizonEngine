#include "ContentSync/EngineContentPublish.h"
#include "ContentSync/CsLog.h"
#include "ContentSync/EngineContentManifest.h"
#include "ContentSync/SftpClient.h"
#include "ContentSync/SftpCredentials.h"

#include <ContentManager/HAsset.h>
#include <Hpak/HpakFormat.h> // Hpak::hash64 — same content-hash function incremental packing uses

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace HE::Cs {

namespace {

// Same shape as ContentManager's private readMetaChunk (ContentManager.cpp),
// rebuilt here from HAsset::Reader's public API rather than shared: publishing
// is the one place outside ContentManager that needs an asset's UUID without
// registering it, and duplicating four lines of chunk parsing is simpler than
// exporting an internal helper across a module boundary for one caller.
bool readAssetUuid(const std::vector<uint8_t>& bytes, HE::UUID& outId)
{
	HAsset::Reader reader;
	if (!reader.openData(bytes)) return false;
	if (reader.header().version < 2) return false; // v1 assets carry no UUID

	const auto* meta = reader.findChunk(HAsset::CHUNK_META);
	if (!meta) return false;

	std::size_t off = sizeof(std::uint16_t); // asset type, already known from the file header
	return HAsset::Reader::readPOD(meta->data, off, outId.hi) &&
	       HAsset::Reader::readPOD(meta->data, off, outId.lo);
}

std::string readWholeFile(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

} // namespace

PublishResult publishEngineContentBlocking(const std::string& engineContentRoot,
                                            std::function<void(const std::string&)> onLog)
{
	auto log = [&](const std::string& s) { if (onLog) onLog(s); };

	PublishResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	if (!endpoint.configured())
	{
		result.error = "EngineContent SFTP endpoint is not configured";
		return result;
	}

	std::error_code ec;
	if (!fs::is_directory(engineContentRoot, ec))
	{
		result.error = "EngineContent root not found: " + engineContentRoot;
		return result;
	}

	// ── Scan: build the local manifest ─────────────────────────────────────────
	EngineContentManifest local;
	for (const auto& p : fs::recursive_directory_iterator(
	         engineContentRoot, fs::directory_options::skip_permission_denied, ec))
	{
		if (ec) break;
		std::error_code fileEc;
		if (!p.is_regular_file(fileEc) || p.path().extension() != ".hasset") continue;

		std::ifstream f(p.path(), std::ios::binary);
		if (!f) continue;
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		f.close();

		HE::UUID id;
		if (!readAssetUuid(bytes, id) || id == HE::UUID{}) continue;

		std::error_code relEc;
		const fs::path rel = fs::relative(p.path(), engineContentRoot, relEc);
		if (relEc) continue;

		EngineContentManifestEntry entry;
		entry.relativePath = rel.generic_string();
		entry.uuid          = id;
		entry.contentHash   = Hpak::hash64(bytes.data(), bytes.size());
		entry.size          = bytes.size();
		local.entries.push_back(std::move(entry));
	}
	log("Scanned " + std::to_string(local.entries.size()) + " EngineContent assets");

	// ── Diff: pull the remote manifest (absent on a first publish — not a
	// failure, just means "everything is new") ─────────────────────────────────
	EngineContentManifest remote;
	{
		const fs::path tmp = fs::temp_directory_path(ec) / "engine_content_remote_manifest.json.tmp";
		if (sftpGetFile(endpoint, "manifest.json", tmp.string()).ok)
			parseManifest(readWholeFile(tmp), remote);
		std::error_code rmEc;
		fs::remove(tmp, rmEc);
	}

	// ── Upload changed/new files ────────────────────────────────────────────────
	std::size_t uploaded = 0, unchanged = 0;
	for (const auto& entry : local.entries)
	{
		const auto* remoteEntry = remote.findByPath(entry.relativePath);
		if (remoteEntry && remoteEntry->contentHash == entry.contentHash)
		{
			++unchanged;
			continue;
		}

		const fs::path relDir = fs::path(entry.relativePath).parent_path();
		if (!relDir.empty())
		{
			const SftpResult mk = sftpEnsureRemoteDir(endpoint, relDir.generic_string());
			if (!mk.ok)
			{
				result.error = "Could not create remote directory '" + relDir.generic_string() +
				                "': " + mk.error;
				return result;
			}
		}

		const std::string localAbs = (fs::path(engineContentRoot) / entry.relativePath).string();
		const SftpResult  put      = sftpPutFile(endpoint, localAbs, entry.relativePath);
		if (!put.ok)
		{
			result.error = "Upload of '" + entry.relativePath + "' failed: " + put.error;
			return result;
		}
		++uploaded;
		log("Uploaded " + entry.relativePath);
	}

	// ── Upload the manifest LAST ────────────────────────────────────────────────
	// Only after every file it references has actually landed, so a consumer
	// that happens to fetch manifest.json mid-publish never sees an entry for a
	// file that is not there yet.
	const fs::path tmpManifest = fs::temp_directory_path(ec) / "engine_content_manifest.json.tmp";
	{
		std::ofstream out(tmpManifest, std::ios::binary | std::ios::trunc);
		out << serializeManifest(local);
	}
	const SftpResult putManifest = sftpPutFile(endpoint, tmpManifest.string(), "manifest.json");
	std::error_code  rmEc;
	fs::remove(tmpManifest, rmEc);
	if (!putManifest.ok)
	{
		result.error = "Uploading manifest.json failed: " + putManifest.error;
		return result;
	}

	result.ok             = true;
	result.filesUploaded  = uploaded;
	result.filesUnchanged = unchanged;
	log("Publish complete: " + std::to_string(uploaded) + " uploaded, " +
	    std::to_string(unchanged) + " unchanged");
	return result;
}

RebuildManifestResult rebuildManifestFromServerBlocking(std::function<void(const std::string&)> onLog)
{
	auto log = [&](const std::string& s) { if (onLog) onLog(s); };

	RebuildManifestResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	if (!endpoint.configured())
	{
		result.error = "EngineContent SFTP endpoint is not configured";
		return result;
	}

	std::vector<RemoteFileInfo> files;
	const SftpResult listing = sftpListRemoteTree(endpoint, files);
	if (!listing.ok)
	{
		result.error = "Could not list the server: " + listing.error;
		return result;
	}
	log("Found " + std::to_string(files.size()) + " files on the server");

	std::error_code ec;
	EngineContentManifest manifest;
	for (const RemoteFileInfo& f : files)
	{
		// manifest.json is the output of this very function, not content to
		// describe — and would otherwise re-list itself as a mysterious
		// zero-UUID raw entry on every subsequent rebuild.
		if (f.relativePath == "manifest.json") continue;

		if (fs::path(f.relativePath).extension() != ".hasset")
		{
			// Raw/loose file: server-reported size + mtime only, no download —
			// see EngineContentManifestEntry's field comments for why.
			EngineContentManifestEntry entry;
			entry.relativePath = f.relativePath;
			entry.size         = f.size;
			entry.mtime        = f.mtime;
			manifest.entries.push_back(std::move(entry));
			++result.rawEntries;
			continue;
		}

		// .hasset: the UUID lives inside the file, so there is no way to learn
		// it without downloading — same cost publishEngineContentBlocking pays
		// locally, just over the network here.
		const fs::path tmp = fs::temp_directory_path(ec) / "engine_content_rebuild_scan.tmp";
		const SftpResult dl = sftpGetFile(endpoint, f.relativePath, tmp.string());
		if (!dl.ok)
		{
			log("Skipped " + f.relativePath + " (download failed: " + dl.error + ")");
			continue;
		}
		const std::string bytesStr = readWholeFile(tmp);
		std::error_code rmEc;
		fs::remove(tmp, rmEc);

		const std::vector<uint8_t> bytes(bytesStr.begin(), bytesStr.end());
		HE::UUID id;
		if (!readAssetUuid(bytes, id) || id == HE::UUID{})
		{
			log("Skipped " + f.relativePath + " (not a valid .hasset — no UUID)");
			continue;
		}

		EngineContentManifestEntry entry;
		entry.relativePath = f.relativePath;
		entry.uuid          = id;
		entry.contentHash   = Hpak::hash64(bytes.data(), bytes.size());
		entry.size          = bytes.size();
		manifest.entries.push_back(std::move(entry));
		++result.hassetEntries;
		log("Indexed " + f.relativePath);
	}

	const fs::path tmpManifest = fs::temp_directory_path(ec) / "engine_content_manifest.json.tmp";
	{
		std::ofstream out(tmpManifest, std::ios::binary | std::ios::trunc);
		out << serializeManifest(manifest);
	}
	const SftpResult putManifest = sftpPutFile(endpoint, tmpManifest.string(), "manifest.json");
	std::error_code  rmEc;
	fs::remove(tmpManifest, rmEc);
	if (!putManifest.ok)
	{
		result.error = "Uploading manifest.json failed: " + putManifest.error;
		return result;
	}

	result.ok = true;
	log("Manifest rebuilt: " + std::to_string(result.hassetEntries) + " .hasset assets, " +
	    std::to_string(result.rawEntries) + " raw files");
	return result;
}

} // namespace HE::Cs
