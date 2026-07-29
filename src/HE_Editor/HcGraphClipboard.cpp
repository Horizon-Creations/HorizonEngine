#include "HcGraphClipboard.h"

#include <HorizonCode/HorizonCode.h>

#include <algorithm>
#include <unordered_map>

namespace HC = HorizonCode;

namespace
{
// Process-wide, so a copy in the Level Script tab pastes into a Widget graph.
std::string g_payload;

bool copyable(const HC::Node& n)
{
	return n.type != HC::NodeType::Event && n.type != HC::NodeType::FunctionEntry;
}
} // namespace

namespace HcClipboard
{

bool copy(const HC::Graph& g, const std::vector<int>& ids)
{
	HC::Graph tmp;
	for (int id : ids)
		if (const HC::Node* n = g.findNode(id); n && copyable(*n))
			tmp.nodes.push_back(*n);
	if (tmp.nodes.empty()) return false;

	auto inSel = [&](int id) {
		return std::any_of(tmp.nodes.begin(), tmp.nodes.end(),
		                   [&](const HC::Node& n) { return n.id == id; });
	};
	for (const HC::Link& l : g.links)
		if (inSel(l.srcNode) && inSel(l.dstNode)) tmp.links.push_back(l);

	// Variables ride along by NAME only (Get/SetVariable nodes carry `s`), so a
	// paste into a graph without that variable leaves an unbound node the user
	// can rebind — pasting a variable DECLARATION would collide with the
	// destination's own names.
	g_payload = HC::toJson(tmp);
	return true;
}

std::vector<int> paste(HC::Graph& g, float atX, float atY, int subgraph)
{
	std::vector<int> fresh;
	HC::Graph tmp;
	if (g_payload.empty() || !HC::fromJson(g_payload, tmp) || tmp.nodes.empty())
		return fresh;

	float mnx = tmp.nodes.front().x, mny = tmp.nodes.front().y;
	for (const HC::Node& n : tmp.nodes) { mnx = std::min(mnx, n.x); mny = std::min(mny, n.y); }

	std::unordered_map<int, int> remap; // clipboard id → new id
	for (const HC::Node& n : tmp.nodes)
	{
		if (!copyable(n)) continue; // defensive: an old payload could carry one
		HC::Node c  = n;
		c.x         = n.x - mnx + atX;
		c.y         = n.y - mny + atY;
		c.subgraph  = subgraph;     // land in the sub-graph being edited
		const int id = g.addNode(std::move(c)); // assigns a fresh id from g.nextId
		remap[n.id] = id;
		fresh.push_back(id);
	}
	for (const HC::Link& l : tmp.links)
	{
		const auto s = remap.find(l.srcNode), d = remap.find(l.dstNode);
		if (s != remap.end() && d != remap.end())
			g.links.push_back({ s->second, l.srcPin, d->second, l.dstPin });
	}
	return fresh;
}

bool empty() { return g_payload.empty(); }

} // namespace HcClipboard
