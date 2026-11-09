#include "MaterialGraph/RvtSplit.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
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

namespace
{
std::atomic<bool> g_rvtEnabled{ true };
}

void matSetRvtEnabled(bool enabled) { g_rvtEnabled.store(enabled, std::memory_order_relaxed); }

bool matRvtEnabled() { return g_rvtEnabled.load(std::memory_order_relaxed); }

MaterialGraph matInlineRvt(const MaterialGraph& graph)
{
    bool any = false;
    for (const MatGraphNode& n : graph.nodes) any = any || n.type == T::RvtRead || n.type == T::RvtWrite;
    if (!any) return graph;

    MaterialGraph g = graph;
    std::map<std::string, int> writeOf; // RVT name -> lowest Write id
    for (const MatGraphNode& n : g.nodes)
        if (n.type == T::RvtWrite)
        {
            auto it = writeOf.find(n.s);
            if (it == writeOf.end() || n.id < it->second) writeOf[n.s] = n.id;
        }
    auto isRead = [&](int id) { const MatGraphNode* n = g.findNode(id); return n && n->type == T::RvtRead; };
    int constOne = 0, constZero = 0;
    auto constant = [&](bool one) {
        int& id = one ? constOne : constZero;
        if (!id) { id = g.addNode(T::ConstFloat); g.findNode(id)->p[0] = one ? 1.0f : 0.0f; }
        return id;
    };

    // A Write input may itself be fed by another Read: a pass resolves one level, so repeat. A loop of Reads and Writes would never
    // settle; the pass count bounds it and the leftover links die with their Read nodes below.
    for (size_t pass = 0; pass <= g.nodes.size(); ++pass)
    {
        bool changed = false;
        std::vector<MatGraphLink> next;
        next.reserve(g.links.size());
        const std::vector<MatGraphLink> before = g.links;
        for (const MatGraphLink& l : before)
        {
            if (!isRead(l.srcNode)) { next.push_back(l); continue; }
            changed = true;
            const MatGraphNode* rd = g.findNode(l.srcNode);
            const auto w = writeOf.find(rd->s);
            if (l.srcPin == kRvtReadValidPin)
            {
                MatGraphLink n = l; n.srcNode = constant(w != writeOf.end()); n.srcPin = 0;
                next.push_back(n);
            }
            else if (w != writeOf.end() && l.srcPin >= 0 && l.srcPin <= kRvtReadAOPin)
            {
                // Read pins 0..3 are Write pins 0..3 (Base Color, Normal, Roughness, AO).
                for (const MatGraphLink& s : before)
                    if (s.dstNode == w->second && s.dstPin == l.srcPin)
                    {
                        MatGraphLink n = l; n.srcNode = s.srcNode; n.srcPin = s.srcPin;
                        next.push_back(n);
                        break;
                    }
            }
        }
        g.links = std::move(next);
        if (!changed) break;
    }

    g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const MatGraphLink& l) {
        const MatGraphNode* a = g.findNode(l.srcNode); const MatGraphNode* b = g.findNode(l.dstNode);
        return (a && (a->type == T::RvtRead || a->type == T::RvtWrite)) || (b && (b->type == T::RvtRead || b->type == T::RvtWrite));
    }), g.links.end());
    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [](const MatGraphNode& n) {
        return n.type == T::RvtRead || n.type == T::RvtWrite; }), g.nodes.end());
    return g;
}

namespace
{
int paramComponents(MatNodeType t)
{
    switch (t)
    {
        case T::ParamFloat: case T::ParamBool: return 1;
        case T::ParamVec2:  return 2;
        case T::ParamColor: return 3;
        case T::ParamVec4:  return 4;
        default: return 0;
    }
}

// What a node reads of the ground, by its type.
uint32_t groundReadsOf(MatNodeType t)
{
    switch (t)
    {
        // World Position carries the ground's height in Y (the page is flat in XZ, the height comes from the mesh).
        case T::WorldPos: return kRvtReadsHeight;
        // The normal, and the normal-map nodes (their frame is the screen-space derivative of the world position).
        case T::NormalWS: case T::NormalMapSample: case T::NormalMapArraySample:
        case T::NormalMapBombSample: case T::NormalMapArrayBombSample: return kRvtReadsNormal | kRvtReadsHeight;
        case T::LandscapeLayerBlend: case T::LandscapeGroupPick: return kRvtReadsWeights;
        default: return 0;
    }
}

bool isFrozenInput(MatNodeType t)
{
    switch (t)
    {
        case T::Time: case T::Panner: case T::Fresnel: case T::ViewDir: case T::CameraPos: case T::CameraDistance:
        case T::ScreenPos: case T::Weather: case T::WaterRipple: case T::Sun: case T::Wind: case T::WindSway: return true;
        default: return false;
    }
}

bool switchValue(const MatGraphNode& n, const MatRvtEnv& env, std::string& name)
{
    name = n.s.empty() ? "switch_" + std::to_string(n.id) : n.s;
    const auto it = env.switches.find(name);
    return it != env.switches.end() ? it->second : n.p[0] > 0.5f;
}
} // namespace

