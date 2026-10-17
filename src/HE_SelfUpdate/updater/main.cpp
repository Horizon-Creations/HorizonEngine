// HorizonUpdater — the whole program. Started detached by the editor
// (Takeover.cpp) with an ApplyPlan on its command line, it waits for the editor
// to be gone, swaps the install, relaunches, watches the new build's health,
// rolls back if it looks dead on arrival, and writes what happened to
// plan.resultFile. See SelfUpdateApplyCore.h for the actual logic — this file
// is only argv in, exit code out.
//
// Links no engine library (HE_SU_STANDALONE, see SuCommon.h): a copy of this
// executable alone, in a temp directory, has to be able to run.

#include "SelfUpdate/SelfUpdateApplyCore.h"
#include "SelfUpdate/SelfUpdateApplyPlan.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	std::vector<std::string> args;
	args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
	for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

	HE::Su::ApplyPlan plan;
	std::string error;
	if (!HE::Su::ApplyPlan::fromArgs(args, plan, error))
	{
		std::fprintf(stderr, "HorizonUpdater: %s\n", error.c_str());
		return 2;
	}

	HE::Su::appendApplyLog(plan.logFile, "HorizonUpdater started (" + plan.fromVersion + " -> " + plan.toVersion + ")");
	const HE::Su::ApplyResult result = HE::Su::applyUpdate(plan);

	const bool ok = result.status == HE::Su::ApplyStatus::Applied || result.status == HE::Su::ApplyStatus::RolledBack;
	return ok ? 0 : 1;
}
