#include "doctest.h"

#include <Net/HttpClient.h>
#include <Net/PortMapper.h>
#include <Net/Socket.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace HE::Net;

// ─── URL parsing ─────────────────────────────────────────────────────────────

TEST_CASE("httpParseUrl: host, port and path")
{
    HttpUrl u;
    REQUIRE(httpParseUrl("http://192.168.1.1:5000/rootDesc.xml", u));
    CHECK(u.host == "192.168.1.1");
    CHECK(u.port == 5000);
    CHECK(u.path == "/rootDesc.xml");

    REQUIRE(httpParseUrl("http://router.local/desc", u));
    CHECK(u.host == "router.local");
    CHECK(u.port == 80);          // default when omitted
    CHECK(u.path == "/desc");

    REQUIRE(httpParseUrl("http://10.0.0.1", u));
    CHECK(u.path == "/");         // empty path normalises to root
}

TEST_CASE("httpParseUrl: https is rejected rather than silently downgraded")
{
    HttpUrl u;
    // This client has no TLS. Stripping the scheme and sending plaintext to a
    // TLS port would be far worse than failing.
    CHECK_FALSE(httpParseUrl("https://example.com/api", u));
    CHECK_FALSE(httpParseUrl("ftp://example.com", u));
    CHECK_FALSE(httpParseUrl("example.com", u));
    CHECK_FALSE(httpParseUrl("http://", u));
    CHECK_FALSE(httpParseUrl("http://host:99999/x", u));   // port out of range
}

// ─── Response parsing ────────────────────────────────────────────────────────

TEST_CASE("httpParseResponse: status and Content-Length body")
{
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/xml\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello-trailing-garbage";

    HttpResponse r;
    REQUIRE(httpParseResponse(raw, r));
    CHECK(r.ok);
    CHECK(r.statusCode == 200);
    CHECK(r.body == "hello");   // truncated to the advertised length
}

TEST_CASE("httpParseResponse: chunked transfer encoding is decoded")
{
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n";

    HttpResponse r;
    REQUIRE(httpParseResponse(raw, r));
    CHECK(r.body == "hello world");
}

TEST_CASE("httpParseResponse: header lookup is case-insensitive")
{
    const std::string raw =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "CONTENT-LENGTH: 3\r\n"
        "\r\n"
        "abcdef";

    HttpResponse r;
    REQUIRE(httpParseResponse(raw, r));
    CHECK(r.statusCode == 500);
    CHECK(r.body == "abc");
}

TEST_CASE("httpParseResponse: malformed input is rejected")
{
    HttpResponse r;
    CHECK_FALSE(httpParseResponse("no header terminator", r));
    CHECK_FALSE(httpParseResponse("garbage\r\n\r\nbody", r));
}

// ─── SSDP ────────────────────────────────────────────────────────────────────

TEST_CASE("PortMapper: SSDP search targets gateways")
{
    const std::string s = PortMapper::buildSsdpSearch();
    CHECK(s.find("M-SEARCH * HTTP/1.1") != std::string::npos);
    CHECK(s.find("239.255.255.250:1900") != std::string::npos);
    CHECK(s.find("ssdp:discover") != std::string::npos);
    CHECK(s.find("InternetGatewayDevice") != std::string::npos);
    CHECK(s.substr(s.size() - 4) == "\r\n\r\n");   // must end with a blank line
}

TEST_CASE("PortMapper: LOCATION header is extracted case-insensitively")
{
    const std::string reply =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "location: http://192.168.1.1:5000/rootDesc.xml\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";
    CHECK(PortMapper::parseSsdpLocation(reply) == "http://192.168.1.1:5000/rootDesc.xml");

    CHECK(PortMapper::parseSsdpLocation("HTTP/1.1 200 OK\r\nST: x\r\n\r\n").empty());
}

// ─── Device description ──────────────────────────────────────────────────────

namespace {
// Trimmed-down but structurally faithful IGD description: several services, the
// WAN one deliberately not first, so a naive "find any controlURL" would pick
// the wrong endpoint.
const char* kIgdXml = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <controlURL>/ctl/L3F</controlURL>
      </service>
    </serviceList>
    <deviceList><device>
      <serviceList>
        <service>
          <serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
          <controlURL>/ctl/CommonIfCfg</controlURL>
        </service>
        <service>
          <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
          <controlURL>/ctl/IPConn</controlURL>
        </service>
      </serviceList>
    </device></deviceList>
  </device>
</root>)";
} // namespace

TEST_CASE("PortMapper: the WAN service's own control URL is selected")
{
    IgdDevice igd;
    REQUIRE(PortMapper::parseDeviceDescription(
        kIgdXml, "http://192.168.1.1:5000/rootDesc.xml", igd));

    CHECK(igd.serviceType == "urn:schemas-upnp-org:service:WANIPConnection:1");
    // Must be the WAN service's URL, not Layer3Forwarding's or CommonIfCfg's.
    CHECK(igd.controlUrl == "http://192.168.1.1:5000/ctl/IPConn");
}