uint32_t matRvtNodeGroundReads(MatNodeType type) { return groundReadsOf(type); }

MatRvtPageDeps matRvtPageDeps(const MaterialGraph& g, const MatRvtPage& page, const MatRvtEnv& env)
{
    MatRvtPageDeps d;
    const MatGraphLink* src = inputLink(g, page.writeNode, page.writePin);
    if (!src) return d;
    std::set<int> seen{ src->srcNode };
    std::vector<int> work{ src->srcNode };
    std::set<std::string> params, switches, assets, frozen;
    while (!work.empty())
    {
        const int id = work.back(); work.pop_back();
        const MatGraphNode* n = g.findNode(id);
        if (!n) continue;
        int onlyPin = -1;
        if (n->type == T::StaticSwitch)
        {
            std::string name;
            onlyPin = switchValue(*n, env, name) ? 0 : 1;
            switches.insert(name);
        }
        d.reads |= groundReadsOf(n->type);
        if (paramComponents(n->type) > 0) params.insert(n->s);
        if (matNodeSamplesTexture(n->type) && !n->s.empty()) assets.insert(n->s);
        if (n->type == T::FunctionCall)
        {
            assets.insert(n->s);
            const auto it = env.functionReads.find(n->s);
            d.reads |= it != env.functionReads.end() ? it->second : kRvtReadsGround;
        }
        if (isFrozenInput(n->type)) frozen.insert(matNodeDesc(n->type).name);
        for (const MatGraphLink& l : g.links)
            if (l.dstNode == id && (onlyPin < 0 || l.dstPin == onlyPin) && seen.insert(l.srcNode).second)
                work.push_back(l.srcNode);
    }
    d.nodes.assign(seen.begin(), seen.end());
    d.params.assign(params.begin(), params.end());
    d.switches.assign(switches.begin(), switches.end());
    d.assets.assign(assets.begin(), assets.end());
    d.frozen.assign(frozen.begin(), frozen.end());
    return d;
}

uint64_t matRvtPageStateHash(const MaterialGraph& g, const MatRvtPage& page, const MatRvtEnv& env)
{
    const MatGraphLink* src = inputLink(g, page.writeNode, page.writePin);
    const MatGraphNode* write = g.findNode(page.writeNode);
    if (!src || !write) return page.hash;
    const MatRvtPageDeps d = matRvtPageDeps(g, page, env);
    Fnv f;
    f.str("rvt-state-v1");
    // The structure and the node constants of the live nodes; the untaken side of a Static Switch is not in d.nodes.
    const uint64_t base = pageHash(g, *write, page.writePin, *src, d.nodes);
    f.bytes(&base, sizeof base);
    // The value each parameter takes: the effective one if the material resolves one, else the node's own default.
    for (int id : d.nodes)
    {
        const MatGraphNode* n = g.findNode(id);
        const int nc = n ? paramComponents(n->type) : 0;
        if (nc == 0) continue;
        f.str(n->s);
        const auto it = env.params.find(n->s);
        for (int c = 0; c < nc; ++c) f.f32(it != env.params.end() ? it->second[static_cast<size_t>(c)] : n->p[c]);
    }
    for (const std::string& sw : d.switches)
    {
        const auto it = env.switches.find(sw);
        f.str(sw);
        f.i32(it != env.switches.end() ? (it->second ? 1 : 0) : -1);   // -1: the node's own default, already part of `base`
    }
    for (const std::string& a : d.assets)
    {
        const auto it = env.assetRevision.find(a);
        const uint64_t rev = it != env.assetRevision.end() ? it->second : 0;
        f.str(a);
        f.bytes(&rev, sizeof rev);
    }
    return f.h ? f.h : 1;
}

MatRvtMaterialState matRvtMaterialState(const MaterialGraph& g, const MatRvtSplit& split, const MatRvtEnv& env)
{
    MatRvtMaterialState r;
    Fnv f;
    f.str("rvt-material-v1");
    for (const MatRvtPage& pg : split.pages)
    {
        r.pageDeps.push_back(matRvtPageDeps(g, pg, env));
        r.pageHashes.push_back(matRvtPageStateHash(g, pg, env));
        r.reads |= r.pageDeps.back().reads;
        f.i32(pg.writeNode); f.i32(pg.writePin);
        f.bytes(&r.pageHashes.back(), sizeof(uint64_t));
    }
    r.state = f.h ? f.h : 1;
    return r;
}

