#include "ContentSync/SftpProbe.h"
#include "ContentSync/SftpClient.h"
#include "ContentSync/SftpCredentials.h"
#include "ContentSync/CsLog.h"

namespace HE::Cs {

SftpProbeResult probeSftp()
{
	SftpProbeResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	result.configured = endpoint.configured();
	if (!result.configured)
	{
		result.detail = "EngineContent SFTP endpoint not configured (empty host/username in the build)";
		// Logged, not just returned. The verdict used to live only in the
		// SftpProbeResult, and the Editor's one caller stores it in a member
		// nothing ever reads — so an endpoint the build left empty produced an
		// Engine folder with no server content, no error, and nothing anywhere
		// to say why. The manifest layer below never gets to speak either: its
		// own "not configured" warning sits behind probe.ready().
		HE_LOG_WARN(ContentSync, "%s",
			"EngineContent: SFTP endpoint not configured — this build has no host/username "
			"compiled in, so the Engine folder shows local content only. Fill in "
			"cmake/EngineContentCredentials.cmake (copy the .template) and configure again.");
		return result;
	}

	const SftpResult conn = sftpTestConnection(endpoint);
	result.reachable = conn.ok;
	result.detail    = conn.error;

	// Host and user, never the password — same rule the configure-time message
	// follows. conn.error already went through detail::scrub().
	if (conn.ok)
		HE_LOG_INFO(ContentSync, "EngineContent: %s@%s:%u reachable",
			endpoint.username.c_str(), endpoint.host.c_str(), (unsigned)endpoint.port);
	else
		HE_LOG_WARN(ContentSync, "EngineContent: cannot reach %s@%s:%u — %s",
			endpoint.username.c_str(), endpoint.host.c_str(), (unsigned)endpoint.port,
			result.detail.c_str());

	return result;
}

} // namespace HE::Cs
