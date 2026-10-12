#include "doctest.h"
#include "TestFsUtil.h"

#include <SelfUpdate/SelfUpdateCheck.h>
#include <SelfUpdate/SelfUpdateEndpoint.h>
#include <SelfUpdate/SelfUpdateManifest.h>
#include <SelfUpdate/SelfUpdateProcess.h>
#include <SelfUpdate/SelfUpdateTransport.h>
#include <SelfUpdate/SelfUpdateVersion.h>
#include <SelfUpdate/Takeover.h>

#include <ContentSync/SftpCredentials.h>
#include <Crypto/Sha256.h>

#include <HorizonVersion.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace HE::Su;

namespace {

// A directory under the test's own temp dir (ctest points TMPDIR at a folder
// per test file), removed again when the scope ends.
struct TempDir
{
	fs::path path;
	TempDir()
	{
		static int counter = 0;
		path = fs::temp_directory_path() /
		       ("he_selfupdate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
		        std::to_string(++counter));
		fs::create_directories(path);
	}
	~TempDir() { he_test::removeAllQuiet(path); }
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
};

void writeFile(const fs::path& p, const std::string& bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readFile(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string sha256Hex(const std::string& bytes)
{
	std::uint8_t digest[HE::Crypto::Sha256::kDigestSize];
	HE::Crypto::Sha256::digest(bytes.data(), bytes.size(), digest);
	return HE::Crypto::Sha256::toHex(digest);
}

// A fake "update server" on disk, laid out as the real account is: a channel
// directory at the root holding the manifest, with the artifacts it names
// relative to that manifest.
//   <root>/packages/manifest.json
//   <root>/packages/releases/<version>/<platform file>
struct FakeServer
{
	TempDir dir;

	// Publishes `version` with one artifact for `platform`.
	void publish(const std::string& version, const std::string& platform, const std::string& bytes)
	{
		const std::string rel = "releases/" + version + "/HorizonEditor-" + platform + ".pkg";
		writeFile(dir.path / "packages" / rel, bytes);

		UpdateManifest m;
		m.version = version;
		m.codename = "Test";
		m.artifacts.push_back({ platform, rel, sha256Hex(bytes), bytes.size() });
		writeFile(dir.path / "packages" / "manifest.json", serializeUpdateManifest(m));
	}

	fs::path artifactFile(const CheckResult& check) const { return dir.path / "packages" / check.artifact.path; }
};

// Wraps a real transport so a test can count fetches and make the first few fail.
class CountingTransport final : public IUpdateTransport
{
public:
	explicit CountingTransport(std::unique_ptr<IUpdateTransport> inner) : m_inner(std::move(inner)) {}

	int fetches = 0;
	int failTransientTimes = 0;
	std::vector<std::string> requested;

	SuResult fetchFile(const std::string& rel, const fs::path& local, const SuProgressFn& progress) override
	{
		++fetches;
		requested.push_back(rel);
		if (failTransientTimes > 0)
		{
			--failTransientTimes;
			return { false, "simulated connection drop", true };
		}
		return m_inner->fetchFile(rel, local, progress);
	}
	std::string describe() const override { return "counting " + m_inner->describe(); }

private:
	std::unique_ptr<IUpdateTransport> m_inner;
};

CheckOptions fastOptions(const fs::path& scratch)
{
	CheckOptions o;
	o.scratchDir = scratch;
	o.backoffMs  = 0;
	return o;
}

EnvLookup mapEnv(const std::map<std::string, std::string>& vars)
{
	return [vars](const char* name) -> const char*
	{
		const auto it = vars.find(name);
		return it == vars.end() ? nullptr : it->second.c_str();
	};
}

const LocalBuild kLocal090 { "0.9.0", "linux-x64" };

} // namespace

// ─── versions ────────────────────────────────────────────────────────────────

TEST_CASE("selfupdate: versions compare as numbers, not as text")
{
	auto cmp = [](const char* a, const char* b)
	{
		Version va, vb;
		REQUIRE(parseVersion(a, va));
		REQUIRE(parseVersion(b, vb));
		return compareVersions(va, vb);
	};

	// The reason this exists: as text, "0.10.0" sorts below "0.9.0".
	CHECK(cmp("0.10.0", "0.9.0") > 0);
	CHECK(cmp("0.9.0", "0.10.0") < 0);
	CHECK(cmp("1.0.0", "0.99.99") > 0);
	CHECK(cmp("0.7.0", "0.7.0") == 0);
	CHECK(cmp("v0.7", "0.7.0") == 0);                 // 'v' and a missing patch field
	CHECK(cmp("0.7.0+build.5", "0.7.0") == 0);        // build metadata is ignored

	// A release is newer than its own pre-releases; pre-releases order by identifier.
	CHECK(cmp("0.8.0", "0.8.0-rc.1") > 0);
	CHECK(cmp("0.8.0-rc.2", "0.8.0-rc.1") > 0);
	CHECK(cmp("0.8.0-rc.10", "0.8.0-rc.9") > 0);      // numeric identifier, not text
	CHECK(cmp("0.8.0-alpha", "0.8.0-1") > 0);         // numeric sorts below alphanumeric
	CHECK(cmp("0.8.0-rc", "0.8.0-rc.1") < 0);         // the shorter list sorts lower
	CHECK(cmp("0.8.0-rc.1", "0.7.9") > 0);

	Version v;
	REQUIRE(parseVersion("v0.8.0-rc.1+b7", v));
	CHECK(v.toString() == "0.8.0-rc.1");
}

TEST_CASE("selfupdate: text that is not a version is rejected")
{
	Version v;
	for (const char* bad : { "", "abc", "1..2", "1.2.3.4", "1.2.3-", "-1", "1.x.3", "1.2.3-a..b", "9999999999.0.0", " 1.0.0" })
		CHECK_MESSAGE(!parseVersion(bad, v), "accepted: '", bad, "'");
}

// ─── manifest ────────────────────────────────────────────────────────────────

namespace {
std::string manifestJson(const std::string& artifactFields, const std::string& top = "\"schema\":1,\"version\":\"0.8.0\"")
{
	return "{" + top + ",\"artifacts\":[{" + artifactFields + "}]}";
}
const std::string kGoodSha = std::string(64, 'a');
std::string goodArtifact(const std::string& path = "releases/0.8.0/Editor.zip")
{
	return "\"platform\":\"windows-x64\",\"path\":\"" + path + "\",\"sha256\":\"" + kGoodSha + "\",\"size\":10";
}
} // namespace

TEST_CASE("selfupdate: manifest round trips and finds the platform's build")
{
	UpdateManifest in;
	in.version  = "0.8.0";
	in.codename = "Cirrus";
	in.released = "2026-11-01";
	in.notes    = "Selbst-Update";
	in.artifacts.push_back({ "windows-x64", "releases/0.8.0/HorizonEditor-windows-x64.zip", kGoodSha, 100 });
	in.artifacts.push_back({ "macos-arm64", "releases/0.8.0/HorizonEditor-0.8.0.dmg", std::string(64, 'b'), 200 });

	UpdateManifest out;
	std::string error;
	REQUIRE_MESSAGE(parseUpdateManifest(serializeUpdateManifest(in), out, error), error);
	CHECK(out.version == "0.8.0");
	CHECK(out.codename == "Cirrus");
	CHECK(out.notes == "Selbst-Update");
	REQUIRE(out.artifacts.size() == 2);

	const UpdateArtifact* mac = out.findArtifact("macos-arm64");
	REQUIRE(mac);
	CHECK(mac->size == 200);
	CHECK(mac->fileName() == "HorizonEditor-0.8.0.dmg");
	CHECK(out.findArtifact("linux-x64") == nullptr);
}

TEST_CASE("selfupdate: manifest ignores fields it does not know and normalises the hash")
{
	UpdateManifest m;
	std::string error;
	const std::string upper(64, 'A');
	REQUIRE_MESSAGE(parseUpdateManifest(
		"{\"schema\":1,\"version\":\"0.8.0\",\"future\":{\"x\":1},\"artifacts\":[{\"platform\":\"linux-x64\","
		"\"path\":\"a/b.tar.gz\",\"sha256\":\"" + upper + "\",\"size\":5,\"signature\":\"later\"}]}", m, error), error);
	CHECK(m.artifacts[0].sha256 == std::string(64, 'a'));
}

TEST_CASE("selfupdate: manifest rejects what could do harm further down the line")
{
	UpdateManifest m;
	std::string error;
	auto rejects = [&](const std::string& json, const char* why)
	{
		CHECK_MESSAGE(!parseUpdateManifest(json, m, error), why);
		CHECK_MESSAGE(!error.empty(), why);
	};

	rejects("not json", "garbage");
	rejects("[]", "not an object");
	rejects("{\"version\":\"0.8.0\"}", "no artifacts");
	rejects(manifestJson(goodArtifact(), "\"schema\":1"), "no version");
	rejects(manifestJson(goodArtifact(), "\"schema\":1,\"version\":\"soon\""), "version is not a version");
	rejects(manifestJson(goodArtifact(), "\"schema\":1,\"version\":\"1.0.0-a/../b\""), "version names a path");
	rejects(manifestJson(goodArtifact(), "\"schema\":2,\"version\":\"0.8.0\""), "a schema this editor does not know");
	rejects(manifestJson(goodArtifact("../../etc/passwd")), "parent traversal");
	rejects(manifestJson(goodArtifact("releases/../../x")), "traversal in the middle");
	rejects(manifestJson(goodArtifact("/etc/passwd")), "absolute path");
	rejects(manifestJson(goodArtifact("C:\\\\Windows\\\\x.exe")), "windows path");
	rejects(manifestJson(goodArtifact("a\\\\b.zip")), "backslash");
	rejects(manifestJson(goodArtifact("dir/")), "trailing slash");
	rejects(manifestJson(goodArtifact("a//b.zip")), "empty segment");
	rejects(manifestJson(goodArtifact("a/b c.zip")), "space");
	rejects(manifestJson(goodArtifact("a/b.")), "name ends in a dot");
	rejects(manifestJson("\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"size\":10"), "no sha256");
	rejects(manifestJson("\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"sha256\":\"abc\",\"size\":10"), "short sha256");
	rejects(manifestJson("\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"sha256\":\"" + std::string(64, 'z') + "\",\"size\":10"),
	        "non-hex sha256");
	rejects(manifestJson("\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"sha256\":\"" + kGoodSha + "\""), "no size");
	rejects(manifestJson("\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"sha256\":\"" + kGoodSha + "\",\"size\":0"), "zero size");
	rejects(manifestJson("\"platform\":\"Linux X64\",\"path\":\"a.zip\",\"sha256\":\"" + kGoodSha + "\",\"size\":1"), "odd platform key");

	const std::string a = "{\"platform\":\"linux-x64\",\"path\":\"a.zip\",\"sha256\":\"" + kGoodSha + "\",\"size\":1}";
	rejects("{\"schema\":1,\"version\":\"0.8.0\",\"artifacts\":[" + a + "," + a + "]}", "a platform twice");
}

TEST_CASE("selfupdate: the safe-path rule")
{
	CHECK(isSafeRelativePath("packages/manifest.json"));
	CHECK(isSafeRelativePath("releases/0.8.0-rc.1/HorizonEditor-0.8.0+b5.dmg"));
	CHECK_FALSE(isSafeRelativePath(""));
	CHECK_FALSE(isSafeRelativePath("."));
	CHECK_FALSE(isSafeRelativePath(".."));
	CHECK_FALSE(isSafeRelativePath("a/./b"));
	CHECK_FALSE(isSafeRelativePath("a/b:stream"));
	CHECK_FALSE(isSafeRelativePath(std::string("a/b\0c", 5)));
	CHECK_FALSE(isSafeRelativePath(std::string(600, 'a')));
}

// ─── the second account ──────────────────────────────────────────────────────

TEST_CASE("selfupdate: the account comes from HE_SELFUPDATE_* and nothing else")
{
	// The cases that expect "off" hand in an empty compiled-in account: a build
	// configured with one (CI with the SELFUPDATE_* secrets, a developer's
	// cmake/SelfUpdateCredentials.cmake) would otherwise be Ready here.
	const HE::Cs::SftpEndpoint noBuiltIn;

	SUBCASE("nothing set means the feature is simply off")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({}), noBuiltIn);
		CHECK(c.state == ConfigState::NotSet);
		CHECK(c.source == ConfigSource::None);
		CHECK_FALSE(c.ready());
		CHECK(c.problem.empty());
		CHECK_FALSE(c.endpoint.configured());
	}
	SUBCASE("a channel alone does not switch it on")
	{
		CHECK(loadSelfUpdateConfig(mapEnv({ { kEnvChannel, "beta" } }), noBuiltIn).state == ConfigState::NotSet);
	}
	SUBCASE("the EngineContent variables are not a source for this account")
	{
		// The EngineContent account is compiled in, but its configure-time variable
		// names are the nearest thing to confuse: setting them must not light this up.
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { "HE_ENGINE_CONTENT_HOST", "files.example.org" },
		                                                          { "HE_ENGINE_CONTENT_USER", "content" },
		                                                          { "HE_ENGINE_CONTENT_PASSWORD", "pw" } }),
		                                                noBuiltIn);
		CHECK(c.state == ConfigState::NotSet);
		CHECK_FALSE(c.endpoint.configured());
	}
	SUBCASE("a half-filled account says what is missing")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { kEnvHost, "updates.example.org" } }));
		CHECK(c.state == ConfigState::Invalid);
		CHECK(c.problem.find(kEnvUser) != std::string::npos);
		CHECK(c.problem.find(kEnvPassword) != std::string::npos);
		CHECK(c.problem.find(kEnvHost) == std::string::npos);
		CHECK_FALSE(c.ready());
	}
	SUBCASE("a complete one is ready, with the defaults filled in")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv(
			{ { kEnvHost, "updates.example.org" }, { kEnvUser, "editor-ro" }, { kEnvPassword, "s3cret" } }));
		REQUIRE(c.ready());
		CHECK(c.endpoint.host == "updates.example.org");
		CHECK(c.endpoint.port == 22);
		CHECK(c.endpoint.username == "editor-ro");
		CHECK(c.endpoint.password == "s3cret");
		CHECK(c.endpoint.remoteBasePath.empty());
		CHECK(c.channel == "packages");
		CHECK(c.manifestRemotePath() == "packages/manifest.json");
	}
	SUBCASE("port, base path and channel are honoured")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { kEnvHost, "h" }, { kEnvUser, "u" }, { kEnvPassword, "p" },
		                                                          { kEnvPort, "2222" }, { kEnvBasePath, "/srv/updates" },
		                                                          { kEnvChannel, "beta-1.2" } }));
		REQUIRE(c.ready());
		CHECK(c.endpoint.port == 2222);
		CHECK(c.endpoint.remoteBasePath == "/srv/updates");
		CHECK(c.manifestRemotePath() == "beta-1.2/manifest.json");
	}
	SUBCASE("a bad port or channel is refused rather than guessed")
	{
		const std::map<std::string, std::string> base { { kEnvHost, "h" }, { kEnvUser, "u" }, { kEnvPassword, "p" } };
		for (const char* port : { "0", "65536", "22x", "-1", " 22", "123456" })
		{
			auto vars = base;
			vars[kEnvPort] = port;
			CHECK_MESSAGE(loadSelfUpdateConfig(mapEnv(vars)).state == ConfigState::Invalid, "port '", port, "'");
		}
		for (const char* channel : { "a/b", "..", "has space", "trailing." })
		{
			auto vars = base;
			vars[kEnvChannel] = channel;
			CHECK_MESSAGE(loadSelfUpdateConfig(mapEnv(vars)).state == ConfigState::Invalid, "channel '", channel, "'");
		}
	}
}

