#include "SelfUpdate/SelfUpdateEndpoint.h"
#include "SelfUpdate/SelfUpdateManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace HE::Su {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

} // namespace

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

SelfUpdateConfig loadSelfUpdateConfig(const EnvLookup& envIn)
{
	const EnvLookup env = envIn ? envIn : EnvLookup([](const char* name) { return std::getenv(name); });

	auto get = [&](const char* name) -> std::string
	{
		const char* v = env(name);
		return v ? std::string(v) : std::string();
	};

	const std::string host     = get(kEnvHost);
	const std::string user     = get(kEnvUser);
	const std::string password = get(kEnvPassword);
	const std::string port     = get(kEnvPort);
	const std::string basePath = get(kEnvBasePath);
	const std::string channel  = get(kEnvChannel);

	SelfUpdateConfig cfg;

	// The channel alone does not switch the feature on: it only picks a folder on
	// a server that has not been named yet.
	if (host.empty() && user.empty() && password.empty() && port.empty() && basePath.empty())
		return cfg;   // NotSet

	cfg.state = ConfigState::Invalid;

	std::string missing;
	if (host.empty())     missing += std::string(missing.empty() ? "" : ", ") + kEnvHost;
	if (user.empty())     missing += std::string(missing.empty() ? "" : ", ") + kEnvUser;
	if (password.empty()) missing += std::string(missing.empty() ? "" : ", ") + kEnvPassword;
	if (!missing.empty())
	{
		cfg.problem = "self-update account is incomplete — still missing: " + missing;
		return cfg;
	}

	std::uint16_t portNumber = 22;
	if (!port.empty())
	{
		unsigned long parsed = 0;
		bool digits = port.size() <= 5;
		for (char c : port)
			if (!std::isdigit(static_cast<unsigned char>(c))) digits = false;
		if (digits) parsed = std::strtoul(port.c_str(), nullptr, 10);
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

	cfg.endpoint.host           = host;
	cfg.endpoint.port           = portNumber;
	cfg.endpoint.username       = user;
	cfg.endpoint.password       = password;
	cfg.endpoint.remoteBasePath = basePath;

	// Rule 2: the same login as the EngineContent account is not a second account.
	if (sameAccount(cfg.endpoint, HE::Cs::engineContentEndpoint()))
	{
		cfg.problem = "the self-update account is the same login as the EngineContent account — "
		              "the update path needs its own, separate account";
		cfg.endpoint = {};
		return cfg;
	}

	cfg.state = ConfigState::Ready;
	return cfg;
}

} // namespace HE::Su
