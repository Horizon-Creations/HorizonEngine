#include "doctest.h"

#include "helpers/OneShotHttpServer.h"

#include <ContentSync/ContentServerClient.h>
#include <ContentSync/EngineContentManifest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// Thema 194, step 3: the engine-side client for the new HTTP EngineContent server. These cases
// exercise HE::Cs::ContentServerClient directly — the pure URL parsing, the test-only endpoint
// override, and the manifest/file fetchers against a real loopback server (HE::Test::OneShotHttpServer,
// shared with test_net_discovery.cpp's HE::Net::HttpClient coverage) — without touching
// EngineContentSync's singleton or its on-disk cache (see test_engine_content_manifest_refresh.cpp
// for that, which drives refreshManifestWith directly and needs the isolated-HOME ctest setup this
// file deliberately avoids by writing only into its own TMPDIR).

using HE::Test::OneShotHttpServer;

namespace
{
std::string readWholeFile(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("parseContentServerUrl: parses host and port, rejects https and non-URLs")
{
	HE::Cs::ContentServerEndpoint e;
	REQUIRE(HE::Cs::detail::parseContentServerUrl("http://127.0.0.1:18080", e));
	CHECK(e.host == "127.0.0.1");
	CHECK(e.port == 18080);
	CHECK(e.configured());

	HE::Cs::ContentServerEndpoint def;
	REQUIRE(HE::Cs::detail::parseContentServerUrl("http://content.example.internal", def));
	CHECK(def.host == "content.example.internal");
	CHECK(def.port == 80); // httpParseUrl's default when the URL names no port

	HE::Cs::ContentServerEndpoint bad;
	CHECK_FALSE(HE::Cs::detail::parseContentServerUrl("https://example.com", bad));
	CHECK_FALSE(bad.configured());
	CHECK_FALSE(HE::Cs::detail::parseContentServerUrl("not a url", bad));
	CHECK_FALSE(HE::Cs::detail::parseContentServerUrl("", bad));
}

TEST_CASE("contentServerEndpoint: the test override round-trips and is restored afterwards")
{
	const HE::Cs::ContentServerEndpoint original = HE::Cs::contentServerEndpoint();

	HE::Cs::setContentServerEndpointForTesting({ "127.0.0.1", 12345 });
	CHECK(HE::Cs::contentServerEndpoint().host == "127.0.0.1");
	CHECK(HE::Cs::contentServerEndpoint().port == 12345);
	CHECK(HE::Cs::contentServerEndpoint().configured());

	HE::Cs::setContentServerEndpointForTesting({});
	CHECK_FALSE(HE::Cs::contentServerEndpoint().configured());

	HE::Cs::setContentServerEndpointForTesting(original); // leave it as every other case expects it
}

TEST_CASE("contentServerGetManifest: fetches the server's body into localPath")
{
	OneShotHttpServer srv;
	const std::string body = R"({"entries":[{"path":"Materials/Foo.hasset","uuid":{"hi":1,"lo":2},"hash":9,"size":3,"mtime":0}]})";
	const std::string headers =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"\r\n";
	srv.start(headers, [&](HE::Net::SocketHandle client) { srv.sendAll(client, body); });

	const HE::Cs::ContentServerEndpoint endpoint{ "127.0.0.1", srv.port };
	const std::filesystem::path dest =
		std::filesystem::temp_directory_path() / "he_content_server_manifest_test.json";
	std::error_code rmEc;
	std::filesystem::remove(dest, rmEc);

	const HE::Cs::SftpResult r = HE::Cs::contentServerGetManifest(endpoint, dest.string());
	srv.server.join();

	REQUIRE(srv.served.load());
	CHECK(srv.receivedRequest.find("GET /manifest.json HTTP/1.1") != std::string::npos);
	CHECK(r.ok);
	CHECK(r.error.empty());
	REQUIRE(std::filesystem::exists(dest));
	CHECK(readWholeFile(dest) == body);

	std::filesystem::remove(dest, rmEc);
}

TEST_CASE("contentServerGetManifest: a server error is reported, not written to localPath")
{
	OneShotHttpServer srv;
	srv.start("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n",
	          [&](HE::Net::SocketHandle) {});

	const HE::Cs::ContentServerEndpoint endpoint{ "127.0.0.1", srv.port };
	const std::filesystem::path dest =
		std::filesystem::temp_directory_path() / "he_content_server_manifest_error_test.json";
	std::error_code rmEc;
	std::filesystem::remove(dest, rmEc);

	const HE::Cs::SftpResult r = HE::Cs::contentServerGetManifest(endpoint, dest.string());
	srv.server.join();

	CHECK_FALSE(r.ok);
	CHECK(r.error.find("500") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(dest));
}

TEST_CASE("contentServerGetFile: percent-encodes the path, finalizes via rename, no .partial left")
{
	OneShotHttpServer srv;
	const std::string body = "mesh bytes";
	const std::string headers =
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"\r\n";
	srv.start(headers, [&](HE::Net::SocketHandle client) { srv.sendAll(client, body); });

	const HE::Cs::ContentServerEndpoint endpoint{ "127.0.0.1", srv.port };
	const std::filesystem::path dest =
		std::filesystem::temp_directory_path() / "he_content_server_file_test" / "Meshes" / "My Rock.fbx";
	std::error_code rmEc;
	std::filesystem::remove(dest, rmEc);
	std::filesystem::remove(std::filesystem::path(dest.string() + ".partial"), rmEc);

	const HE::Cs::SftpResult r =
		HE::Cs::contentServerGetFile(endpoint, "Meshes/My Rock.fbx", dest.string());
	srv.server.join();

	REQUIRE(srv.served.load());
	// The space in the path must be percent-encoded (the server's own decodePercent expects
	// exactly this), '/' must stay literal.
	CHECK(srv.receivedRequest.find("GET /content/Meshes/My%20Rock.fbx HTTP/1.1") != std::string::npos);

	CHECK(r.ok);
	REQUIRE(std::filesystem::exists(dest));
	CHECK(readWholeFile(dest) == body);
	CHECK_FALSE(std::filesystem::exists(dest.string() + ".partial"));

	std::filesystem::remove(dest, rmEc);
	std::filesystem::remove_all(dest.parent_path().parent_path(), rmEc);
}

TEST_CASE("contentServerGetFile: a failed download leaves neither the final file nor .partial")
{
	OneShotHttpServer srv;
	srv.start("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\n",
	          [&](HE::Net::SocketHandle client) { srv.sendAll(client, "not found"); });

	const HE::Cs::ContentServerEndpoint endpoint{ "127.0.0.1", srv.port };
	const std::filesystem::path dest =
		std::filesystem::temp_directory_path() / "he_content_server_file_missing_test.bin";
	std::error_code rmEc;
	std::filesystem::remove(dest, rmEc);
	std::filesystem::remove(std::filesystem::path(dest.string() + ".partial"), rmEc);

	const HE::Cs::SftpResult r =
		HE::Cs::contentServerGetFile(endpoint, "Gone.bin", dest.string());
	srv.server.join();

	CHECK_FALSE(r.ok);
	CHECK_FALSE(std::filesystem::exists(dest));
	CHECK_FALSE(std::filesystem::exists(dest.string() + ".partial"));
}

// Live check against a real horizon-content-server.exe (step 2's server repo), the same kind of
// case test_sftp_connection.cpp runs for SFTP under HE_TEST_SFTP — skipped unless asked for, so
// CI and a bare `he_tests` run never need a server running.
TEST_CASE("contentServerGetManifest/GetFile: a real horizon-content-server.exe (HE_TEST_CONTENT_SERVER)")
{
	const char* spec = std::getenv("HE_TEST_CONTENT_SERVER"); // "host:port", e.g. "127.0.0.1:18099"
	if (!spec || !*spec) { MESSAGE("HE_TEST_CONTENT_SERVER not set — skipped"); return; }

	const std::string specStr = spec;
	const std::size_t colon = specStr.rfind(':');
	REQUIRE(colon != std::string::npos);
	const HE::Cs::ContentServerEndpoint endpoint{
		specStr.substr(0, colon),
		static_cast<std::uint16_t>(std::stoi(specStr.substr(colon + 1))) };

	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_content_server_live_test";
	std::error_code rmEc;
	std::filesystem::remove_all(dir, rmEc);
	std::filesystem::create_directories(dir, rmEc);

	const HE::Cs::SftpResult manifestResult =
		HE::Cs::contentServerGetManifest(endpoint, (dir / "manifest.json").string());
	REQUIRE_MESSAGE(manifestResult.ok, manifestResult.error);
	HE::Cs::EngineContentManifest manifest;
	REQUIRE(HE::Cs::parseManifest(readWholeFile(dir / "manifest.json"), manifest));
	REQUIRE_FALSE(manifest.entries.empty());

	// Download the first entry the real server's catalogue actually lists — whatever that is,
	// a live server's content_dir is the one piece this test cannot assume.
	const auto& entry = manifest.entries.front();
	const HE::Cs::SftpResult fileResult =
		HE::Cs::contentServerGetFile(endpoint, entry.relativePath, (dir / "downloaded.bin").string());
	REQUIRE_MESSAGE(fileResult.ok, fileResult.error);
	CHECK(std::filesystem::file_size(dir / "downloaded.bin") == entry.size);

	std::filesystem::remove_all(dir, rmEc);
}