TEST_CASE("selfupdate: the compiled-in account, and the environment replacing it as a whole")
{
	HE::Cs::SftpEndpoint baked;
	baked.host = "updates.example.org"; baked.port = 2222; baked.username = "editor-ro";
	baked.password = "baked-pw"; baked.remoteBasePath = "/srv/updates";

	SUBCASE("with nothing in the environment the compiled-in account is used")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({}), baked);
		REQUIRE_MESSAGE(c.ready(), c.problem);
		CHECK(c.source == ConfigSource::BuiltIn);
		CHECK(c.endpoint.host == "updates.example.org");
		CHECK(c.endpoint.port == 2222);
		CHECK(c.endpoint.username == "editor-ro");
		CHECK(c.endpoint.password == "baked-pw");
		CHECK(c.endpoint.remoteBasePath == "/srv/updates");
		CHECK(c.manifestRemotePath() == "packages/manifest.json");
	}
	SUBCASE("the channel still comes from the environment, and is still validated")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { kEnvChannel, kPreviewChannel } }), baked);
		REQUIRE(c.ready());
		CHECK(c.source == ConfigSource::BuiltIn);
		CHECK(c.manifestRemotePath() == "preview-packages/manifest.json");
		CHECK(loadSelfUpdateConfig(mapEnv({ { kEnvChannel, "a/b" } }), baked).state == ConfigState::Invalid);
	}
	SUBCASE("a complete account in the environment wins")
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv(
			{ { kEnvHost, "127.0.0.1" }, { kEnvUser, "test" }, { kEnvPassword, "env-pw" } }), baked);
		REQUIRE(c.ready());
		CHECK(c.source == ConfigSource::Environment);
		CHECK(c.endpoint.host == "127.0.0.1");
		CHECK(c.endpoint.port == 22);                  // not the compiled-in 2222
		CHECK(c.endpoint.password == "env-pw");
		CHECK(c.endpoint.remoteBasePath.empty());      // not the compiled-in base path
	}
	SUBCASE("a partial environment is never topped up from the compiled-in account")
	{
		// Otherwise naming a host in the environment would send the baked password to it.
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { kEnvHost, "elsewhere.example.org" } }), baked);
		CHECK(c.state == ConfigState::Invalid);
		CHECK(c.source == ConfigSource::Environment);
		CHECK(c.problem.find(kEnvPassword) != std::string::npos);
		CHECK(c.problem.find("replaces the compiled-in one") != std::string::npos);
		CHECK_FALSE(c.endpoint.configured());
		CHECK(c.problem.find("baked-pw") == std::string::npos);

		// HE_SELFUPDATE_PORT alone counts as naming an account, too.
		CHECK(loadSelfUpdateConfig(mapEnv({ { kEnvPort, "22" } }), baked).state == ConfigState::Invalid);
	}
	SUBCASE("an incomplete compiled-in account says so")
	{
		HE::Cs::SftpEndpoint half = baked;
		half.password.clear();
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({}), half);
		CHECK(c.state == ConfigState::Invalid);
		CHECK(c.source == ConfigSource::BuiltIn);
		CHECK(c.problem.find("compiled into this build") != std::string::npos);
		CHECK(c.problem.find(kEnvPassword) != std::string::npos);
	}
	SUBCASE("a compiled-in account that is the EngineContent login is refused like any other")
	{
		const HE::Cs::SftpEndpoint& content = HE::Cs::engineContentEndpoint();
		if (content.configured())
		{
			HE::Cs::SftpEndpoint same = content;
			same.password = "anything";
			const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({}), same);
			CHECK(c.state == ConfigState::Invalid);
			CHECK(c.problem.find("EngineContent") != std::string::npos);
			CHECK_FALSE(c.endpoint.configured());
		}
		else
		{
			MESSAGE("no EngineContent account compiled into this build — nothing to collide with");
		}
	}
	SUBCASE("this build's own compiled-in account, whatever it is, is what the one-argument form falls back to")
	{
		const SelfUpdateConfig viaDefault = loadSelfUpdateConfig(mapEnv({}));
		const SelfUpdateConfig explicitly = loadSelfUpdateConfig(mapEnv({}), builtInSelfUpdateEndpoint());
		CHECK(viaDefault.state == explicitly.state);
		CHECK(viaDefault.source == explicitly.source);
		CHECK(viaDefault.endpoint.host == explicitly.endpoint.host);
		MESSAGE("compiled-in update account: ", builtInSelfUpdateEndpoint().host.empty() ? "none" : "present");
	}
}

