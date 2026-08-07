#include "ContentSync/SftpProbe.h"
#include "ContentSync/SftpClient.h"
#include "ContentSync/SftpCredentials.h"

namespace HE::Cs {

SftpProbeResult probeSftp()
{
	SftpProbeResult result;
	const SftpEndpoint& endpoint = engineContentEndpoint();
	result.configured = endpoint.configured();
	if (!result.configured)
	{
		result.detail = "EngineContent SFTP endpoint not configured (see SftpCredentials.cpp)";
		return result;
	}

	const SftpResult conn = sftpTestConnection(endpoint);
	result.reachable = conn.ok;
	result.detail    = conn.error;
	return result;
}

} // namespace HE::Cs
