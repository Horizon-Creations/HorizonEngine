#include "HcFallbackReport.h"

#include <algorithm>

namespace HcFallbackReport
{

std::vector<Notice> collect(const std::vector<HE::hccg::ClassSource>& sources,
                            const HE::hccg::Result& res)
{
	std::vector<Notice> out;
	// Walked source-first, not fallback-first: `fallbacks` is in the order the
	// classes were EMITTED (topological — bases before the classes that derive
	// from them), and that is not the order anyone picked the assets in.
	for (const HE::hccg::ClassSource& src : sources)
	{
		const auto fb = std::find_if(res.fallbacks.begin(), res.fallbacks.end(),
			[&](const HE::hccg::Result::Fallback& f) { return f.key == src.key; });
		if (fb == res.fallbacks.end()) continue;
		Notice n;
		n.key    = src.key;
		n.label  = src.label.empty() ? src.key : src.label;
		n.reason = fb->reason;
		n.node   = fb->node;
		out.push_back(std::move(n));
	}
	// A fallback whose key is in no source would be a bug in the caller (the
	// generator only ever reports keys it was handed), but dropping it silently
	// would hide exactly the kind of thing this file exists to stop hiding.
	for (const HE::hccg::Result::Fallback& f : res.fallbacks)
	{
		const bool known = std::any_of(sources.begin(), sources.end(),
			[&](const HE::hccg::ClassSource& s) { return s.key == f.key; });
		if (known) continue;
		out.push_back(Notice{ f.key, f.key, f.reason, f.node });
	}
	return out;
}

std::string describe(const Notice& n)
{
	std::string line = (n.label.empty() ? n.key : n.label);
	line += " \xe2\x80\x94 ";           // em dash
	line += n.reason;
	// Several reasons end in "…at node 12" already; appending the same number a
	// second time reads as two different nodes.
	if (n.node != 0 &&
	    n.reason.find("node " + std::to_string(n.node)) == std::string::npos)
		line += " (node " + std::to_string(n.node) + ")";
	return line;
}

std::string headline(size_t total, size_t interpreted, bool buildFailed)
{
	if (buildFailed)
	{
		// Nothing was compiled at all, so the count of validation fallbacks is
		// no longer the interesting number — it is a footnote to "all of them".
		std::string s = "HorizonCode was not compiled: all " + std::to_string(total)
		              + " class(es) ship interpreted.";
		if (interpreted > 0)
			s += " " + std::to_string(interpreted) + " of them could not be translated:";
		return s;
	}
	return std::to_string(interpreted) + " of " + std::to_string(total)
	     + " HorizonCode class(es) ship interpreted, not compiled:";
}

} // namespace HcFallbackReport