MatRvtLayerState matRvtLayerState(const MatRvtSplit& split, const MatRvtMaterialState& state, const std::string& rvtName)
{
    // The same page per pin as matRvtLayerViews takes (the first of that RVT), and the same pin-to-layer cut.
    const MatRvtPage* pg[4] = { nullptr, nullptr, nullptr, nullptr };
    size_t idx[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < split.pages.size(); ++i)
    {
        const MatRvtPage& p = split.pages[i];
        if (p.rvtName == rvtName && p.writePin >= 0 && p.writePin < 4 && !pg[p.writePin]) { pg[p.writePin] = &p; idx[p.writePin] = i; }
    }
    static const int kPinsOfLayer[3][2] = { { kRvtWriteBaseColorPin, -1 }, { kRvtWriteNormalPin, -1 }, { kRvtWriteRoughnessPin, kRvtWriteAOPin } };
    MatRvtLayerState r;
    for (int k = 0; k < 3; ++k)
    {
        Fnv f;
        f.str("rvt-layer-v1");
        f.i32(k);
        for (int pin : kPinsOfLayer[k])
        {
            if (pin < 0 || !pg[pin]) continue;
            f.i32(pin);
            if (idx[pin] < state.pageHashes.size()) f.bytes(&state.pageHashes[idx[pin]], sizeof(uint64_t));
            if (idx[pin] < state.pageDeps.size()) r.reads[k] |= state.pageDeps[idx[pin]].reads;
        }
        r.hash[k] = f.h ? f.h : 1;
    }
    return r;
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

MatRvtLayerViews matRvtLayerViews(const MatRvtSplit& split, const std::string& rvtName)
{
    MatRvtLayerViews r;
    const MatRvtPage* pg[4] = { nullptr, nullptr, nullptr, nullptr };
    for (const MatRvtPage& p : split.pages)
        if (p.rvtName == rvtName && p.writePin >= 0 && p.writePin < 4 && !pg[p.writePin]) pg[p.writePin] = &p;
    if (!pg[0] && !pg[1] && !pg[2] && !pg[3]) return r;

    struct Src { int node = 0; int pin = 0; };
    // Copies page `p` into `dst` (nodes and links both pages share are taken once) and returns what its Output was fed by.
    auto take = [](MaterialGraph& dst, const MatRvtPage& p) {
        for (const MatGraphNode& n : p.graph.nodes)
            if (n.id != p.outputNode && !dst.findNode(n.id)) dst.nodes.push_back(n);
        for (const MatGraphLink& l : p.graph.links)
        {
            if (l.dstNode == p.outputNode) continue;
            bool have = false;
            for (const MatGraphLink& d : dst.links)
                have = have || (d.dstNode == l.dstNode && d.dstPin == l.dstPin);
            if (!have) dst.links.push_back(l);
        }
        Src s;
        for (const MatGraphLink& l : p.graph.links)
            if (l.dstNode == p.outputNode && l.dstPin == kMatOutputBaseColorPin) { s.node = l.srcNode; s.pin = l.srcPin; }
        return s;
    };
    int nextId = 1;
    for (const MatRvtPage* p : pg) if (p) nextId = std::max(nextId, p->graph.nextId);
    auto begin = [&](MaterialGraph& g) {
        g.nextId = nextId;
        const int out = g.addNode(T::Output);
        g.findNode(out)->p[0] = 0.0f;               // unlit: the frame IS the value
        return out;
    };
    auto constant = [](MaterialGraph& g, float a, float b, float c) {
        const int n = g.addNode(T::ConstColor);
        g.findNode(n)->p[0] = a; g.findNode(n)->p[1] = b; g.findNode(n)->p[2] = c;
        return Src{ n, 0 };
    };
    auto scalar = [](MaterialGraph& g, float v) {
        const int n = g.addNode(T::ConstFloat);
        g.findNode(n)->p[0] = v;
        return n;
    };

    {   // layer 0: colour
        MaterialGraph& g = r.view[0];
        const int out = begin(g);
        const Src s = pg[0] ? take(g, *pg[0]) : constant(g, 0.5f, 0.5f, 0.5f);
        g.connect(s.node, s.pin, out, kMatOutputBaseColorPin);
        r.pins[0] = 1u << kRvtWriteBaseColorPin;
    }
    {   // layer 1: normal, n * 0.5 + 0.5
        MaterialGraph& g = r.view[1];
        const int out = begin(g);
        const Src s = pg[1] ? take(g, *pg[1]) : constant(g, 0.0f, 1.0f, 0.0f);
        const int m = g.addNode(T::Multiply), a = g.addNode(T::Add), half = scalar(g, 0.5f);
        g.connect(s.node, s.pin, m, 0);
        g.connect(half, 0, m, 1);
        g.connect(m, 0, a, 0);
        g.connect(half, 0, a, 1);
        g.connect(a, 0, out, kMatOutputBaseColorPin);
        r.pins[1] = 1u << kRvtWriteNormalPin;
    }
    {   // layer 2: AO, roughness
        MaterialGraph& g = r.view[2];
        const int out = begin(g);
        const Src ao = pg[3] ? take(g, *pg[3]) : Src{ scalar(g, 1.0f), 0 };
        const Src rg = pg[2] ? take(g, *pg[2]) : Src{ scalar(g, 1.0f), 0 };
        const int c = g.addNode(T::Combine3), zero = scalar(g, 0.0f);
        g.connect(ao.node, ao.pin, c, 0);
        g.connect(rg.node, rg.pin, c, 1);
        g.connect(zero, 0, c, 2);
        g.connect(c, 0, out, kMatOutputBaseColorPin);
        r.pins[2] = (1u << kRvtWriteRoughnessPin) | (1u << kRvtWriteAOPin);
    }
    r.ok = true;
    return r;
}
} // namespace HE