TEST_CASE("selfupdate: the same login as the EngineContent account is not a second account")
{
	HE::Cs::SftpEndpoint a;
	a.host = "Files.Example.org"; a.port = 22; a.username = "content"; a.password = "x";
	HE::Cs::SftpEndpoint b = a;
	b.host = "files.example.org"; b.password = "different";

	CHECK(sameAccount(a, b));                 // host compares without case; the password is not part of the identity
	b.username = "updates";
	CHECK_FALSE(sameAccount(a, b));
	b.username = "content"; b.port = 2222;
	CHECK_FALSE(sameAccount(a, b));
	CHECK_FALSE(sameAccount(HE::Cs::SftpEndpoint{}, HE::Cs::SftpEndpoint{}));   // two empty ones are not "the same"

	// And the loader acts on it. Only meaningful in a build that has an
	// EngineContent account compiled in (CI with the secrets; a developer's
	// credentials file) — a plain build has nothing to collide with.
	const HE::Cs::SftpEndpoint& content = HE::Cs::engineContentEndpoint();
	if (content.configured())
	{
		const SelfUpdateConfig c = loadSelfUpdateConfig(mapEnv({ { kEnvHost, content.host.c_str() },
		                                                          { kEnvUser, content.username.c_str() },
		                                                          { kEnvPort, std::to_string(content.port).c_str() },
		                                                          { kEnvPassword, "anything" } }));
		CHECK(c.state == ConfigState::Invalid);
		CHECK(c.problem.find("EngineContent") != std::string::npos);
		CHECK_FALSE(c.endpoint.configured());   // and nothing of it is handed out
	}
	else
	{
		MESSAGE("no EngineContent account compiled into this build — the collision check on the loader ran only for sameAccount()");
	}
}

