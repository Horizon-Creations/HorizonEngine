#pragma once

// ─── Is this machine able to do source control at all? ───────────────────────
// Answered once at startup, off the frame thread, so the editor can say what is
// missing instead of failing at the first commit.
//
// Four things are checked, and each fails differently for the user:
//   • git itself — missing entirely on many Windows machines.
//   • git-lfs — missing on almost every machine until someone installs it, and
//     it is the component that carries the large assets, so without it the very
//     files source control exists for are the ones that break.
//   • user.name / user.email — configured on a developer's machine and unset on
//     a fresh one. `git commit` refuses without them, so this is the single most
//     common way a first commit fails, and it is invisible until that moment.
//   • a credential helper — without one, every push would need the token typed
//     again, and there is nowhere safe to keep it.
//
// The probe deliberately reports all four rather than a single boolean: "source
// control unavailable" is not an actionable message, and three of the four have
// completely different remedies.

#include "SourceControl/ScCommon.h"

#include <filesystem>
#include <string>

namespace HE::Sc {

struct GitProbe
{
	bool                  gitFound = false;
	std::string           gitVersion;         // "2.43.0", empty when unparsable
	std::filesystem::path gitPath;

	bool        lfsFound = false;
	std::string lfsVersion;                   // "3.4.1"

	// Both must be set for `git commit` to work at all.
	bool        identityConfigured = false;
	std::string userName;
	std::string userEmail;

	// The configured credential.helper, or empty. Not an error on its own — the
	// UI offers to set the platform default — but it must be known before a
	// token is handed over, since without a helper there is nowhere to put it.
	std::string credentialHelper;

	// What was tried and what came back, for the "details" section of the dialog.
	// Never contains a token: it is built through the scrubbing log helpers.
	std::string detail;

	// Everything needed to make a first commit. The credential helper is not part
	// of this — you can commit locally without one; it is only pushing that needs
	// somewhere to keep a token.
	bool ready() const { return gitFound && lfsFound && identityConfigured; }
};

// Runs several git invocations; blocking, and belongs on a worker thread.
HE_SC_API GitProbe probeGit();

// ── Pure helpers, exposed so version parsing is testable without git present ──
// git reports "git version 2.43.0" and, on Apple systems, "git version 2.39.3
// (Apple Git-146)"; git-lfs reports "git-lfs/3.4.1 (GitHub; darwin arm64; go
// 1.21.0)". Both are parsed to a bare "2.43.0" / "3.4.1".
HE_SC_API std::string parseGitVersion(const std::string& versionOutput);
HE_SC_API std::string parseLfsVersion(const std::string& versionOutput);

} // namespace HE::Sc
