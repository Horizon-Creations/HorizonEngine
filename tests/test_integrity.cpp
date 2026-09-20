#include "doctest.h"

#include <Crypto/Sha256.h>
#include <Integrity/IntegrityProbe.h>
#include <Hpak/HpakReader.h>
#include <Hpak/HpakWriter.h>
#include <HorizonScene/AntiCheat/AntiCheatService.h>
#include <HorizonScene/GameReplication.h>
#include <HorizonScene/HorizonWorld.h>

#include <Diagnostics/Log.h>
#include <Net/LoopbackTransport.h>
#include <Net/NetSession.h>

#include "TestFsUtil.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace HE::Net;
using namespace HE::Integrity;
using HE::AntiCheat::AntiCheatService;
using HE::AntiCheat::Kind;
using HE::AntiCheat::Level;

// ─── Anti-cheat, step 6: the integrity manifest (docs/anti-cheat-plan.md §3.5) ─
// Three things are pinned here. The SHA-256 the probe hashes files with (known
// answers, streaming = one-shot, so the KeyDerivation refactor cannot have bent
// it). The probe and the comparison over real files in a temp directory,
// including a real .hpak written twice with different content. And the
// exchange at join over LoopbackTransport: identical builds produce no
// observation, a manipulated pak produces IntegrityMismatch on the host, and
// a dev build with no manifest switches the check off without a crash.

