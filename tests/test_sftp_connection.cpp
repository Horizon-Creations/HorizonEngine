#include "doctest.h"

#include <ContentSync/SftpClient.h>
#include <Net/Socket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// CSY-101 (docs/audit/modul_contentsync.md): SftpConnection keeps one TCP+SSH+SFTP
// session across the operations of a batch (publish, manifest rebuild, download
// queue), where every free function pays a handshake of its own.
//
// What runs everywhere: the paths that need no server — a missing endpoint, a
// missing local file (no handshake at all), a peer that drops the connection
// during the handshake (a fresh session's failure is final, never retried).
//
// What needs a server: HE_TEST_SFTP=host:port:user:password:remoteDir points the
// last case at a real SFTP account (a throwaway local one, never the shared
// EngineContent library: the case writes files). It checks that a whole batch
// runs on one session and prints the time against the one-shot functions.

namespace fs = std::filesystem;
using namespace HE::Cs;

namespace {

fs::path scratch(const char* stem)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path p = fs::temp_directory_path() / (std::string("he-sftp-") + stem + "-" + std::to_string(stamp));
	fs::create_directories(p);
	return p;
}

void writeText(const fs::path& p, const std::string& text)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}

std::string readText(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Accepts on 127.0.0.1 and closes every connection at once: a server that goes
// away in the middle of the SSH handshake.
struct DroppingListener
{
	HE::Net::SocketHandle listener = HE::Net::kInvalidSocket;
	std::uint16_t         port     = 0;
	std::atomic<bool>     stop{ false };
	std::atomic<int>      accepted{ 0 };
	std::thread           thread;

	DroppingListener()
	{
		HE::Net::socketSystemInit();
		listener = HE::Net::socketCreateListenerLoopback(0);
		port     = HE::Net::socketBoundPort(listener);
		thread   = std::thread([this] {
			while (!stop)
			{
				HE::Net::SocketHandle s = HE::Net::kInvalidSocket;
				if (HE::Net::socketAccept(listener, s) == HE::Net::SocketResult::Ok)
				{
					++accepted;
					HE::Net::socketClose(s);
					continue;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		});
	}
	~DroppingListener()
	{
		stop = true;
		thread.join();
		HE::Net::socketClose(listener);
	}
};

} // namespace

TEST_CASE("SftpConnection: no endpoint, no session")
{
	SftpConnection c{ SftpEndpoint{} };
	CHECK_FALSE(c.isOpen());
	const fs::path dir = scratch("noendpoint");
	const SftpResult r = c.getFile("manifest.json", (dir / "m.json").string());
	CHECK_FALSE(r.ok);
	CHECK_FALSE(r.transient);
	CHECK(r.error.find("not configured") != std::string::npos);
	CHECK_FALSE(c.isOpen());
	CHECK(c.sessionsOpened() == 1);

	// The one-shot function says the same.
	CHECK(sftpGetFile(SftpEndpoint{}, "manifest.json", (dir / "m.json").string()).error == r.error);
	std::error_code ec;
	fs::remove_all(dir, ec);
}

TEST_CASE("SftpConnection: a missing local file costs no handshake")
{
	SftpEndpoint e;
	e.host     = "127.0.0.1";
	e.port     = 1;   // nothing may be dialled at all
	e.username = "nobody";
	SftpConnection c(e);
	const SftpResult r = c.putFile((fs::temp_directory_path() / "he-sftp-does-not-exist.bin").string(),
	                               "x.bin");
	CHECK_FALSE(r.ok);
	CHECK(r.error.find("Could not open local file") != std::string::npos);
	CHECK(c.sessionsOpened() == 0);
}

TEST_CASE("SftpConnection: a handshake the peer drops fails once, transient, without a retry")
{
	DroppingListener server;
	REQUIRE(server.port != 0);
	SftpEndpoint e;
	e.host     = "127.0.0.1";
	e.port     = server.port;
	e.username = "nobody";
	e.password = "secret";

	SftpConnection c(e);
	const fs::path dir = scratch("dropped");
	const SftpResult r = c.getFile("manifest.json", (dir / "m.json").string());
	CHECK_FALSE(r.ok);
	CHECK(r.transient);   // a dropped handshake is what sftpWithConnectRetry retries
	CHECK(r.error.find("secret") == std::string::npos);
	CHECK_FALSE(c.isOpen());
	// A fresh session's failure is final: one attempt, not two.
	CHECK(c.sessionsOpened() == 1);
	CHECK_FALSE(fs::exists(dir / "m.json"));
	CHECK_FALSE(fs::exists(dir / "m.json.partial"));

	// The next operation tries again with a session of its own.
	std::vector<RemoteFileInfo> listed;
	CHECK_FALSE(c.listRemoteTree(listed).ok);
	CHECK(c.sessionsOpened() == 2);
	std::error_code ec;
	fs::remove_all(dir, ec);
}

TEST_CASE("SftpConnection: a batch on one session against a real server (HE_TEST_SFTP)")
{
	const char* spec = std::getenv("HE_TEST_SFTP");
	if (!spec || !*spec) { MESSAGE("HE_TEST_SFTP not set — skipped"); return; }

	// host:port:user:password:remoteDir
	std::vector<std::string> f;
	{
		std::string cur;
		for (const char* p = spec; *p; ++p)
		{
			if (*p == ':' && f.size() < 4) { f.push_back(cur); cur.clear(); }
			else cur.push_back(*p);
		}
		f.push_back(cur);
	}
	REQUIRE(f.size() == 5);
	SftpEndpoint e;
	e.host           = f[0];
	e.port           = static_cast<std::uint16_t>(std::stoi(f[1]));
	e.username       = f[2];
	e.password       = f[3];
	e.remoteBasePath = f[4];

	constexpr int kFiles = 20;
	const fs::path dir = scratch("live");
	for (int i = 0; i < kFiles; ++i)
		writeText(dir / ("f" + std::to_string(i) + ".hasset"), "payload " + std::to_string(i));

	// Upload, list and download the batch through one connection.
	const auto t0 = std::chrono::steady_clock::now();
	{
		SftpConnection c(e);
		for (int i = 0; i < kFiles; ++i)
		{
			const std::string rel = "batch/sub" + std::to_string(i % 3) + "/f" + std::to_string(i) + ".hasset";
			REQUIRE(c.ensureRemoteDir("batch/sub" + std::to_string(i % 3)).ok);
			REQUIRE(c.putFile((dir / ("f" + std::to_string(i) + ".hasset")).string(), rel).ok);
		}
		std::vector<RemoteFileInfo> listed;
		REQUIRE(c.listRemoteTree(listed).ok);
		int inBatch = 0;
		for (const RemoteFileInfo& r : listed)
			if (r.relativePath.rfind("batch/", 0) == 0) ++inBatch;
		CHECK(inBatch == kFiles);
		for (int i = 0; i < kFiles; ++i)
		{
			const std::string rel = "batch/sub" + std::to_string(i % 3) + "/f" + std::to_string(i) + ".hasset";
			const fs::path back = dir / ("back" + std::to_string(i));
			REQUIRE(c.getFile(rel, back.string()).ok);
			CHECK(readText(back) == "payload " + std::to_string(i));
		}
		// A missing file is the server saying no, not a broken connection: the
		// session stays.
		CHECK_FALSE(c.getFile("batch/no-such-file", (dir / "none").string()).ok);
		CHECK(c.isOpen());
		CHECK(c.getFile("batch/sub0/f0.hasset", (dir / "again").string()).ok);
		CHECK(c.sessionsOpened() == 1);
	}
	const auto t1 = std::chrono::steady_clock::now();

	// The same downloads through the one-shot functions: a handshake each.
	for (int i = 0; i < kFiles; ++i)
	{
		const std::string rel = "batch/sub" + std::to_string(i % 3) + "/f" + std::to_string(i) + ".hasset";
		REQUIRE(sftpGetFile(e, rel, (dir / ("once" + std::to_string(i))).string()).ok);
	}
	const auto t2 = std::chrono::steady_clock::now();
	{
		SftpConnection c(e);
		for (int i = 0; i < kFiles; ++i)
		{
			const std::string rel = "batch/sub" + std::to_string(i % 3) + "/f" + std::to_string(i) + ".hasset";
			REQUIRE(c.getFile(rel, (dir / ("conn" + std::to_string(i))).string()).ok);
		}
	}
	const auto t3 = std::chrono::steady_clock::now();
	using ms = std::chrono::duration<double, std::milli>;
	MESSAGE("SftpConnection: " << kFiles << " dirs+puts, list, " << kFiles + 2 << " gets on one session: "
	        << ms(t1 - t0).count() << " ms; " << kFiles << " gets one-shot: " << ms(t2 - t1).count()
	        << " ms; same gets on one connection: " << ms(t3 - t2).count() << " ms");

	std::error_code ec;
	fs::remove_all(dir, ec);
}
