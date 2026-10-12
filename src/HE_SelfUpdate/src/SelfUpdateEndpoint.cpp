#include "SelfUpdate/SelfUpdateEndpoint.h"
#include "SelfUpdate/SelfUpdateManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

// The compiled-in account comes from the BUILD, never from this file — see
// src/HE_SelfUpdate/CMakeLists.txt for the three ways a value gets in. Undefined
// (a plain configure with nothing supplied) leaves it empty, which the loader
// reports as NotSet unless the environment names an account.
#ifndef HE_SELFUPDATE_HOST
#define HE_SELFUPDATE_HOST ""
#endif
#ifndef HE_SELFUPDATE_PORT
#define HE_SELFUPDATE_PORT 22
#endif
#ifndef HE_SELFUPDATE_USER
#define HE_SELFUPDATE_USER ""
#endif
#ifndef HE_SELFUPDATE_PASSWORD
#define HE_SELFUPDATE_PASSWORD ""
#endif
#ifndef HE_SELFUPDATE_BASEPATH
#define HE_SELFUPDATE_BASEPATH ""
#endif

namespace HE::Su {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// The account variables as strings, whichever place they came from.
struct RawAccount
{
	std::string host, user, password, port, basePath;
};

// Validates `raw` into a config. `source` is not None; `where` opens a problem text.
SelfUpdateConfig buildConfig(const RawAccount& raw, const std::string& channel, ConfigSource source, const std::string& where)
{
	SelfUpdateConfig cfg;
	cfg.state  = ConfigState::Invalid;
	cfg.source = source;

	std::string missing;
	if (raw.host.empty())     missing += std::string(missing.empty() ? "" : ", ") + kEnvHost;
	if (raw.user.empty())     missing += std::string(missing.empty() ? "" : ", ") + kEnvUser;
	if (raw.password.empty()) missing += std::string(missing.empty() ? "" : ", ") + kEnvPassword;
	if (!missing.empty())
	{
		cfg.problem = where + " is incomplete — still missing: " + missing;
		return cfg;
	}

	std::uint16_t portNumber = 22;
	if (!raw.port.empty())
	{
		unsigned long parsed = 0;
		bool digits = raw.port.size() <= 5;
		for (char c : raw.port)
			if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
		if (digits) parsed = std::strtoul(raw.port.c_str(), nullptr, 10);
		if (!digits || parsed < 1 || parsed > 65535)
		{
			cfg.problem = std::string(kEnvPort) + " must be a number from 1 to 65535";
			return cfg;
		}
		portNumber = static_cast<std::uint16_t>(parsed);
	}

	if (!channel.empty())
	{
		if (channel.find('/') != std::string::npos || !isSafeRelativePath(channel))
		{
			cfg.problem = std::string(kEnvChannel) + " may only contain letters, digits and . _ + -";
			return cfg;
		}
		cfg.channel = channel;
	}

	cfg.endpoint.host           = raw.host;
	cfg.endpoint.port           = portNumber;
	cfg.endpoint.username       = raw.user;
	cfg.endpoint.password       = raw.password;
	cfg.endpoint.remoteBasePath = raw.basePath;

	// Rule 2: the same login as the EngineContent account is not a second account.
	if (sameAccount(cfg.endpoint, HE::Cs::engineContentEndpoint()))
	{
		cfg.problem = where + " is the same login as the EngineContent account — "
		              "the update path needs its own, separate account";
		cfg.endpoint = {};
		return cfg;
	}

	cfg.state = ConfigState::Ready;
	return cfg;
}

} // namespace

const HE::Cs::SftpEndpoint& builtInSelfUpdateEndpoint()
{
	static const HE::Cs::SftpEndpoint kEndpoint = []
	{
		HE::Cs::SftpEndpoint e;
		e.host           = HE_SELFUPDATE_HOST;
		e.port           = (std::uint16_t)HE_SELFUPDATE_PORT;
		e.username       = HE_SELFUPDATE_USER;
		e.password       = HE_SELFUPDATE_PASSWORD;
		e.remoteBasePath = HE_SELFUPDATE_BASEPATH;   // "" = the account's own root
		return e;
	}();
	return kEndpoint;
}

std::string SelfUpdateConfig::manifestRemotePath() const
{
	return channel + "/manifest.json";
}

bool sameAccount(const HE::Cs::SftpEndpoint& a, const HE::Cs::SftpEndpoint& b)
{
	if (!a.configured() || !b.configured()) return false;
	return lower(a.host) == lower(b.host) && a.port == b.port && a.username == b.username;
}

std::string scrubSecret(std::string text, const std::string& secret)
{
	if (secret.empty()) return text;
	std::size_t pos = 0;
	while ((pos = text.find(secret, pos)) != std::string::npos)
	{
		text.replace(pos, secret.size(), "<redacted>");
		pos += 10;   // length of "<redacted>"
	}
	return text;
}

SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& env)
{
	return loadSelfUpdateConfig(env, builtInSelfUpdateEndpoint());
}

SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& envIn, const HE::Cs::SftpEndpoint& builtIn)
{
	const EnvLookup env = envIn ? envIn : EnvLookup([](const char* name) { return std::getenv(name); });

	auto get = [&](const char* name) -> std::string
	{
		const char* v = env(name);
		return v ? std::string(v) : std::string();
	};

	const RawAccount fromEnv { get(kEnvHost), get(kEnvUser), get(kEnvPassword), get(kEnvPort), get(kEnvBasePath) };
	const std::string channel = get(kEnvChannel);

	const bool hasBuiltIn = !builtIn.host.empty() || !builtIn.username.empty() || !builtIn.password.empty();

	// Rule 1a: any account variable in the environment replaces the compiled-in
	// account as a whole. The channel alone does not count: it only picks a folder
	// on a server, it does not name one.
	if (!fromEnv.host.empty() || !fromEnv.user.empty() || !fromEnv.password.empty() ||
	    !fromEnv.port.empty() || !fromEnv.basePath.empty())
	{
		return buildConfig(fromEnv, channel, ConfigSource::Environment,
		                   hasBuiltIn ? "the self-update account from HE_SELFUPDATE_* (which replaces the compiled-in one as a whole)"
		                              : "the self-update account from HE_SELFUPDATE_*");
	}

	// Rule 1b: the account compiled into this build.
	if (hasBuiltIn)
	{
		const RawAccount baked { builtIn.host, builtIn.username, builtIn.password,
		                         std::to_string(builtIn.port), builtIn.remoteBasePath };
		return buildConfig(baked, channel, ConfigSource::BuiltIn, "the self-update account compiled into this build");
	}

	return {};   // NotSet
}

} // namespace HE::Su
