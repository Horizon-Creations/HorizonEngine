#include "ContentSync/ContentServerClient.h"
#include "ContentSync/CsLog.h"
#include "CsRename.h"

#include <Diagnostics/GlobalState.h>
#include <Net/HttpClient.h>

#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace HE::Cs {

namespace {

std::string percentEncodeSegment(const std::string& raw)
{
	constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(raw.size());
	for (unsigned char c : raw)
	{
		const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
		if (safe) out += static_cast<char>(c);
		else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 0xF]; }
	}
	return out;
}

// '/' stays literal — it is the path's own separator, not part of a segment — everything else
// goes through percentEncodeSegment. Without it a space or a non-ASCII byte in an asset's
// relative path breaks the request line (the server's own decodePercent, FileServer.cpp in the
// server repo, expects exactly this encoding back).
std::string percentEncodePath(const std::string& relativePath)
{
	std::string out;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t slash = relativePath.find('/', start);
		out += percentEncodeSegment(relativePath.substr(start,
			slash == std::string::npos ? std::string::npos : slash - start));
		if (slash == std::string::npos) break;
		out += '/';
		start = slash + 1;
	}
	return out;
}

std::string baseUrl(const ContentServerEndpoint& endpoint)
{
	return "http://" + endpoint.host + ":" + std::to_string(endpoint.port);
}

// A connection that never opened, or opened and then stalled/timed out, is the kind a caller
// can plausibly retry a moment later (sftpWithConnectRetry's definition of `transient` for the
// SFTP side) — a 4xx/404 from the server itself is not, retrying only repeats the same answer.
bool isTransient(const std::string& error)
{
	return error == "connect failed" || error == "connect timeout" ||
	       error == "empty response" || error == "download stalled" ||
	       error == "timed out waiting for response headers";
}

ContentServerEndpoint& mutableContentServerEndpoint()
{
	static ContentServerEndpoint endpoint = []
	{
		ContentServerEndpoint e;
		const std::string url = GlobalState::getInstance().getCustomConfigString("EngineContentServerUrl", "");
		if (!url.empty() && !detail::parseContentServerUrl(url, e))
		{
			HE_LOG_WARN(ContentSync,
			            "EngineContentServerUrl ('%s') is not a plain http:// URL — the new "
			            "content server stays off, falling back to SFTP",
			            url.c_str());
		}
		return e;
	}();
	return endpoint;
}

} // namespace

bool detail::parseContentServerUrl(const std::string& url, ContentServerEndpoint& out)
{
	HE::Net::HttpUrl parsed;
	if (!HE::Net::httpParseUrl(url, parsed)) return false;
	out.host = parsed.host;
	out.port = parsed.port;
	return true;
}

const ContentServerEndpoint& contentServerEndpoint()
{
	return mutableContentServerEndpoint();
}

void setContentServerEndpointForTesting(const ContentServerEndpoint& endpoint)
{
	mutableContentServerEndpoint() = endpoint;
}

SftpResult contentServerGetManifest(const ContentServerEndpoint& endpoint, const std::string& localPath)
{
	const HE::Net::HttpResponse resp = HE::Net::httpGet(baseUrl(endpoint) + "/manifest.json", /*timeoutMs*/ 15000);
	if (!resp.ok || resp.statusCode != 200)
	{
		SftpResult r;
		r.error     = resp.error.empty() ? ("content server returned HTTP " + std::to_string(resp.statusCode)) : resp.error;
		r.transient = isTransient(resp.error);
		return r;
	}

	std::ofstream out(localPath, std::ios::binary | std::ios::trunc);
	if (!out) return { false, "could not write the manifest temp file" };
	out.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
	out.flush();
	if (!out.good()) return { false, "could not write the manifest temp file" };
	return { true, {} };
}

SftpResult contentServerGetFile(const ContentServerEndpoint& endpoint, const std::string& remoteRelPath,
                                 const std::string& localPath, const SftpProgressFn& onProgress)
{
	const fs::path local = localPath;
	std::error_code ec;
	fs::create_directories(local.parent_path(), ec);

	const fs::path partial(local.string() + ".partial");
	const std::string url = baseUrl(endpoint) + "/content/" + percentEncodePath(remoteRelPath);

	const HE::Net::HttpResponse resp = HE::Net::httpDownloadToFile(url, partial.string(), onProgress);
	if (!resp.ok)
	{
		fs::remove(partial, ec);
		SftpResult r;
		r.error     = resp.error;
		r.transient = isTransient(resp.error);
		return r;
	}

	detail::renameReplacing(partial, local, ec);
	if (ec)
	{
		fs::remove(partial, ec);
		return { false, "could not finalize the downloaded file: " + ec.message() };
	}
	return { true, {} };
}

} // namespace HE::Cs
