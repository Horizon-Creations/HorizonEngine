#pragma once

// ─── The takeover itself: wait, swap, relaunch, roll back ─────────────────────
// Compiled into HorizonSelfUpdate (so the Editor's tests can drive it in-process)
// AND into HorizonUpdater (HE_SU_STANDALONE, no engine library at all) — see
// SelfUpdateApplyPlan.h for why the two must share one implementation.
//
// applyUpdate() trusts its ApplyPlan. Everything that can be checked while the
// Editor is still running — the staged build re-verifying, the archive already
// extracted and its executable present, the install directory writable, the
// updater binary actually starting — is the EDITOR's job, before it ever builds
// a plan or quits (see Takeover.h). By the time this runs, the editor process is
// on its way out and there is nobody left to show an error to; every failure
// from here on is written to the result file and, wherever the old build can
// still be told apart from the new one, answered by relaunching SOMETHING rather
// than leaving the user with no running editor.

#include "SelfUpdate/SelfUpdateApplyPlan.h"
#include "SelfUpdate/SuCommon.h"

#include <string>

namespace HE::Su {

// Waits for `plan.waitPid`, swaps `installRoot` and `payloadRoot` (via
// `backupRoot`), copies `preserve` over, relaunches and watches the new build's
// health, and rolls back if it looks dead on arrival. Never throws. Always
// returns a result, and — when `plan.resultFile` is set — has already written it
// before returning, so a process killed right after this call still leaves a
// record the next editor start can read.
HE_SU_API ApplyResult applyUpdate(const ApplyPlan& plan);

// `path` empty is a no-op success (nowhere asked for a record). A write failure
// (directory not creatable, disk full) is reported but never thrown.
HE_SU_API bool writeApplyResult(const std::string& path, const ApplyResult& result, std::string& error);

// False with `error` set: no such file, or it does not parse. Tolerant of fields
// a newer writer added (`schema` tracks the ones this reads).
HE_SU_API bool readApplyResult(const std::string& path, ApplyResult& out, std::string& error);

// Appends one UTC-timestamped line to `logFile` (created if missing). A `logFile`
// of "" is a silent no-op — tests that do not care about the log pass one.
HE_SU_API void appendApplyLog(const std::string& logFile, const std::string& line);

} // namespace HE::Su
