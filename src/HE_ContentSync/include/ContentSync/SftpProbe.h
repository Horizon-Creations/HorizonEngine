#pragma once

// ─── Can the EngineContent SFTP endpoint actually be reached right now? ──────
// Answered once at Editor startup, off the frame thread — mirrors GitProbe's
// role for source control (see SourceControl/GitProbe.h), but much smaller:
// there is no external tool to detect (libssh2 is linked in, not shelled out
// to) and no local identity to configure. The only two things that can be
// wrong are "nobody filled in SftpCredentials.h yet" and "the network/server
// says no right now" — and they need different messages, so both are reported
// rather than a single boolean.

#include "ContentSync/CsCommon.h"

#include <string>

namespace HE::Cs {

struct SftpProbeResult
{
	bool configured = false;   // SftpCredentials.h has a non-empty host/username
	bool reachable  = false;   // connect + authenticate succeeded (only meaningful if configured)
	std::string detail;        // scrubbed error text when !reachable, empty otherwise

	bool ready() const { return configured && reachable; }
};

// Blocking; belongs on a worker thread (mirrors probeGit()).
HE_CS_API SftpProbeResult probeSftp();

} // namespace HE::Cs