TEST_CASE("PortMapper: a description without a WAN service fails cleanly")
{
    const char* xml = R"(<root><device><serviceList><service>
        <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
        <controlURL>/ctl/L3F</controlURL>
      </service></serviceList></device></root>)";
    IgdDevice igd;
    CHECK_FALSE(PortMapper::parseDeviceDescription(xml, "http://192.168.1.1:5000/d.xml", igd));
}

TEST_CASE("PortMapper: relative and absolute control URLs both resolve")
{
    CHECK(PortMapper::resolveUrl("http://192.168.1.1:5000/rootDesc.xml", "/ctl/IPConn")
          == "http://192.168.1.1:5000/ctl/IPConn");
    CHECK(PortMapper::resolveUrl("http://192.168.1.1:5000/rootDesc.xml", "ctl/IPConn")
          == "http://192.168.1.1:5000/ctl/IPConn");
    CHECK(PortMapper::resolveUrl("http://192.168.1.1:5000/d.xml", "http://10.0.0.1/x")
          == "http://10.0.0.1/x");
    CHECK(PortMapper::resolveUrl("http://192.168.1.1/d.xml", "").empty());
}

// ─── SOAP ────────────────────────────────────────────────────────────────────

TEST_CASE("PortMapper: SOAP body carries action, service type and arguments")
{
    const std::string body = PortMapper::buildSoapBody(
        "urn:schemas-upnp-org:service:WANIPConnection:1", "AddPortMapping",
        { { "NewExternalPort", "7777" }, { "NewProtocol", "TCP" } });

    CHECK(body.find("<u:AddPortMapping") != std::string::npos);
    CHECK(body.find("urn:schemas-upnp-org:service:WANIPConnection:1") != std::string::npos);
    CHECK(body.find("<NewExternalPort>7777</NewExternalPort>") != std::string::npos);
    CHECK(body.find("<NewProtocol>TCP</NewProtocol>") != std::string::npos);
    CHECK(body.find("</s:Envelope>") != std::string::npos);
}

TEST_CASE("PortMapper: SOAP arguments are XML-escaped")
{
    // Session names reach this as a mapping description, so unescaped markup
    // would corrupt the request (or worse, inject elements).
    const std::string body = PortMapper::buildSoapBody(
        "urn:test", "AddPortMapping",
        { { "NewPortMappingDescription", "Anna & <Bob>'s \"session\"" } });

    CHECK(body.find("Anna &amp; &lt;Bob&gt;&apos;s &quot;session&quot;") != std::string::npos);
    CHECK(body.find("<Bob>") == std::string::npos);
}

TEST_CASE("PortMapper: values are read back out of a SOAP response")
{
    const std::string resp =
        "<?xml version=\"1.0\"?><s:Envelope><s:Body>"
        "<u:GetExternalIPAddressResponse>"
        "<NewExternalIPAddress>203.0.113.42</NewExternalIPAddress>"
        "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";

    CHECK(PortMapper::extractXmlValue(resp, "NewExternalIPAddress") == "203.0.113.42");
    CHECK(PortMapper::extractXmlValue(resp, "Missing").empty());
}

// ─── CGNAT detection ─────────────────────────────────────────────────────────

TEST_CASE("PortMapper: unreachable address ranges are recognised")
{
    // CGNAT — the case where no port mapping can ever work.
    CHECK(PortMapper::isPrivateOrCgnat("100.64.0.1"));
    CHECK(PortMapper::isPrivateOrCgnat("100.127.255.254"));

    CHECK(PortMapper::isPrivateOrCgnat("10.0.0.1"));
    CHECK(PortMapper::isPrivateOrCgnat("192.168.1.1"));
    CHECK(PortMapper::isPrivateOrCgnat("172.16.0.1"));
    CHECK(PortMapper::isPrivateOrCgnat("172.31.255.255"));
    CHECK(PortMapper::isPrivateOrCgnat("127.0.0.1"));
    CHECK(PortMapper::isPrivateOrCgnat("169.254.1.1"));

    // Genuinely routable — a mapping here can be reachable.
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("203.0.113.42"));
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("8.8.8.8"));
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("172.32.0.1"));   // just outside RFC1918
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("100.63.255.255"));// just below CGNAT
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("100.128.0.0"));  // just above CGNAT
    CHECK_FALSE(PortMapper::isPrivateOrCgnat("not-an-ip"));
}

// ─── Local address ───────────────────────────────────────────────────────────

TEST_CASE("socketLocalAddress: reports a usable LAN address")
{
    const std::string ip = socketLocalAddress();
    // Sandboxes without a route to the internet legitimately yield nothing.
    if (ip.empty()) return;

    CHECK(ip.find('.') != std::string::npos);
    CHECK(ip != "0.0.0.0");
}