TEST_CASE("selfupdate: the password is scrubbed out of text")
{
	CHECK(scrubSecret("auth failed for pw=hunter2 (hunter2)", "hunter2") == "auth failed for pw=<redacted> (<redacted>)");
	CHECK(scrubSecret("nothing here", "hunter2") == "nothing here");
	CHECK(scrubSecret("leave alone", "") == "leave alone");
}

// ─── platform and running build ──────────────────────────────────────────────

TEST_CASE("selfupdate: the running build and platform key")
{
	const std::string key = currentPlatformKey();
	CHECK(std::regex_match(key, std::regex("(windows|macos|linux|unknown)-(x64|arm64|unknown)")));
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
	CHECK(key == "windows-x64");
#elif defined(__APPLE__) && defined(__aarch64__)
	CHECK(key == "macos-arm64");
#elif defined(__linux__) && defined(__x86_64__)
	CHECK(key == "linux-x64");
#endif
	const LocalBuild b = runningBuild();
	CHECK(b.version == HE_VERSION_STRING);
	CHECK(b.platform == key);
	Version v;
	CHECK(parseVersion(b.version, v));
}

// ─── the check ───────────────────────────────────────────────────────────────

TEST_CASE("selfupdate: the check against a server directory")
{
	FakeServer server;
	TempDir scratch;
	const CheckOptions options = fastOptions(scratch.path);
	CHECK_FALSE(server.dir.path.empty());

	auto transport = [&] { return makeLocalDirTransport(server.dir.path); };

	SUBCASE("a newer remote is offered, compared as numbers")
	{
		server.publish("0.10.0", "linux-x64", "new build");
		const auto t = transport();
		const CheckResult r = checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		REQUIRE_MESSAGE(r.status == CheckStatus::UpdateAvailable, r.error);
		CHECK(r.updateAvailable());
		CHECK(r.remoteVersion == "0.10.0");
		CHECK(r.localVersion == "0.9.0");
		CHECK(r.artifact.platform == "linux-x64");
		CHECK(r.artifact.path == "releases/0.10.0/HorizonEditor-linux-x64.pkg");
		// The artifact sits beside the manifest, so it is asked for under the channel directory.
		CHECK(r.artifactRemotePath == "packages/releases/0.10.0/HorizonEditor-linux-x64.pkg");
		CHECK(r.artifact.size == 9);
		CHECK(r.manifest.codename == "Test");
	}
	SUBCASE("the same version is up to date")
	{
		server.publish("0.9.0", "linux-x64", "same");
		const auto t = transport();
		CHECK(checkForUpdate(*t, "packages/manifest.json", kLocal090, options).status == CheckStatus::UpToDate);
	}
	SUBCASE("an older remote is reported but never offered")
	{
		server.publish("0.8.9", "linux-x64", "old");
		const auto t = transport();
		const CheckResult r = checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		CHECK(r.status == CheckStatus::RemoteOlder);
		CHECK_FALSE(r.updateAvailable());
	}
	SUBCASE("a pre-release is older than its release")
	{
		server.publish("0.9.0-rc.1", "linux-x64", "rc");
		const auto t = transport();
		CHECK(checkForUpdate(*t, "packages/manifest.json", kLocal090, options).status == CheckStatus::RemoteOlder);
	}
	SUBCASE("newer, but not built for this platform")
	{
		server.publish("0.10.0", "windows-x64", "win only");
		const auto t = transport();
		const CheckResult r = checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		CHECK(r.status == CheckStatus::NoBuildForPlatform);
		CHECK(r.error.find("linux-x64") != std::string::npos);
	}
	SUBCASE("a missing manifest is a failure that names the file")
	{
		const auto t = transport();
		const CheckResult r = checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		CHECK(r.status == CheckStatus::Failed);
		CHECK(r.error.find("packages/manifest.json") != std::string::npos);
	}
	SUBCASE("an unreadable manifest is a failure, not a crash")
	{
		writeFile(server.dir.path / "packages" / "manifest.json", "{ this is not json");
		const auto t = transport();
		const CheckResult r = checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		CHECK(r.status == CheckStatus::Failed);
		CHECK(r.error.find("not usable") != std::string::npos);
	}
	SUBCASE("a running version that is not a version fails loudly")
	{
		server.publish("0.10.0", "linux-x64", "x");
		const auto t = transport();
		CHECK(checkForUpdate(*t, "packages/manifest.json", { "dev", "linux-x64" }, options).status == CheckStatus::Failed);
	}
	SUBCASE("the scratch manifest does not stay behind")
	{
		server.publish("0.10.0", "linux-x64", "x");
		const auto t = transport();
		checkForUpdate(*t, "packages/manifest.json", kLocal090, options);
		CHECK(fs::is_empty(scratch.path));
	}
	SUBCASE("a dropped connection is retried, a missing file is not")
	{
		server.publish("0.10.0", "linux-x64", "x");
		CountingTransport flaky(transport());
		flaky.failTransientTimes = 2;
		CHECK(checkForUpdate(flaky, "packages/manifest.json", kLocal090, options).status == CheckStatus::UpdateAvailable);
		CHECK(flaky.fetches == 3);

		CountingTransport missing(transport());
		CHECK(checkForUpdate(missing, "packages/nope.json", kLocal090, options).status == CheckStatus::Failed);
		CHECK(missing.fetches == 1);

		CountingTransport dead(transport());
		dead.failTransientTimes = 99;
		CHECK(checkForUpdate(dead, "packages/manifest.json", kLocal090, options).status == CheckStatus::Failed);
		CHECK(dead.fetches == 3);   // gives up after the configured attempts
	}
}

