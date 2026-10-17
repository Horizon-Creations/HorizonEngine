#include "doctest.h"

#include <ContentSync/SftpClient.h>
#include <Net/Socket.h>

#include <chrono>
#include <future>
#include <thread>

// CSY-102: only the TCP connect had a time limit. A server that accepts the
// connection and then says nothing — a hung sshd, a middlebox swallowing the
// stream — held the calling worker in libssh2's blocking handshake forever, and
// with it the whole EngineContent download queue (EngineContentSync.h: the asset
// stays "materializing"). SftpEndpoint::sessionTimeoutMs now bounds every
// blocking SSH call.

using namespace HE;

TEST_CASE("SFTP: a server that accepts and never speaks fails the call after the session timeout")
{
	// 127.0.0.1 only: a wildcard listener raises the firewall prompt on Windows.
	// Nobody accepts — the kernel completes the handshake from the backlog, which
	// is exactly a server that took the connection and went silent.
	Net::socketSystemInit();
	Net::SocketHandle listener = Net::socketCreateListenerLoopback(0);
	REQUIRE(listener != Net::kInvalidSocket);
	const std::uint16_t port = Net::socketBoundPort(listener);
	REQUIRE(port != 0);

	Cs::SftpEndpoint ep;
	ep.host             = "127.0.0.1";
	ep.port             = port;
	ep.username         = "nobody";
	ep.password         = "nothing";
	ep.sessionTimeoutMs = 500;

	// On a thread of its own, so a regression fails here instead of hanging the
	// suite: closing the listener below resets the queued connection, which
	// unblocks even the old, unbounded handshake. Called directly, not through
	// sftpWithConnectRetry — a handshake timeout is transient and would be retried.
	std::promise<Cs::SftpResult> done;
	std::future<Cs::SftpResult>  result = done.get_future();
	const auto start = std::chrono::steady_clock::now();
	std::thread worker([&] { done.set_value(Cs::sftpTestConnection(ep)); });

	const bool finished = result.wait_for(std::chrono::seconds(15)) == std::future_status::ready;
	const auto elapsed  = std::chrono::steady_clock::now() - start;
	Net::socketClose(listener);
	worker.join();

	REQUIRE(finished);
	CHECK(elapsed < std::chrono::seconds(10));
	const Cs::SftpResult r = result.get();
	CHECK_FALSE(r.ok);
	CHECK(r.transient);   // a silent server is worth another try; a refused one would not be
	CHECK_FALSE(r.error.empty());
}
