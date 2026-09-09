#pragma once

#include <HorizonScene/HcCodegen.h>
#include <string>
#include <vector>

// ── Which HorizonCode classes shipped interpreted, and why ───────────────────
// The codegen already knows: a graph it cannot translate never fails the run,
// it becomes a Result::Fallback and that class ships interpreted (the export
// dialog's default, OnFailure::Interpret). Until now the verdict only reached
// the build log — where it scrolls past between two hundred compiler lines —
// and hc_report.txt beside the generated sources, which nobody opens after an
// export that said "OK". So an export that compiled fifteen of sixteen classes
// looked on screen exactly like one that compiled all sixteen.
//
// This is the model behind the warning list the build window shows: the
// fallbacks joined against the sources that produced them, so a line can name
// the class the way its author knows it rather than by registry key. It changes
// nothing about the fallback itself — the same graphs ship the same way, they
// are just no longer silent about it.
//
// Deliberately free of ImGui and AppContext: this is what the test asserts on,
// and the dialogs only draw it.
namespace HcFallbackReport
{
	struct Notice
	{
		std::string key;      // registry key ("Content/Enemy.hasset", "level:Main")
		std::string label;    // what the author calls it; the key when there is none
		std::string reason;   // the codegen's own words — already prose, passed through
		int         node = 0; // the graph node it anchors to; 0 = the whole graph
	};

	// The classes of `sources` that `res` could not compile, in source order —
	// the order the build log translated them in, so the two read alike.
	std::vector<Notice> collect(const std::vector<HE::hccg::ClassSource>& sources,
	                            const HE::hccg::Result& res);

	// One line: "Enemy.hasset — exec cycle at node 12 (node 12)" reads twice, so
	// the node suffix is only appended when the reason does not already carry it.
	std::string describe(const Notice& n);

	// The sentence above the list. `buildFailed` means the toolchain step did not
	// produce a library, in which case EVERY class ships interpreted and saying
	// "N of T" would still overclaim — the point of the whole exercise.
	std::string headline(size_t total, size_t interpreted, bool buildFailed);
}