// ─── UDP round-trip ──────────────────────────────────────────────────────────

TEST_CASE("UDP sockets: datagram round-trips over loopback with sender address")
{
    SocketHandle receiver = socketCreateUdp();
    REQUIRE(receiver != kInvalidSocket);
    REQUIRE(socketBindUdp(receiver, 0));

    // Reuse the TCP helper to learn the bound port — getsockname is protocol
    // agnostic.
    const std::uint16_t port = socketBoundPort(receiver);
    REQUIRE(port != 0);

    SocketHandle sender = socketCreateUdp();
    REQUIRE(sender != kInvalidSocket);

    const std::string msg = "M-SEARCH-ish";
    std::size_t sent = 0;
    REQUIRE(socketSendTo(sender, reinterpret_cast<const std::uint8_t*>(msg.data()),
                         msg.size(), "127.0.0.1", port, sent) == SocketResult::Ok);
    CHECK(sent == msg.size());

    REQUIRE(socketWaitReadable(receiver, 2000));

    std::uint8_t buf[256];
    std::size_t got = 0;
    std::string fromHost;
    std::uint16_t fromPort = 0;
    REQUIRE(socketRecvFrom(receiver, buf, sizeof(buf), got, fromHost, fromPort)
            == SocketResult::Ok);

    CHECK(std::string(reinterpret_cast<char*>(buf), got) == msg);
    CHECK(fromHost == "127.0.0.1");   // this is how SSDP locates the router
    CHECK(fromPort != 0);

    socketClose(sender);
    socketClose(receiver);
}

// ─── HTTP against a real server ──────────────────────────────────────────────

TEST_CASE("httpRequest: performs a real GET over a loopback socket")
{
    // A one-shot HTTP server on a background thread, so the client is exercised
    // end-to-end (connect, request framing, response parse) rather than only its
    // pure helpers.
    SocketHandle listener = socketCreateTcp();
    REQUIRE(listener != kInvalidSocket);
    REQUIRE(socketBindListen(listener, 0));
    const std::uint16_t port = socketBoundPort(listener);
    REQUIRE(port != 0);

    std::atomic<bool> served{ false };
    std::string       receivedRequest;

    std::thread server([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        SocketHandle client = kInvalidSocket;
        while (std::chrono::steady_clock::now() < deadline) {
            if (socketAccept(listener, client) == SocketResult::Ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (client == kInvalidSocket) return;

        std::uint8_t buf[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            if (!socketWaitReadable(client, 200)) continue;
            std::size_t got = 0;
            if (socketRecv(client, buf, sizeof(buf), got) != SocketResult::Ok) break;
            receivedRequest.append(reinterpret_cast<char*>(buf), got);
            if (receivedRequest.find("\r\n\r\n") != std::string::npos) break;
        }

        const std::string payload = "<root><ok/></root>";
        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/xml\r\n"
            "Content-Length: " + std::to_string(payload.size()) + "\r\n"
            "\r\n" + payload;

        std::size_t off = 0;
        while (off < resp.size()) {
            std::size_t sent = 0;
            const SocketResult sr = socketSend(
                client, reinterpret_cast<const std::uint8_t*>(resp.data()) + off,
                resp.size() - off, sent);
            if (sr == SocketResult::Ok)         { off += sent; continue; }
            if (sr == SocketResult::WouldBlock) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            break;
        }
        // Closing signals the end of the body to a Connection: close client.
        socketClose(client);
        served = true;
    });

    const HttpResponse r = httpGet("http://127.0.0.1:" + std::to_string(port) + "/rootDesc.xml", 5000);
    server.join();
    socketClose(listener);

    REQUIRE(served.load());
    CHECK(receivedRequest.find("GET /rootDesc.xml HTTP/1.1") != std::string::npos);
    CHECK(receivedRequest.find("Host: 127.0.0.1") != std::string::npos);

    CHECK(r.ok);
    CHECK(r.statusCode == 200);
    CHECK(r.body == "<root><ok/></root>");
}

TEST_CASE("httpRequest: a refused connection fails without hanging")
{
    // Bind then release, so the port is free but nothing listens.
    std::uint16_t deadPort = 0;
    {
        SocketHandle probe = socketCreateTcp();
        REQUIRE(probe != kInvalidSocket);
        REQUIRE(socketBindListen(probe, 0));
        deadPort = socketBoundPort(probe);
        socketClose(probe);
    }

    const auto start = std::chrono::steady_clock::now();
    const HttpResponse r = httpGet("http://127.0.0.1:" + std::to_string(deadPort) + "/x", 2000);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK_FALSE(r.ok);
    CHECK(!r.error.empty());
    CHECK(elapsed < std::chrono::seconds(5));   // bounded by the timeout
}
