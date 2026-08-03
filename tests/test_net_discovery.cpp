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

// ─── NAT-PMP (RFC 6886) ──────────────────────────────────────────────────────
// The protocol some routers speak instead of UPnP — Apple base stations
// historically only this one. Twelve bytes of binary rather than SOAP over HTTP.

TEST_CASE("NAT-PMP: a mapping request matches the RFC layout")
{
    const auto req = PortMapper::buildNatPmpRequest(2, 7777, 7777, 7200);

    REQUIRE(req.size() == 12);
    CHECK(req[0] == 0);      // version
    CHECK(req[1] == 2);      // opcode 2 = map TCP

    // Everything is big-endian on the wire.
    CHECK(((req[4] << 8) | req[5]) == 7777);    // internal port
    CHECK(((req[6] << 8) | req[7]) == 7777);    // suggested external port
    const std::uint32_t lifetime =
        (req[8] << 24) | (req[9] << 16) | (req[10] << 8) | req[11];
    CHECK(lifetime == 7200);
}

TEST_CASE("NAT-PMP: an address request is only two bytes")
{
    // Opcode 0 carries no payload at all — the whole appeal of this protocol
    // next to UPnP's XML.
    const auto req = PortMapper::buildNatPmpRequest(0, 0, 0, 0);
    REQUIRE(req.size() == 2);
    CHECK(req[0] == 0);
    CHECK(req[1] == 0);
}

TEST_CASE("NAT-PMP: a deletion is a zero lifetime, per the RFC")
{
    const auto req = PortMapper::buildNatPmpRequest(2, 7777, 0, 0);
    CHECK(((req[6] << 8) | req[7]) == 0);   // external port 0
    const std::uint32_t lifetime =
        (req[8] << 24) | (req[9] << 16) | (req[10] << 8) | req[11];
    CHECK(lifetime == 0);
}

TEST_CASE("NAT-PMP: a mapping response is parsed")
{
    // ver, opcode 2+128, result 0, epoch, internal 7777, external 40000, lifetime 3600
    const std::uint8_t reply[16] = {
        0, 130, 0, 0, 0, 0, 0x12, 0x34,
        0x1E, 0x61, 0x9C, 0x40, 0, 0, 0x0E, 0x10
    };

    std::uint16_t internalPort = 0, externalPort = 0, result = 0xFFFF;
    std::uint32_t lifetime = 0;
    REQUIRE(PortMapper::parseNatPmpMapResponse(reply, sizeof(reply), internalPort,
                                               externalPort, lifetime, result));
    CHECK(result == 0);
    CHECK(internalPort == 7777);
    // The router may hand back a DIFFERENT external port than requested; using
    // the requested one would publish an endpoint nobody listens on.
    CHECK(externalPort == 40000);
    CHECK(lifetime == 3600);
}

TEST_CASE("NAT-PMP: a refusal is reported rather than mistaken for success")
{
    // Result code 2 = "not authorised / refused", which is what a router with
    // NAT-PMP switched off answers.
    std::uint8_t reply[16] = { 0, 130, 0, 2 };
    std::uint16_t a = 0, b = 0, result = 0;
    std::uint32_t c = 0;
    REQUIRE(PortMapper::parseNatPmpMapResponse(reply, sizeof(reply), a, b, c, result));
    CHECK(result == 2);
}

TEST_CASE("NAT-PMP: malformed frames are rejected")
{
    std::uint16_t a = 0, b = 0, result = 0;
    std::uint32_t c = 0;

    const std::uint8_t tooShort[8] = { 0, 130 };
    CHECK_FALSE(PortMapper::parseNatPmpMapResponse(tooShort, sizeof(tooShort), a, b, c, result));

    // Version we do not speak.
    const std::uint8_t badVersion[16] = { 9, 130 };
    CHECK_FALSE(PortMapper::parseNatPmpMapResponse(badVersion, sizeof(badVersion), a, b, c, result));

    // A REQUEST opcode where a response was expected — responses have the high
    // bit set, so this is either a loop or a spoof.
    const std::uint8_t notAResponse[16] = { 0, 2 };
    CHECK_FALSE(PortMapper::parseNatPmpMapResponse(notAResponse, sizeof(notAResponse), a, b, c, result));

    CHECK_FALSE(PortMapper::parseNatPmpMapResponse(nullptr, 16, a, b, c, result));
}

TEST_CASE("NAT-PMP: the external address is read out of the response")
{
    const std::uint8_t reply[12] = { 0, 128, 0, 0, 0, 0, 0, 0, 203, 0, 113, 42 };

    std::string ip;
    std::uint16_t result = 0xFFFF;
    REQUIRE(PortMapper::parseNatPmpAddressResponse(reply, sizeof(reply), ip, result));
    CHECK(result == 0);
    CHECK(ip == "203.0.113.42");
}

TEST_CASE("NAT-PMP: a CGNAT external address is recognised as unreachable")
{
    // A router can happily map a port and still be behind carrier-grade NAT,
    // in which case no forward at this level can ever be reached.
    const std::uint8_t reply[12] = { 0, 128, 0, 0, 0, 0, 0, 0, 100, 90, 1, 5 };
    std::string ip;
    std::uint16_t result = 0;
    REQUIRE(PortMapper::parseNatPmpAddressResponse(reply, sizeof(reply), ip, result));
    CHECK(ip == "100.90.1.5");
    CHECK(PortMapper::isPrivateOrCgnat(ip));
}

TEST_CASE("socketDefaultGateway: reports a usable router address")
{
    const std::string gw = socketDefaultGateway();
    // A machine with no default route legitimately has none.
    if (gw.empty()) return;

    CHECK(gw.find('.') != std::string::npos);
    CHECK(gw != "0.0.0.0");
    // The gateway is by definition on a local network.
    CHECK(PortMapper::isPrivateOrCgnat(gw));
}

// ─── Telling "the router said no" from "we never reached the router" ─────────

TEST_CASE("A blocked local network is a distinct outcome from an absent router")
{
    // These two arrive at the same place — no mapping — but call for completely
    // different action, so they must not share a result code. Blaming the router
    // for a permission problem sends people to change settings that were never
    // consulted.
    //
    // Found in the field: on macOS every LAN destination fails with EHOSTUNREACH
    // ("no route to host") for the very gateway that IS the default route and
    // through which all internet traffic is flowing, unless the app holds the
    // Local Network permission. Nothing leaves the machine, and the old message
    // said the router had refused.
    CHECK(HE::Net::PortMapResult::LocalNetworkBlocked !=
          HE::Net::PortMapResult::NoRouterFound);

    // socketLocalNetworkBlocked() must never claim "blocked" when it simply has
    // nothing to test against — "cannot tell" reported as a fault would be worse
    // than saying nothing, because it names a cause that may not exist.
    const std::string gw = HE::Net::socketDefaultGateway();
    if (gw.empty())
    {
        CHECK_FALSE(HE::Net::socketLocalNetworkBlocked());
    }
    else
    {
        // With a gateway present the answer is environment-dependent — a machine
        // with the permission granted reports false, one without reports true —
        // so only its consistency is asserted here, not its value.
        const bool first  = HE::Net::socketLocalNetworkBlocked();
        const bool second = HE::Net::socketLocalNetworkBlocked();
        CHECK(first == second);
    }
}