TEST_CASE("selfupdate: no account means no network and says so")
{
	SelfUpdateConfig off;   // NotSet
	CheckResult r = checkForUpdate(off, kLocal090);
	CHECK(r.status == CheckStatus::NotConfigured);
	CHECK(r.error.find("HE_SELFUPDATE") != std::string::npos);

	SelfUpdateConfig broken = loadSelfUpdateConfig(mapEnv({ { kEnvHost, "h" } }));
	r = checkForUpdate(broken, kLocal090);
	CHECK(r.status == CheckStatus::NotConfigured);
	CHECK(r.error.find(kEnvPassword) != std::string::npos);
}

TEST_CASE("selfupdate: the SFTP transport refuses an unconfigured account without falling back")
{
	// An unconfigured endpoint must not reach SftpClient, whose own message would
	// name the EngineContent endpoint, and must never borrow that account.
	const auto t = makeSftpTransport(HE::Cs::SftpEndpoint{});
	TempDir dir;
	const SuResult r = t->fetchFile("packages/manifest.json", dir.path / "m.json", {});
	CHECK_FALSE(r.ok);
	CHECK_FALSE(r.transient);
	CHECK(r.error.find("self-update") != std::string::npos);
	CHECK(r.error.find("EngineContent") == std::string::npos);
	CHECK_FALSE(fs::exists(dir.path / "m.json"));
}

TEST_CASE("selfupdate: the SFTP transport refuses an unsafe path and keeps the password out of its errors")
{
	HE::Cs::SftpEndpoint ep;
	ep.host = "127.0.0.1";
	ep.port = 1;   // nothing listens here: the connection is refused at once
	ep.username = "editor-ro";
	ep.password = "pw-that-must-not-leak";
	const auto t = makeSftpTransport(ep);
	TempDir dir;

	const SuResult unsafe = t->fetchFile("../../etc/passwd", dir.path / "x", {});
	CHECK_FALSE(unsafe.ok);
	CHECK(unsafe.error.find("unsafe") != std::string::npos);

	const SuResult refused = t->fetchFile("packages/manifest.json", dir.path / "m.json", {});
	CHECK_FALSE(refused.ok);
	CHECK(refused.error.find("pw-that-must-not-leak") == std::string::npos);
	CHECK(t->describe().find("pw-that-must-not-leak") == std::string::npos);
	CHECK(t->describe().find("editor-ro@127.0.0.1:1") != std::string::npos);
}

