#include "ContentSync/SftpCredentials.h"

// The endpoint comes from the BUILD, never from this file — see the header for
// why, and src/HE_ContentSync/CMakeLists.txt for the three ways a value gets
// in. Undefined (a plain configure with nothing supplied) leaves host and
// username empty, which SftpProbe reports as a clear "not configured" instead
// of trying to reach a placeholder.
#ifndef HE_ENGINE_CONTENT_HOST
#define HE_ENGINE_CONTENT_HOST ""
#endif
#ifndef HE_ENGINE_CONTENT_PORT
#define HE_ENGINE_CONTENT_PORT 22
#endif
#ifndef HE_ENGINE_CONTENT_USER
#define HE_ENGINE_CONTENT_USER ""
#endif
#ifndef HE_ENGINE_CONTENT_PASSWORD
#define HE_ENGINE_CONTENT_PASSWORD ""
#endif
#ifndef HE_ENGINE_CONTENT_BASEPATH
#define HE_ENGINE_CONTENT_BASEPATH ""
#endif

namespace HE::Cs {

const SftpEndpoint& engineContentEndpoint()
{
	static const SftpEndpoint kEndpoint = []
	{
		SftpEndpoint e;
		e.host           = HE_ENGINE_CONTENT_HOST;
		e.port           = (std::uint16_t)HE_ENGINE_CONTENT_PORT;
		e.username       = HE_ENGINE_CONTENT_USER;
		e.password       = HE_ENGINE_CONTENT_PASSWORD;
		e.remoteBasePath = HE_ENGINE_CONTENT_BASEPATH;   // "" = the account's own root
		return e;
	}();
	return kEndpoint;
}

} // namespace HE::Cs
