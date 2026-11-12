#pragma once

// ─── A one-shot loopback HTTP server for exercising a real client end-to-end ──
// Accepts exactly one connection, reads a request (until the header block ends),
// sends a fixed response built from `headers` + whatever `sendBody` writes, then
// closes — letting a test see a truncated/short body without any buildResponse-
// style helper hiding the mismatch between what it sends and what Content-Length
// claims. Shared by test_net_discovery.cpp (HE::Net::httpRequest/httpDownloadToFile)
// and test_content_server_client.cpp (HE::Cs::contentServerGetManifest/GetFile) —
// both drive the same kind of client against the same kind of fake server.

#include <Net/Socket.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace HE::Test {

struct OneShotHttpServer
{
	HE::Net::SocketHandle listener = HE::Net::kInvalidSocket;
	std::uint16_t         port     = 0;
	std::thread           server;
	std::atomic<bool>     served{ false };
	std::string           receivedRequest;

	void start(const std::string& headers, std::function<void(HE::Net::SocketHandle)> sendBody)
	{
		using namespace HE::Net;
		listener = socketCreateTcp();
		if (listener == kInvalidSocket) return;
		if (!socketBindListen(listener, 0)) return;
		port = socketBoundPort(listener);
		if (port == 0) return;

		server = std::thread([this, headers, sendBody = std::move(sendBody)]
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			SocketHandle client = kInvalidSocket;
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (socketAccept(listener, client) == SocketResult::Ok) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			if (client == kInvalidSocket) return;

			std::uint8_t buf[4096];
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (!socketWaitReadable(client, 200)) continue;
				std::size_t got = 0;
				if (socketRecv(client, buf, sizeof(buf), got) != SocketResult::Ok) break;
				receivedRequest.append(reinterpret_cast<char*>(buf), got);
				if (receivedRequest.find("\r\n\r\n") != std::string::npos) break;
			}

			if (!sendAll(client, headers)) { socketClose(client); return; }
			sendBody(client);
			socketClose(client);
			served = true;
		});
	}

	// Exposed for `sendBody` callbacks that need to send more than one piece
	// (e.g. send a short body then stop, to exercise a truncation path).
	bool sendAll(HE::Net::SocketHandle client, const std::string& data)
	{
		using namespace HE::Net;
		std::size_t off = 0;
		while (off < data.size())
		{
			std::size_t sent = 0;
			const SocketResult sr = socketSend(
				client, reinterpret_cast<const std::uint8_t*>(data.data()) + off,
				data.size() - off, sent);
			if (sr == SocketResult::Ok)         { off += sent; continue; }
			if (sr == SocketResult::WouldBlock) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
			return false;
		}
		return true;
	}

	~OneShotHttpServer()
	{
		if (server.joinable()) server.join();
		if (listener != HE::Net::kInvalidSocket) HE::Net::socketClose(listener);
	}
};

} // namespace HE::Test
