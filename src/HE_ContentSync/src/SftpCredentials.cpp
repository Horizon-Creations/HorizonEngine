#include "ContentSync/SftpCredentials.h"

namespace HE::Cs {

const SftpEndpoint& engineContentEndpoint()
{
	// ─── FILL THESE IN ─────────────────────────────────────────────────────────
	// See the comment in SftpCredentials.h for why these are hardcoded here
	// rather than in a settings file. Leave host/username empty to keep the
	// feature harmlessly disabled (SftpProbe reports "not configured").
	static const SftpEndpoint kEndpoint = []
	{
		SftpEndpoint e;
		e.host           = "";     // e.g. "example.com"
		e.port           = 22;
		e.username       = "";
		e.password       = "";
		e.remoteBasePath = "";     // "" = the SFTP account's own root
		return e;
	}();
	return kEndpoint;
}

} // namespace HE::Cs
