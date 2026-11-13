#include "ContentSync/EngineContentPublish.h"
#include "ContentSync/CsLog.h"
#include "ContentSync/EngineContentManifest.h"
#include "ContentSync/SftpClient.h"
#include "ContentSync/SftpCredentials.h"

#include <ContentManager/HAsset.h>
#include <Hpak/HpakFormat.h> // Hpak::fnv1a64Bytes — the manifest's change key, see EngineContentManifestEntry

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_set>

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
		entry.contentHash   = Hpak::fnv1a64Bytes(bytes.data(), bytes.size());
		entry.size          = bytes.size();
		local.entries.push_back(std::move(entry));
	}
	log("Scanned " + std::to_string(local.entries.size()) + " EngineContent assets");

	// ── Diff: pull the remote manifest (absent on a first publish — not a
	// failure, just means "everything is new") ─────────────────────────────────
	// One session for the whole publish: the manifest, every directory, every
	// file and the manifest again, instead of one or two handshakes per file.
	SftpConnection conn(endpoint);
	EngineContentManifest remote;
	{
		const fs::path tmp = fs::temp_directory_path(ec) / "engine_content_remote_manifest.json.tmp";
		if (conn.getFile("manifest.json", tmp.string()).ok)
			parseManifest(readWholeFile(tmp), remote);
		std::error_code rmEc;
		fs::remove(tmp, rmEc);
	}

	// ── Upload changed/new files ────────────────────────────────────────────────
	std::size_t uploaded = 0, unchanged = 0;
	std::unordered_set<std::string> ensuredDirs;   // created (or found) once per publish
	for (const auto& entry : local.entries)
	{
		const auto* remoteEntry = remote.findByPath(entry.relativePath);
		if (remoteEntry && remoteEntry->contentHash == entry.contentHash)
		{
			++unchanged;
			continue;
		}

		const fs::path relDir = fs::path(entry.relativePath).parent_path();
		if (!relDir.empty() && !ensuredDirs.count(relDir.generic_string()))
		{
			const SftpResult mk = conn.ensureRemoteDir(relDir.generic_string());
			if (!mk.ok)
			{
				result.error = "Could not create remote directory '" + relDir.generic_string() +
				                "': " + mk.error;
				return result;
			}
			ensuredDirs.insert(relDir.generic_string());
		}

		const std::string localAbs = (fs::path(engineContentRoot) / entry.relativePath).string();
		const SftpResult  put      = conn.putFile(localAbs, entry.relativePath);
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
	const SftpResult putManifest = conn.putFile(tmpManifest.string(), "manifest.json");
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

namespace {

// The manifest entry of one local .hasset (same fields the full publish scan fills).
bool makeLocalEntry(const std::string& root, const std::string& rel, EngineContentManifestEntry& out,
                    std::string& error)
{
	const fs::path abs = fs::path(root) / rel;
	std::ifstream f(abs, std::ios::binary);
	if (!f) { error = "cannot read '" + rel + "'"; return false; }
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	HE::UUID id;
	if (abs.extension() != ".hasset" || !readAssetUuid(bytes, id) || id == HE::UUID{})
	{ error = "'" + rel + "' is not a .hasset with a UUID"; return false; }
	out.relativePath = fs::path(rel).generic_string();
	out.uuid         = id;
	out.contentHash  = Hpak::fnv1a64Bytes(bytes.data(), bytes.size());
	out.size         = bytes.size();
	return true;
}

// The remote manifest, or a failure: unlike the full publish, a manifest that cannot be read is NOT
// "nothing there yet" here, because the merge would then write a manifest holding only our files.
bool fetchRemoteManifest(SftpConnection& conn, EngineContentManifest& out, std::string& error)
{
	std::error_code ec;
	const fs::path tmp = fs::temp_directory_path(ec) / "engine_content_remote_manifest_merge.json.tmp";
	const SftpResult got = conn.getFile("manifest.json", tmp.string());
	if (!got.ok) { error = "cannot read the remote manifest.json: " + got.error; return false; }
	const bool parsed = parseManifest(readWholeFile(tmp), out);
	std::error_code rmEc;
	fs::remove(tmp, rmEc);
	if (!parsed || out.entries.empty()) { error = "the remote manifest.json is empty or unreadable"; return false; }
	return true;
}

} // namespace

PublishResult publishEngineContentFilesBlocking(const std::string& engineContentRoot,
                                                 const std::vector<std::string>& relPaths,
                                                 std::function<void(const std::string&)> onLog)
{
	auto log = [&](const std::string& s) { if (onLog) onLog(s); };
	PublishResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	if (!endpoint.configured()) { result.error = "EngineContent SFTP endpoint is not configured"; return result; }

	std::vector<EngineContentManifestEntry> local;
	for (const std::string& rel : relPaths)
	{
		EngineContentManifestEntry e;
		if (!makeLocalEntry(engineContentRoot, rel, e, result.error)) return result;
		local.push_back(std::move(e));
	}

	SftpConnection conn(endpoint);
	EngineContentManifest remote;
	if (!fetchRemoteManifest(conn, remote, result.error)) return result;
	const std::size_t before = remote.entries.size();

	std::unordered_set<std::string> ensuredDirs;
	for (const auto& entry : local)
	{
		const auto* have = remote.findByPath(entry.relativePath);
		if (have && have->contentHash == entry.contentHash) { ++result.filesUnchanged; continue; }
		const fs::path relDir = fs::path(entry.relativePath).parent_path();
		if (!relDir.empty() && !ensuredDirs.count(relDir.generic_string()))
		{
			const SftpResult mk = conn.ensureRemoteDir(relDir.generic_string());
			if (!mk.ok) { result.error = "Could not create remote directory '" + relDir.generic_string() + "': " + mk.error; return result; }
			ensuredDirs.insert(relDir.generic_string());
		}
		const SftpResult put = conn.putFile((fs::path(engineContentRoot) / entry.relativePath).string(), entry.relativePath);
		if (!put.ok) { result.error = "Upload of '" + entry.relativePath + "' failed: " + put.error; return result; }
		++result.filesUploaded;
		log("Uploaded " + entry.relativePath);
	}

	// Merge: replace the entry of the same path, keep every other, append the new ones.
	for (const auto& entry : local)
	{
		auto it = std::find_if(remote.entries.begin(), remote.entries.end(),
		                       [&](const EngineContentManifestEntry& r) { return r.relativePath == entry.relativePath; });
		if (it != remote.entries.end()) *it = entry; else remote.entries.push_back(entry);
	}
	if (remote.entries.size() < before) { result.error = "internal: the merged manifest lost entries"; return result; }

	std::error_code ec;
	const fs::path tmpManifest = fs::temp_directory_path(ec) / "engine_content_manifest_merge.json.tmp";
	{
		std::ofstream out(tmpManifest, std::ios::binary | std::ios::trunc);
		out << serializeManifest(remote);
	}
	const SftpResult putManifest = conn.putFile(tmpManifest.string(), "manifest.json");
	std::error_code rmEc;
	fs::remove(tmpManifest, rmEc);
	if (!putManifest.ok) { result.error = "Uploading manifest.json failed: " + putManifest.error; return result; }

	result.ok = true;
	log("Merged into the remote manifest: " + std::to_string(before) + " -> " + std::to_string(remote.entries.size()) + " entries");
	return result;
}

PublishResult verifyEngineContentFilesBlocking(const std::string& engineContentRoot,
                                                const std::vector<std::string>& relPaths,
                                                std::function<void(const std::string&)> onLog)
{
	auto log = [&](const std::string& s) { if (onLog) onLog(s); };
	PublishResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	if (!endpoint.configured()) { result.error = "EngineContent SFTP endpoint is not configured"; return result; }

	SftpConnection conn(endpoint);
	EngineContentManifest remote;
	if (!fetchRemoteManifest(conn, remote, result.error)) return result;
	std::vector<RemoteFileInfo> files;
	const SftpResult listing = conn.listRemoteTree(files);
	if (!listing.ok) { result.error = "cannot list the server: " + listing.error; return result; }

	std::size_t bad = 0;
	for (const std::string& rel : relPaths)
	{
		EngineContentManifestEntry e;
		std::string err;
		if (!makeLocalEntry(engineContentRoot, rel, e, err)) { log("BAD  " + err); ++bad; continue; }
		const auto it = std::find_if(files.begin(), files.end(), [&](const RemoteFileInfo& f) { return f.relativePath == e.relativePath; });
		const auto* m = remote.findByPath(e.relativePath);
		const bool sizeOk = it != files.end() && it->size == e.size;
		const bool manOk  = m && m->contentHash == e.contentHash && m->uuid == e.uuid;
		log(std::string(sizeOk && manOk ? "OK   " : "BAD  ") + e.relativePath + "  remote " +
		    (it != files.end() ? std::to_string(it->size) : std::string("missing")) + " B, local " + std::to_string(e.size) +
		    " B, manifest " + (manOk ? "matches" : "differs"));
		if (!(sizeOk && manOk)) ++bad;
	}
	log("Remote manifest holds " + std::to_string(remote.entries.size()) + " entries");
	result.ok = bad == 0;
	if (bad) result.error = std::to_string(bad) + " file(s) differ";
	result.filesUnchanged = relPaths.size() - bad;
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

	// One session for the listing, every .hasset download and the new manifest.
	SftpConnection conn(endpoint);
	std::vector<RemoteFileInfo> files;
	const SftpResult listing = conn.listRemoteTree(files);
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
		const SftpResult dl = conn.getFile(f.relativePath, tmp.string());
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
		entry.contentHash   = Hpak::fnv1a64Bytes(bytes.data(), bytes.size());
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
	const SftpResult putManifest = conn.putFile(tmpManifest.string(), "manifest.json");
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