// ─── the download ────────────────────────────────────────────────────────────

TEST_CASE("selfupdate: download, verify and stage")
{
	FakeServer server;
	TempDir scratch, staging;
	const CheckOptions options = fastOptions(scratch.path);
	const std::string bytes(300 * 1024 + 17, 'Q');   // spans several read chunks
	server.publish("0.10.0", "linux-x64", bytes);

	CountingTransport transport(makeLocalDirTransport(server.dir.path));
	const CheckResult check = checkForUpdate(transport, "packages/manifest.json", kLocal090, options);
	REQUIRE(check.updateAvailable());
	transport.fetches = 0;
	transport.requested.clear();   // the check above fetched the manifest; count from here

	SUBCASE("the happy path")
	{
		std::uint64_t lastDone = 0, lastTotal = 0;
		const DownloadResult d = downloadUpdate(transport, check, staging.path,
			[&](std::uint64_t done, std::uint64_t total) { lastDone = done; lastTotal = total; }, options);
		REQUIRE_MESSAGE(d.ok, d.error);
		CHECK_FALSE(d.alreadyStaged);
		CHECK(d.staged.version == "0.10.0");
		CHECK(d.staged.platform == "linux-x64");
		CHECK(d.staged.sha256 == sha256Hex(bytes));
		CHECK(readFile(d.staged.filePath) == bytes);
		CHECK(d.staged.filePath.filename() == "HorizonEditor-linux-x64.pkg");
		CHECK(lastDone == bytes.size());
		CHECK(lastTotal == bytes.size());
		CHECK(transport.requested == std::vector<std::string>{ "packages/releases/0.10.0/HorizonEditor-linux-x64.pkg" });

		// pending.json is what the takeover step reads back.
		StagedUpdate pending;
		std::string error;
		REQUIRE_MESSAGE(readPendingUpdate(staging.path, pending, error), error);
		CHECK(pending.version == "0.10.0");
		CHECK(pending.filePath == d.staged.filePath);
		CHECK(pending.sha256 == d.staged.sha256);
		CHECK(verifyStagedUpdate(pending, error));

		// Nothing half-written is left lying around.
		CHECK_FALSE(fs::exists(d.staged.filePath.string() + ".partial"));
		CHECK_FALSE(fs::exists(staging.path / "pending.json.tmp"));
	}
	SUBCASE("a second run finds it already staged and fetches nothing")
	{
		REQUIRE(downloadUpdate(transport, check, staging.path, {}, options).ok);
		transport.fetches = 0;
		const DownloadResult again = downloadUpdate(transport, check, staging.path, {}, options);
		REQUIRE_MESSAGE(again.ok, again.error);
		CHECK(again.alreadyStaged);
		CHECK(transport.fetches == 0);
	}
	SUBCASE("a staged file that no longer verifies is fetched again")
	{
		const DownloadResult first = downloadUpdate(transport, check, staging.path, {}, options);
		REQUIRE(first.ok);
		writeFile(first.staged.filePath, std::string(bytes.size(), 'X'));   // same size, wrong content
		transport.fetches = 0;
		const DownloadResult again = downloadUpdate(transport, check, staging.path, {}, options);
		REQUIRE_MESSAGE(again.ok, again.error);
		CHECK_FALSE(again.alreadyStaged);
		CHECK(transport.fetches == 1);
		CHECK(readFile(again.staged.filePath) == bytes);
	}
	SUBCASE("a file that differs from the manifest is thrown away, and an earlier staging stays valid")
	{
		// An earlier build is staged first ...
		FakeServer earlier;
		earlier.publish("0.9.5", "linux-x64", "earlier build");
		CountingTransport earlierT(makeLocalDirTransport(earlier.dir.path));
		const CheckResult earlierCheck = checkForUpdate(earlierT, "packages/manifest.json", kLocal090, options);
		REQUIRE(earlierCheck.updateAvailable());
		const DownloadResult staged = downloadUpdate(earlierT, earlierCheck, staging.path, {}, options);
		REQUIRE(staged.ok);

		// ... then the server's file is swapped for something else of the same size.
		writeFile(server.artifactFile(check), std::string(bytes.size(), 'Z'));
		const DownloadResult d = downloadUpdate(transport, check, staging.path, {}, options);
		CHECK_FALSE(d.ok);
		CHECK(d.error.find("SHA-256") != std::string::npos);
		CHECK_FALSE(fs::exists(staging.path / "0.10.0-linux-x64"));

		StagedUpdate pending;
		std::string error;
		REQUIRE_MESSAGE(readPendingUpdate(staging.path, pending, error), error);
		CHECK(pending.version == "0.9.5");
		CHECK(verifyStagedUpdate(pending, error));
	}
	SUBCASE("a truncated file is caught by its size before it is hashed")
	{
		writeFile(server.artifactFile(check), bytes.substr(0, bytes.size() / 2));
		const DownloadResult d = downloadUpdate(transport, check, staging.path, {}, options);
		CHECK_FALSE(d.ok);
		CHECK(d.error.find("bytes") != std::string::npos);
		CHECK_FALSE(fs::exists(staging.path / "pending.json"));
	}
	SUBCASE("an artifact that is gone from the server fails cleanly")
	{
		fs::remove(server.artifactFile(check));
		const DownloadResult d = downloadUpdate(transport, check, staging.path, {}, options);
		CHECK_FALSE(d.ok);
		CHECK(d.error.find(check.artifactRemotePath) != std::string::npos);
		CHECK_FALSE(fs::exists(staging.path / "0.10.0-linux-x64"));
	}
	SUBCASE("a dropped connection mid-download is retried")
	{
		transport.failTransientTimes = 1;
		const DownloadResult d = downloadUpdate(transport, check, staging.path, {}, options);
		REQUIRE_MESSAGE(d.ok, d.error);
		CHECK(transport.fetches == 2);
	}
	SUBCASE("only a build the check offered can be downloaded")
	{
		CheckResult nothing;
		nothing.status = CheckStatus::UpToDate;
		CHECK_FALSE(downloadUpdate(transport, nothing, staging.path, {}, options).ok);
		CHECK_FALSE(downloadUpdate(transport, check, fs::path(), {}, options).ok);
	}
	SUBCASE("a newer build replaces the older staged one")
	{
		REQUIRE(downloadUpdate(transport, check, staging.path, {}, options).ok);
		CHECK(fs::is_directory(staging.path / "0.10.0-linux-x64"));

		server.publish("0.11.0", "linux-x64", "the next one");
		const CheckResult next = checkForUpdate(transport, "packages/manifest.json", kLocal090, options);
		REQUIRE(next.updateAvailable());
		const DownloadResult d = downloadUpdate(transport, next, staging.path, {}, options);
		REQUIRE_MESSAGE(d.ok, d.error);
		CHECK(fs::is_directory(staging.path / "0.11.0-linux-x64"));
		CHECK_FALSE(fs::exists(staging.path / "0.10.0-linux-x64"));
	}
	SUBCASE("pending.json cannot point outside the staging directory")
	{
		REQUIRE(downloadUpdate(transport, check, staging.path, {}, options).ok);
		const fs::path outside = scratch.path / "outside.bin";
		writeFile(outside, bytes);
		writeFile(staging.path / "pending.json",
		          "{\"schema\":1,\"version\":\"0.10.0\",\"platform\":\"linux-x64\",\"file\":\"../" +
		              scratch.path.filename().string() + "/outside.bin\",\"sha256\":\"" + sha256Hex(bytes) + "\",\"size\":" +
		              std::to_string(bytes.size()) + "}");
		StagedUpdate pending;
		std::string error;
		CHECK_FALSE(readPendingUpdate(staging.path, pending, error));
		CHECK_FALSE(error.empty());
	}
	SUBCASE("nothing staged is not an error")
	{
		StagedUpdate pending;
		std::string error = "stale";
		CHECK_FALSE(readPendingUpdate(staging.path, pending, error));
		CHECK(error.empty());
	}
	SUBCASE("discarding removes what this module staged and leaves the rest")
	{
		REQUIRE(downloadUpdate(transport, check, staging.path, {}, options).ok);
		writeFile(staging.path / "foreign" / "keep.txt", "not ours");
		discardStagedUpdates(staging.path);
		CHECK_FALSE(fs::exists(staging.path / "pending.json"));
		CHECK_FALSE(fs::exists(staging.path / "0.10.0-linux-x64"));
		CHECK(fs::exists(staging.path / "foreign" / "keep.txt"));
	}
}