namespace {

constexpr MessageId kMsgIntegrity = kFirstUserMessage + 202;
constexpr float     kFrame        = 1.0f / 60.0f;

std::string hexOf(const void* data, std::size_t len)
{
    std::uint8_t d[HE::Crypto::Sha256::kDigestSize];
    HE::Crypto::Sha256::digest(data, len, d);
    return HE::Crypto::Sha256::toHex(d);
}

struct TempDir
{
    std::filesystem::path path;
    explicit TempDir(const char* tag)
        : path(std::filesystem::temp_directory_path() / ("he_test_integrity_" + std::string(tag)))
    {
        he_test::removeAllQuiet(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { he_test::removeAllQuiet(path); }

    std::filesystem::path write(const char* name, const std::string& bytes) const
    {
        const auto p = path / name;
        std::ofstream f(p, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

// A minimal pak with one entry whose content is `payload`. Two paks with
// different payloads have different content hashes, therefore different TOCs,
// therefore different tocHash values — which is exactly what the manifest
// carries for a pak.
std::uint64_t writePak(const std::filesystem::path& file, const std::string& payload)
{
    HpakWriter w;
    const HE::UUID id{ 0x1111222233334444ULL, 0x5555666677778888ULL };
    std::vector<uint8_t> blob(payload.begin(), payload.end());
    w.addEntry(id, blob);
    REQUIRE(w.write(file.string()));
    HpakReader r;
    REQUIRE(r.open(file.string()));
    return r.tocHash();
}

Manifest sampleManifest(const char* pakHash = "00000000deadbeef")
{
    Manifest m;
    m.entries.push_back({ FileKind::Executable, "HorizonGame", std::string(64, 'a') });
    m.entries.push_back({ FileKind::Library, "libHorizonCore.dylib", std::string(64, 'b') });
    m.entries.push_back({ FileKind::Library, "libHorizonScene.dylib", std::string(64, 'c') });
    m.entries.push_back({ FileKind::Pak, "Game.hpak", pakHash });
    return m;
}

struct Rig
{
    std::unique_ptr<LoopbackTransport> hostT, clientT;
    std::unique_ptr<NetSession>        hostNet, clientNet;
    std::unique_ptr<GameReplication>   host, client;
    HorizonWorld hostWorld, clientWorld;
    AntiCheatService svc;

    void frame()
    {
        hostT->update();
        clientT->update();
        hostNet->pump();
        clientNet->pump();
        host->update(kFrame);
        client->update(kFrame);
    }
    void frames(int n) { for (int i = 0; i < n; ++i) frame(); }
};

struct RigOptions
{
    bool attachService = true;
    // nullopt = "this side has no manifest" (dev build); unset = adopt the
    // process-wide probe (whatever state it is in).
    std::optional<std::optional<Manifest>> hostManifest;
    std::optional<std::optional<Manifest>> clientManifest;
    HE::AntiCheat::Config    config;
    GameReplication::Config  repl;
};

std::unique_ptr<Rig> makeRig(RigOptions opt)
{
    auto r = std::make_unique<Rig>();
    r->svc = AntiCheatService(opt.config);
    auto [a, b] = LoopbackTransport::createPair();
    r->hostT   = std::move(a);
    r->clientT = std::move(b);
    r->hostNet   = std::make_unique<NetSession>(r->hostT.get(), NetRole::Host);
    r->clientNet = std::make_unique<NetSession>(r->clientT.get(), NetRole::Client);
    r->host   = std::make_unique<GameReplication>(r->hostNet.get(), NetRole::Host, opt.repl);
    r->client = std::make_unique<GameReplication>(r->clientNet.get(), NetRole::Client, opt.repl);
    r->host->setWorld(&r->hostWorld);
    r->client->setWorld(&r->clientWorld);
    if (opt.attachService) r->host->setAntiCheat(&r->svc);
    if (opt.hostManifest)   r->host->setLocalManifest(*opt.hostManifest);
    if (opt.clientManifest) r->client->setLocalManifest(*opt.clientManifest);
    // Establish the link (Connected events) without ticking replication, so a
    // test can put a hand-written frame on the wire BEFORE the client's own
    // manifest goes out.
    r->hostT->update(); r->clientT->update();
    r->hostNet->pump(); r->clientNet->pump();
    return r;
}

// The logger folds consecutive identical lines into "repeated N times". Two
// rigs in a row both saying "integrity check off" would count as one, so a
// test that counts that line puts a distinct line between them first.
void breakRepeatChain()
{
    static int n = 0;
    HE_LOG_INFO(AntiCheat, "test separator %d", ++n);
}

class LogSpy
{
public:
    LogSpy() : m_previous(HE::Log::verbosity(HE::Log::Cat::AntiCheat))
    {
        HE::Log::setVerbosity(HE::Log::Cat::AntiCheat, HE::LogLevel::Trace);
        m_handle = HE::Log::addSink(&LogSpy::onRecord, this);
    }
    ~LogSpy()
    {
        HE::Log::removeSink(m_handle);
        HE::Log::setVerbosity(HE::Log::Cat::AntiCheat, m_previous);
    }
    LogSpy(const LogSpy&)            = delete;
    LogSpy& operator=(const LogSpy&) = delete;

    std::size_t count(const std::string& needle) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::size_t n = 0;
        for (const std::string& line : m_lines)
            if (line.find(needle) != std::string::npos) ++n;
        return n;
    }

private:
    static void onRecord(const HE::Log::Record& rec, void* user)
    {
        if (rec.category != HE::Log::Cat::AntiCheat) return;
        auto* self = static_cast<LogSpy*>(user);
        std::lock_guard<std::mutex> lk(self->m_mutex);
        self->m_lines.emplace_back(rec.message ? rec.message : "");
    }
    mutable std::mutex       m_mutex;
    std::vector<std::string> m_lines;
    int                      m_handle = 0;
    HE::LogLevel             m_previous;
};

} // namespace

// ─── SHA-256 ─────────────────────────────────────────────────────────────────

TEST_CASE("Integrity: SHA-256 known answers, and streaming equals one-shot")
{
    CHECK(hexOf("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexOf("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Two blocks plus padding into a second block — the 56-byte edge.
    const std::string s56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hexOf(s56.data(), s56.size()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // 200 KB fed in awkward chunk sizes must match the one-shot digest.
    std::string big(200 * 1024, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    HE::Crypto::Sha256 h;
    std::size_t pos = 0, step = 1;
    while (pos < big.size())
    {
        const std::size_t n = std::min(step, big.size() - pos);
        h.update(big.data() + pos, n);
        pos += n;
        step = (step * 3 + 1) % 5000 + 1;
    }
    std::uint8_t d[32];
    h.finish(d);
    CHECK(HE::Crypto::Sha256::toHex(d) == hexOf(big.data(), big.size()));

    // hashFile streams the same bytes through a 64 KB buffer.
    TempDir dir("sha");
    const auto p = dir.write("blob.bin", big);
    std::uint8_t fd[32];
    REQUIRE(HE::Crypto::Sha256::hashFile(p, fd));
    CHECK(HE::Crypto::Sha256::toHex(fd) == hexOf(big.data(), big.size()));
    CHECK_FALSE(HE::Crypto::Sha256::hashFile(dir.path / "does_not_exist", fd));
}

// ─── The probe over real files ───────────────────────────────────────────────

TEST_CASE("Integrity: the probe lists exe, libraries and paks by name, sorted, and ignores the rest")
{
    TempDir dir("probe");
    const auto exe = dir.write("HorizonGame", "I am the executable");
    dir.write("libHorizonCore.dylib", "core bytes");
    dir.write("HorizonScene.dll", "scene bytes");
    dir.write("libpython3.12.so.1.0", "python bytes");
    dir.write("config.json", "{}");           // data, not code
    dir.write("AppIcon.png", "not a library");
    const std::uint64_t toc = writePak(dir.path / "Game.hpak", "hello");

    Inputs in;
    in.executable = exe;
    in.libraryDirs = { dir.path, dir.path };   // the flat-export case: same dir twice
    in.paks.push_back({ "Game.hpak", toc });

    const Manifest m = probe(in);
    REQUIRE(m.entries.size() == 5);
    CHECK(m.entries[0].kind == FileKind::Executable);
    CHECK(m.entries[0].name == "HorizonGame");
    CHECK(m.entries[0].hash == hexOf("I am the executable", 19));
    // Libraries sorted by name, each listed once despite the duplicate dir.
    CHECK(m.entries[1].name == "HorizonScene.dll");
    CHECK(m.entries[2].name == "libHorizonCore.dylib");
    CHECK(m.entries[3].name == "libpython3.12.so.1.0");
    CHECK(m.entries[3].hash == hexOf("python bytes", 12));
    CHECK(m.entries[4].kind == FileKind::Pak);
    CHECK(m.entries[4].name == "Game.hpak");
    CHECK(m.entries[4].hash.size() == 16);

    // Same files again: byte-identical manifest. Both sides of a listen server
    // rely on exactly this.
    const Manifest again = probe(in);
    CHECK(compare(m, again).empty());

    // A missing executable is left out, not fatal.
    Inputs gone = in;
    gone.executable = dir.path / "nope";
    CHECK(probe(gone).entries.size() == 4);
}

TEST_CASE("Integrity: compare reports differing, missing and extra files with their names")
{
    const Manifest host = sampleManifest();

    CHECK(compare(host, host).empty());

    Manifest guest = host;
    guest.entries[3].hash = "ffffffffffffffff";                              // pak edited
    guest.entries.erase(guest.entries.begin() + 2);                          // a library gone
    guest.entries.push_back({ FileKind::Library, "libDebugHook.dylib", std::string(64, 'd') });

    const auto diffs = compare(host, guest);
    REQUIRE(diffs.size() == 3);
    // Merge order: (kind, name) — the missing library comes before the extra
    // one alphabetically, the pak last.
    CHECK(diffs[0].what == Mismatch::ExtraOnGuest);
    CHECK(diffs[0].name == "libDebugHook.dylib");
    CHECK(diffs[1].what == Mismatch::MissingOnGuest);
    CHECK(diffs[1].name == "libHorizonScene.dylib");
    CHECK(diffs[2].what == Mismatch::HashDiffers);
    CHECK(diffs[2].name == "Game.hpak");
    CHECK(describe(diffs[2]) == "pak Game.hpak differs");
    CHECK(describe(diffs[1]) == "library libHorizonScene.dylib missing on client");
    CHECK(describe(diffs[0]) == "library libDebugHook.dylib not part of the build");

    // Order on the wire does not matter.
    Manifest shuffled = host;
    std::swap(shuffled.entries[0], shuffled.entries[3]);
    CHECK(compare(host, shuffled).empty());
}

TEST_CASE("Integrity: a manipulated pak changes its tocHash, an identical one does not")
{
    TempDir dir("pak");
    const std::uint64_t a  = writePak(dir.path / "a.hpak", "print('hello')");
    const std::uint64_t a2 = writePak(dir.path / "a2.hpak", "print('hello')");
    const std::uint64_t b  = writePak(dir.path / "b.hpak", "print('hello'); god_mode = true");
    CHECK(a == a2);
    CHECK(a != b);
}

// ─── The exchange at join ────────────────────────────────────────────────────

TEST_CASE("Integrity: identical builds produce no observation at join")
{
    RigOptions opt;
    opt.hostManifest   = sampleManifest();
    opt.clientManifest = sampleManifest();
    auto r = makeRig(opt);
    r->frames(4);

    const ConnectionId conn = LoopbackTransport::kPeer;
    CHECK(r->client->stats().manifestsSent == 1);
    CHECK(r->host->stats().manifestsChecked == 1);
    CHECK(r->host->stats().integrityMismatches == 0);
    CHECK(r->svc.observationCount(conn) == 0);
    CHECK(r->svc.level(conn) == Level::Info);

    // Sent once, not once per frame.
    r->frames(30);
    CHECK(r->client->stats().manifestsSent == 1);
    CHECK(r->host->stats().manifestsChecked == 1);
}

TEST_CASE("Integrity: a manipulated pak is an IntegrityMismatch naming the file, Suspect by default")
{
    TempDir dir("join");
    const std::uint64_t clean  = writePak(dir.path / "clean.hpak", "speed = 1");
    const std::uint64_t edited = writePak(dir.path / "edited.hpak", "speed = 9");
    REQUIRE(clean != edited);

    Inputs hostIn, guestIn;
    hostIn.paks.push_back({ "Game.hpak", clean });
    guestIn.paks.push_back({ "Game.hpak", edited });

    RigOptions opt;
    opt.hostManifest   = probe(hostIn);
    opt.clientManifest = probe(guestIn);
    auto r = makeRig(opt);
    r->frames(4);

    const ConnectionId conn = LoopbackTransport::kPeer;
    CHECK(r->host->stats().manifestsChecked == 1);
    CHECK(r->host->stats().integrityMismatches == 1);
    CHECK(r->svc.observationCount(conn, Kind::IntegrityMismatch) == 1);
    // Default weight = suspect threshold: one edited file is Suspect at once.
    CHECK(r->svc.level(conn) == Level::Suspect);

    int id = 0;
    REQUIRE(r->svc.takeReport(id));
    const auto* rep = r->svc.findReport(id);
    REQUIRE(rep != nullptr);
    CHECK(rep->trigger == Kind::IntegrityMismatch);
    CHECK(rep->detail.find("pak Game.hpak differs") != std::string::npos);
}

TEST_CASE("Integrity: several differing files climb to Confirmed, and integrityHard makes one Hard")
{
    SUBCASE("weighed")
    {
        RigOptions opt;
        opt.hostManifest = sampleManifest();
        Manifest guest   = sampleManifest("ffffffffffffffff");
        guest.entries[0].hash = std::string(64, 'x');   // exe differs too
        guest.entries.push_back({ FileKind::Library, "libDebugHook.dylib", std::string(64, 'd') });
        guest.entries.push_back({ FileKind::Library, "libOverlay.dylib", std::string(64, 'e') });
        opt.clientManifest = guest;
        auto r = makeRig(opt);
        r->frames(4);
        const ConnectionId conn = LoopbackTransport::kPeer;
        CHECK(r->host->stats().integrityMismatches == 4);
        CHECK(r->svc.observationCount(conn, Kind::IntegrityMismatch) == 4);
        CHECK(r->svc.level(conn) == Level::Confirmed);
    }
    SUBCASE("hard")
    {
        RigOptions opt;
        opt.config.integrityHard = true;
        opt.hostManifest   = sampleManifest();
        opt.clientManifest = sampleManifest("ffffffffffffffff");
        auto r = makeRig(opt);
        r->frames(4);
        const ConnectionId conn = LoopbackTransport::kPeer;
        CHECK(r->svc.observationCount(conn, Kind::IntegrityMismatch) == 1);
        CHECK(r->svc.level(conn) == Level::Hard);
        CHECK(r->svc.score(conn) == doctest::Approx(0.0f));
    }
}

TEST_CASE("Integrity: a dev build has no manifest — the check is off, logged once, and nothing crashes")
{
    LogSpy spy;

    SUBCASE("host without a manifest ignores what clients send")
    {
        breakRepeatChain();
        RigOptions opt;
        opt.hostManifest   = std::nullopt;   // explicit: this build has none
        opt.clientManifest = sampleManifest();
        auto r = makeRig(opt);
        r->frames(4);
        const ConnectionId conn = LoopbackTransport::kPeer;
        CHECK(r->client->stats().manifestsSent == 1);
        CHECK(r->host->stats().manifestsChecked == 0);
        CHECK(r->svc.observationCount(conn) == 0);
        CHECK(spy.count("integrity check off") == 1);
    }
    SUBCASE("client without a manifest joining a packaged host is one mismatch")
    {
        RigOptions opt;
        opt.hostManifest   = sampleManifest();
        opt.clientManifest = std::nullopt;
        auto r = makeRig(opt);
        r->frames(4);
        const ConnectionId conn = LoopbackTransport::kPeer;
        CHECK(r->host->stats().manifestsChecked == 1);
        CHECK(r->svc.observationCount(conn, Kind::IntegrityMismatch) == 1);
        CHECK(r->svc.level(conn) == Level::Suspect);
        int id = 0;
        REQUIRE(r->svc.takeReport(id));
        CHECK(r->svc.findReport(id)->detail.find("no manifest from client") != std::string::npos);
    }
    SUBCASE("neither side has one: both are dev builds, silence")
    {
        breakRepeatChain();
        RigOptions opt;
        opt.hostManifest   = std::nullopt;
        opt.clientManifest = std::nullopt;
        auto r = makeRig(opt);
        r->frames(4);
        CHECK(r->svc.observationCount(LoopbackTransport::kPeer) == 0);
        CHECK(spy.count("integrity check off") == 1);
    }
}

TEST_CASE("Integrity: without a call, both sides adopt the process-wide probe")
{
    auto& probeInst = IntegrityProbe::instance();
    probeInst.reset();
    REQUIRE(probeInst.state() == IntegrityProbe::State::Idle);

    SUBCASE("Idle probe = dev build")
    {
        LogSpy spy;
        breakRepeatChain();
        RigOptions opt;   // no manifests set
        auto r = makeRig(opt);
        r->frames(4);
        CHECK_FALSE(r->host->hasLocalManifest());
        CHECK(r->host->stats().manifestsChecked == 0);
        CHECK(spy.count("integrity check off") == 1);
    }
    SUBCASE("a Ready probe is used by both, and matches itself")
    {
        TempDir dir("global");
        Inputs in;
        in.executable = dir.write("HorizonGame", "exe");
        in.libraryDirs = { dir.path };
        dir.write("libHorizonCore.dylib", "lib");
        probeInst.runNow(in);
        REQUIRE(probeInst.ready());
        CHECK(probeInst.manifest().entries.size() == 2);

        RigOptions opt;
        auto r = makeRig(opt);
        r->frames(4);
        CHECK(r->host->hasLocalManifest());
        CHECK(r->host->stats().manifestsChecked == 1);
        CHECK(r->host->stats().integrityMismatches == 0);
    }
    SUBCASE("start() hashes on the pool and the join waits for it")
    {
        TempDir dir("async");
        Inputs in;
        in.executable = dir.write("HorizonGame", "exe bytes");
        probeInst.start(in);
        // Nothing is sent or compared while the worker hashes; once it is
        // Ready both sides pick the manifest up on their next update.
        RigOptions opt;
        auto r = makeRig(opt);
        r->frames(2);
        for (int i = 0; i < 5000 && !probeInst.ready(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(probeInst.ready());
        r->frames(4);
        CHECK(r->host->hasLocalManifest());
        CHECK(r->client->stats().manifestsSent == 1);
        CHECK(r->host->stats().manifestsChecked == 1);
        CHECK(r->host->stats().integrityMismatches == 0);
    }
    probeInst.reset();
}

TEST_CASE("Integrity: a malformed manifest is Hard, and a second one is ignored")
{
    RigOptions opt;
    opt.hostManifest   = sampleManifest();
    opt.clientManifest = sampleManifest();
    auto r = makeRig(opt);
    const ConnectionId conn = LoopbackTransport::kPeer;

    SUBCASE("truncated")
    {
        // Hand-written frame before the client's real one: present=true, count=2,
        // then nothing.
        BitWriter w;
        w.writeBool(true);
        w.writeUInt16(2);
        r->clientNet->send(conn, kMsgIntegrity, w, SendMode::ReliableOrdered);
        r->frames(4);
        CHECK(r->svc.observationCount(conn, Kind::Malformed) == 1);
        CHECK(r->svc.level(conn) == Level::Hard);
        // The real manifest that followed was the second one: ignored.
        CHECK(r->host->stats().manifestsChecked == 0);
    }
    SUBCASE("over the entry cap")
    {
        BitWriter w;
        w.writeBool(true);
        w.writeUInt16(1000);
        r->clientNet->send(conn, kMsgIntegrity, w, SendMode::ReliableOrdered);
        r->frames(4);
        CHECK(r->svc.observationCount(conn, Kind::Malformed) == 1);
    }
    SUBCASE("a repeated, 'repaired' manifest does not overwrite the first")
    {
        r->frames(4);
        CHECK(r->host->stats().manifestsChecked == 1);
        r->client->setLocalManifest(sampleManifest("ffffffffffffffff"));
        BitWriter w;
        w.writeBool(true);
        w.writeUInt16(0);
        r->clientNet->send(conn, kMsgIntegrity, w, SendMode::ReliableOrdered);
        r->frames(4);
        CHECK(r->host->stats().manifestsChecked == 1);
        CHECK(r->svc.observationCount(conn) == 0);
    }
}

TEST_CASE("Integrity: no service or integrityCheck=false compares nothing")
{
    SUBCASE("no service")
    {
        RigOptions opt;
        opt.attachService  = false;
        opt.hostManifest   = sampleManifest();
        opt.clientManifest = sampleManifest("ffffffffffffffff");
        auto r = makeRig(opt);
        r->frames(4);
        CHECK(r->client->stats().manifestsSent == 1);
        CHECK(r->host->stats().manifestsChecked == 0);
        CHECK(r->host->stats().integrityMismatches == 0);
    }
    SUBCASE("setting off")
    {
        RigOptions opt;
        opt.repl.integrityCheck = false;
        opt.hostManifest   = sampleManifest();
        opt.clientManifest = sampleManifest("ffffffffffffffff");
        auto r = makeRig(opt);
        r->frames(4);
        CHECK(r->host->stats().manifestsChecked == 0);
        CHECK(r->svc.observationCount(LoopbackTransport::kPeer) == 0);
    }
}
