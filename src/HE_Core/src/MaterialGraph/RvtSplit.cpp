#include "MaterialGraph/RvtSplit.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace HE
{
namespace
{
using T = MatNodeType;

// FNV-1a 64: stable across platforms and runs (std::hash is neither), cheap enough to run on every graph edit.
struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    void i32(int v) { bytes(&v, sizeof v); }
    void f32(float v)
    {
        if (v == 0.0f) v = 0.0f;               // -0 and +0 are the same parameter
        uint32_t u; std::memcpy(&u, &v, sizeof u);
        bytes(&u, sizeof u);
    }
    void str(const std::string& s) { i32(static_cast<int>(s.size())); bytes(s.data(), s.size()); }
};

// Forward reachability through the links, starting at `seeds`.
std::set<int> forward(const MaterialGraph& g, const std::set<int>& seeds)
{
    std::set<int> seen = seeds;
    std::vector<int> work(seeds.begin(), seeds.end());
    while (!work.empty())
    {
        const int id = work.back(); work.pop_back();
        for (const MatGraphLink& l : g.links)
            if (l.srcNode == id && seen.insert(l.dstNode).second)
                work.push_back(l.dstNode);
    }
    return seen;
}

const MatGraphLink* inputLink(const MaterialGraph& g, int node, int pin)
{
    for (const MatGraphLink& l : g.links)
        if (l.dstNode == node && l.dstPin == pin) return &l;
    return nullptr;
}

uint64_t pageHash(const MaterialGraph& g, const MatGraphNode& write, int writePin, const MatGraphLink& source,
                  const std::vector<int>& deps)
{
    Fnv f;
    f.str("rvt-page-v1");
    // The Write node: which virtual texture, at which density, which layers, and which pin this page is.
    f.str(write.s);
    f.f32(write.p[0]); f.f32(write.p[1]);
    f.i32(writePin);
    f.i32(source.srcNode); f.i32(source.srcPin);
    // The dependency nodes, by id: type, parameters, string. Not x/y, group, tooltip, tess (editor and Output-only data).
    for (int id : deps)
    {
        const MatGraphNode* n = g.findNode(id);
        if (!n) continue;
        f.i32(n->id); f.i32(static_cast<int>(n->type));
        for (float v : n->p) f.f32(v);
        f.str(n->s);
    }
    // The wiring between them, in a canonical order (the link vector's order is an editing accident).
    std::vector<MatGraphLink> ls;
    for (const MatGraphLink& l : g.links)
        if (std::binary_search(deps.begin(), deps.end(), l.dstNode) && std::binary_search(deps.begin(), deps.end(), l.srcNode))
            ls.push_back(l);
    std::sort(ls.begin(), ls.end(), [](const MatGraphLink& a, const MatGraphLink& b) {
        if (a.dstNode != b.dstNode) return a.dstNode < b.dstNode;
        if (a.dstPin != b.dstPin)   return a.dstPin < b.dstPin;
        if (a.srcNode != b.srcNode) return a.srcNode < b.srcNode;
        return a.srcPin < b.srcPin;
    });
    for (const MatGraphLink& l : ls) { f.i32(l.srcNode); f.i32(l.srcPin); f.i32(l.dstNode); f.i32(l.dstPin); }
    return f.h ? f.h : 1;
}
} // namespace

std::vector<int> matGraphDependencies(const MaterialGraph& graph, int pinSource)
{
    std::set<int> seen{ pinSource };
    std::vector<int> work{ pinSource };
    while (!work.empty())
    {
        const int id = work.back(); work.pop_back();
        for (const MatGraphLink& l : graph.links)
            if (l.dstNode == id && seen.insert(l.srcNode).second)
                work.push_back(l.srcNode);
    }
    return std::vector<int>(seen.begin(), seen.end());
}

std::set<int> matRvtDownstreamOfRead(const MaterialGraph& graph)
{
    std::set<int> reads;
    for (const MatGraphNode& n : graph.nodes)
        if (n.type == T::RvtRead) reads.insert(n.id);
    return forward(graph, reads);
}