// ─── live, against a real SFTP server ────────────────────────────────────────

// Off unless HE_SELFUPDATE_LIVE_TEST=1: it talks to whatever HE_SELFUPDATE_* names,
// so it must be asked for. It proves the part the tests above cannot — that the
// second account's endpoint, SftpClient and a real server work together. The
// server needs the layout of a real one (<channel>/manifest.json, and the artifact
// the manifest names for this platform beside it) and a version newer than 0.0.1.
// ── What earlier takeovers left beside the install (Takeover.h) ──────────────
namespace {

// <parent>/.heselfupdate-<name>/ with a backup/ (a finished swap) or only a
// payload/ (an aborted or in-flight one), dated `age` ago.
fs::path seedWorkDir(const fs::path& parent, const std::string& name, bool withBackup, std::chrono::hours age)
{
	const fs::path d = parent / (".heselfupdate-" + name);
	writeFile(d / (withBackup ? "backup" : "payload") / "HorizonEditor", "old build");
	fs::last_write_time(d, fs::file_time_type::clock::now() - age);
	return d;
}

void writeLastApply(const fs::path& file, const std::string& status, const std::string& toVersion,
                    const fs::path& backupRoot)
{
	writeFile(file, "{\"schema\":1,\"status\":\"" + status + "\",\"error\":\"\",\"fromVersion\":\"0.6.0\","
	                "\"toVersion\":\"" + toVersion + "\",\"installRoot\":\"\",\"backupRoot\":" +
	                nlohmann::json(utf8FromPath(backupRoot)).dump() + ",\"newPid\":1}");
}

} // namespace

TEST_CASE("selfupdate: older update leftovers are pruned, the one last-apply.json names is kept")
{
	TempDir t;
	const fs::path install = t.path / "Editor";
	writeFile(install / "HorizonEditor", "running build");
	const fs::path oldest = seedWorkDir(t.path, "0.5.0-aaaa", true,  std::chrono::hours(72));
	const fs::path named  = seedWorkDir(t.path, "0.7.0-bbbb", true,  std::chrono::hours(48));
	const fs::path newer  = seedWorkDir(t.path, "0.6.0-cccc", true,  std::chrono::hours(24));  // newer, but not named
	const fs::path stale  = seedWorkDir(t.path, "0.7.0-dddd", false, std::chrono::hours(30));  // rolled back / aborted
	// Names that only look alike are none of this code's business.
	writeFile(t.path / "heselfupdate-0.4.0" / "backup" / "x", "x");
	writeFile(t.path / ".heselfupdate-file", "a file, not a work directory");
	writeLastApply(t.path / "last-apply.json", "applied", "0.7.0", named / "backup");

	PruneOptions o;
	o.installRoot    = install;
	o.lastApplyFile  = t.path / "last-apply.json";
	o.runningVersion = "0.7.0";
	const PruneResult r = pruneTakeoverLeftovers(o);

	CHECK(fs::equivalent(pathFromUtf8(r.kept), named));
	CHECK(fs::exists(named / "backup" / "HorizonEditor"));
	CHECK_FALSE(fs::exists(oldest));
	CHECK_FALSE(fs::exists(newer));
	CHECK_FALSE(fs::exists(stale));
	CHECK(r.removed.size() == 3);
	CHECK(r.errors.empty());
	CHECK(fs::exists(install / "HorizonEditor"));
	CHECK(fs::exists(t.path / "heselfupdate-0.4.0" / "backup" / "x"));
	CHECK(fs::exists(t.path / ".heselfupdate-file"));
}

TEST_CASE("selfupdate: right after a swap the newest backup is kept, whatever last-apply.json still says")
{
	// The relaunched editor starts while HorizonUpdater is still watching it:
	// last-apply.json is the PREVIOUS update's, applied to another version.
	TempDir t;
	const fs::path install = t.path / "Editor";
	writeFile(install / "HorizonEditor", "running build");
	const fs::path previous = seedWorkDir(t.path, "0.7.0-aaaa", true, std::chrono::hours(48));
	const fs::path justNow  = seedWorkDir(t.path, "0.8.0-bbbb", true, std::chrono::hours(1));
	writeLastApply(t.path / "last-apply.json", "applied", "0.7.0", previous / "backup");

	PruneOptions o;
	o.installRoot    = install;
	o.lastApplyFile  = t.path / "last-apply.json";
	o.runningVersion = "0.8.0";
	const PruneResult r = pruneTakeoverLeftovers(o);

	CHECK(fs::equivalent(pathFromUtf8(r.kept), justNow));
	CHECK(fs::exists(justNow / "backup"));
	CHECK_FALSE(fs::exists(previous));

	SUBCASE("and a missing last-apply.json picks the same one")
	{
		const fs::path older = seedWorkDir(t.path, "0.6.0-cccc", true, std::chrono::hours(96));
		o.lastApplyFile = t.path / "nothing-here.json";
		const PruneResult again = pruneTakeoverLeftovers(o);
		CHECK(fs::equivalent(pathFromUtf8(again.kept), justNow));
		CHECK_FALSE(fs::exists(older));
	}
}

TEST_CASE("selfupdate: nothing younger than the grace period is pruned")
{
	// A second editor on the same install may be mid-takeover: its payload/ and
	// even a just-swapped backup/ look like leftovers until HorizonUpdater is done.
	TempDir t;
	const fs::path install = t.path / "Editor";
	writeFile(install / "HorizonEditor", "running build");
	const fs::path kept     = seedWorkDir(t.path, "0.7.0-aaaa", true,  std::chrono::hours(48));
	const fs::path inFlight = seedWorkDir(t.path, "0.8.0-bbbb", false, std::chrono::hours(0));
	const fs::path old      = seedWorkDir(t.path, "0.6.0-cccc", true,  std::chrono::hours(72));
	writeLastApply(t.path / "last-apply.json", "applied", "0.7.0", kept / "backup");

	PruneOptions o;
	o.installRoot    = install;
	o.lastApplyFile  = t.path / "last-apply.json";
	o.runningVersion = "0.7.0";
	const PruneResult r = pruneTakeoverLeftovers(o);

	CHECK(fs::equivalent(pathFromUtf8(r.kept), kept));
	CHECK(fs::exists(inFlight / "payload" / "HorizonEditor"));
	CHECK(r.young.size() == 1);
	CHECK_FALSE(fs::exists(old));

	SUBCASE("with no work directories at all there is nothing to report")
	{
		TempDir empty;
		writeFile(empty.path / "Editor" / "HorizonEditor", "running build");
		o.installRoot = empty.path / "Editor";
		const PruneResult none = pruneTakeoverLeftovers(o);
		CHECK(none.kept.empty());
		CHECK(none.removed.empty());
		CHECK(none.young.empty());
	}
}

TEST_CASE("selfupdate: live round trip over SFTP" * doctest::skip(std::getenv("HE_SELFUPDATE_LIVE_TEST") == nullptr))
{
	const SelfUpdateConfig config = loadSelfUpdateConfig();
	REQUIRE_MESSAGE(config.ready(), config.problem);

	TempDir scratch, staging;
	const CheckOptions options = fastOptions(scratch.path);
	const CheckResult check = checkForUpdate(config, { "0.0.1", currentPlatformKey() }, options);
	REQUIRE_MESSAGE(check.updateAvailable(), check.error);
	MESSAGE("live: offered ", check.remoteVersion, " (", check.artifact.size, " bytes) at ", check.artifactRemotePath);

	const auto transport = makeSftpTransport(config.endpoint);
	const DownloadResult d = downloadUpdate(*transport, check, staging.path, {}, options);
	REQUIRE_MESSAGE(d.ok, d.error);

	StagedUpdate pending;
	std::string error;
	REQUIRE_MESSAGE(readPendingUpdate(staging.path, pending, error), error);
	CHECK(verifyStagedUpdate(pending, error));
	CHECK(fs::file_size(pending.filePath) == check.artifact.size);

	// The very same version must now read as up to date.
	CHECK(checkForUpdate(config, { check.remoteVersion, currentPlatformKey() }, options).status == CheckStatus::UpToDate);
}

TEST_CASE("selfupdate: live, a wrong password is refused and never printed" * doctest::skip(std::getenv("HE_SELFUPDATE_LIVE_TEST") == nullptr))
{
	SelfUpdateConfig config = loadSelfUpdateConfig();
	REQUIRE_MESSAGE(config.ready(), config.problem);
	const std::string wrong = config.endpoint.password + "-wrong";
	config.endpoint.password = wrong;

	TempDir scratch;
	const CheckResult check = checkForUpdate(config, { "0.0.1", currentPlatformKey() }, fastOptions(scratch.path));
	CHECK(check.status == CheckStatus::Failed);
	CHECK(check.error.find("authentication") != std::string::npos);
	CHECK(check.error.find(wrong) == std::string::npos);
	CHECK(check.error.find(config.endpoint.password.substr(0, config.endpoint.password.size() - 6)) == std::string::npos);
}