MatRvtSplit matSplitRvt(const MaterialGraph& graph)
{
    MatRvtSplit r;
    r.live = graph;
    std::vector<const MatGraphNode*> writes;
    for (const MatGraphNode& n : graph.nodes)
    {
        if (n.type == T::RvtRead) r.hasRvtNodes = true;
        if (n.type == T::RvtWrite) { r.hasRvtNodes = true; writes.push_back(&n); }
    }
    if (!r.hasRvtNodes) return r;

    std::sort(writes.begin(), writes.end(), [](const MatGraphNode* a, const MatGraphNode* b) { return a->id < b->id; });
    const std::set<int> afterRead = matRvtDownstreamOfRead(graph);

    std::set<std::string> seenNames;
    std::set<int> pageNodes; // nodes of every page that was made
    const MatNodeDesc& wd = matNodeDesc(T::RvtWrite);
    for (const MatGraphNode* w : writes)
    {
        if (!seenNames.insert(w->s).second)
        {
            r.problems.push_back("RVT Write #" + std::to_string(w->id) + ": another Write already paints the RVT '" + w->s
                                 + "' (lowest id wins, this one is ignored)");
            continue;
        }
        for (int pin = 0; pin < kRvtWritePinCount; ++pin)
        {
            const MatGraphLink* src = inputLink(graph, w->id, pin);
            if (!src) continue;
            const std::string pinName = wd.inputs[pin].name;
            const std::vector<int> deps = matGraphDependencies(graph, src->srcNode);
            // Static check: nothing downstream of a Read may feed a page. A Read in the chain is also what makes a node
            // downstream of one, so this single test covers both (a page that reads the pool it is painted into).
            bool bad = false;
            for (int id : deps) bad = bad || afterRead.count(id) != 0;
            if (bad)
            {
                r.problems.push_back("RVT Write #" + std::to_string(w->id) + " '" + w->s + "' " + pinName
                                     + ": the pin depends on an RVT Read, so no page shader can be cut for it");
                continue;
            }
            MatRvtPage page;
            page.rvtName = w->s; page.writeNode = w->id; page.writePin = pin; page.pinName = pinName;
            page.sourceNodes = deps;
            page.hash = pageHash(graph, *w, pin, *src, deps);
            // The page graph: the dependency nodes under their source ids, the links between them, and an unlit Output.
            // Float sources reach the Vec3 Base Color pin through the codegen's own pin cast.
            page.graph.nextId = graph.nextId;
            for (int id : deps) if (const MatGraphNode* n = graph.findNode(id)) page.graph.nodes.push_back(*n);
            for (const MatGraphLink& l : graph.links)
                if (std::binary_search(deps.begin(), deps.end(), l.dstNode) && std::binary_search(deps.begin(), deps.end(), l.srcNode))
                    page.graph.links.push_back(l);
            page.outputNode = page.graph.addNode(T::Output);
            page.graph.findNode(page.outputNode)->p[0] = 0.0f; // unlit: the frame IS the value
            page.graph.connect(src->srcNode, src->srcPin, page.outputNode, kMatOutputBaseColorPin);
            for (int id : deps) pageNodes.insert(id);
            r.pages.push_back(std::move(page));
        }
    }

    // The live graph: no Write node, and no node that only a page needed. The emit is demand-driven from the Output, so the
    // pruned nodes would not be emitted anyway; pruning makes "the live shader holds only what is live" checkable.
    std::set<int> drop;
    for (const MatGraphNode& n : graph.nodes) if (n.type == T::RvtWrite) drop.insert(n.id);
    MaterialGraph live = graph;
    live.links.erase(std::remove_if(live.links.begin(), live.links.end(), [&](const MatGraphLink& l) {
        return drop.count(l.dstNode) != 0 || drop.count(l.srcNode) != 0; }), live.links.end());
    std::set<int> needed;
    std::vector<int> work;
    for (const MatGraphNode& n : live.nodes)
        if (n.type == T::Output) { needed.insert(n.id); work.push_back(n.id); }
    while (!work.empty())
    {
        const int id = work.back(); work.pop_back();
        for (const MatGraphLink& l : live.links)
            if (l.dstNode == id && needed.insert(l.srcNode).second) work.push_back(l.srcNode);
    }
    for (int id : pageNodes) if (!needed.count(id)) drop.insert(id);
    live.nodes.erase(std::remove_if(live.nodes.begin(), live.nodes.end(), [&](const MatGraphNode& n) {
        return drop.count(n.id) != 0; }), live.nodes.end());
    live.links.erase(std::remove_if(live.links.begin(), live.links.end(), [&](const MatGraphLink& l) {
        return drop.count(l.dstNode) != 0 || drop.count(l.srcNode) != 0; }), live.links.end());
    r.live = std::move(live);
    return r;
}
} // namespace HE
