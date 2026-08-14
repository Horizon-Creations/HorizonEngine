#include "HorizonCode/HcClassResolve.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace HorizonCode {

namespace
{
// An entry point is what a host can start execution at, and therefore what an
// override replaces. Everything else in a graph is body.
bool isEntryPoint(const Node& n)
{
    return n.type == NodeType::Event || n.type == NodeType::FunctionEntry;
}

// Two entry points are "the same member" when they are the same KIND with the
// same name — an event and a function may share a name without colliding.
bool sameMember(const Node& a, const Node& b)
{
    return a.type == b.type && !a.s.empty() && a.s == b.s;
}

// Drop `entry` from `g`, plus the nodes only it owns, plus every link that loses
// an endpoint.
//
// For a FunctionEntry that is exact: a function's body is partitioned by
// `subgraph` (assignSubgraphs runs as part of fromJson), so its nodes are known
// precisely.
//
// For an Event it is deliberately only the Event NODE. An event handler's chain
// lives in sub-graph 0 together with every other handler's, and nodes are freely
// shared between them — a Print, a Get Variable, a Branch reached from two
// events. Pruning by reachability would have to subtract what every OTHER entry
// point reaches, and getting that subtraction subtly wrong deletes live nodes
// from the base class. What is left behind is unreachable instead: the
// interpreter only ever enters through an entry point, and the codegen only
// emits from one, so dead nodes cost a little memory and nothing else.
void removeMember(Graph& g, int entryId, NodeType entryType)
{
    // Taken BY VALUE, not as a reference into g.nodes: this function erases
    // from that very vector, and a reference the caller handed in would
    // dangle the moment it did.
    std::unordered_set<int> doomed{ entryId };
    if (entryType == NodeType::FunctionEntry)
        for (const Node& n : g.nodes)
            if (n.subgraph == entryId) doomed.insert(n.id);

    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(),
        [&](const Node& n) { return doomed.count(n.id) != 0; }), g.nodes.end());
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
        [&](const Link& l) { return doomed.count(l.srcNode) || doomed.count(l.dstNode); }),
        g.links.end());
}

int highestId(const Graph& g)
{
    int hi = g.nextId - 1;
    for (const Node& n : g.nodes) hi = std::max(hi, n.id);
    return hi;
}
} // namespace

void mergeDerivedInto(Graph& base, Graph derived)
{
    // 1. Give the incoming nodes ids that cannot collide with the base's. The
    //    BASE keeps its ids: it was loaded first, and its links already address
    //    them.
    const int shift = highestId(base);
    if (shift > 0)
    {
        for (Node& n : derived.nodes)
        {
            n.id += shift;
            if (n.subgraph) n.subgraph += shift;   // the owning FunctionEntry moved too
        }
        for (Link& l : derived.links) { l.srcNode += shift; l.dstNode += shift; }
    }

    // 2. Overrides: an incoming entry point whose member the base also declares
    //    replaces it outright. This is the whole of "only the override runs".
    for (const Node& d : derived.nodes)
    {
        if (!isEntryPoint(d)) continue;
        for (const Node& b : base.nodes)
            if (sameMember(b, d)) { removeMember(base, b.id, b.type); break; }
    }

    // 3. Bodies and wires.
    base.nodes.insert(base.nodes.end(), derived.nodes.begin(), derived.nodes.end());
    base.links.insert(base.links.end(), derived.links.begin(), derived.links.end());

    // 4. Variables: a derived declaration of the same name replaces the base's,
    //    which is what shadowing means — one name, one slot in the instance's
    //    store, the nearest declaration's type and default.
    for (Variable& dv : derived.variables)
    {
        auto it = std::find_if(base.variables.begin(), base.variables.end(),
                               [&](const Variable& bv) { return bv.name == dv.name; });
        if (it != base.variables.end()) *it = std::move(dv);
        else                            base.variables.push_back(std::move(dv));
    }

    // 5. Declared events (the class's raise-able interface) union by name.
    for (EventDecl& de : derived.events)
    {
        auto it = std::find_if(base.events.begin(), base.events.end(),
                               [&](const EventDecl& be) { return be.name == de.name; });
        if (it != base.events.end()) *it = std::move(de);
        else                         base.events.push_back(std::move(de));
    }

    base.nextId = highestId(base) + 1;
}

namespace
{
// Walk `key` upwards, collecting each ancestor's graph. Returns root-first, so
// the caller can merge in the order the overrides have to be applied.
struct Ancestry
{
    std::vector<std::string> keysRootFirst;
    std::vector<Graph>       graphsRootFirst;
    std::string              engineBase;
    bool                     ok = false;
};

Ancestry walk(const std::string& key, const ClassLoader& load)
{
    Ancestry out;
    std::vector<std::string> keys;     // leaf → root
    std::vector<Graph>       graphs;
    std::unordered_set<std::string> seen;

    std::string cur = key;
    while (!cur.empty())
    {
        if (!seen.insert(cur).second)
        {
            // A derives from B derives from A. Stop and keep what resolved — a
            // half-resolved class still runs its own logic, which beats hanging
            // or silently producing an empty one.
            HE_LOG_ERROR(HorizonCode, "%s",
                ("HorizonCode: class inheritance cycle at '" + cur +
                 "' — the chain above it is ignored").c_str());
            break;
        }
        std::string json, base;
        if (!load(cur, json, base))
        {
            if (cur == key) return out;   // the class itself is missing: nothing to do
            HE_LOG_ERROR(HorizonCode, "%s",
                ("HorizonCode: base class '" + cur + "' was not found — the chain "
                 "stops there").c_str());
            break;
        }
        Graph g;
        if (!json.empty()) fromJson(json, g);
        keys.push_back(cur);
        graphs.push_back(std::move(g));

        // An engine taxonomy row (or nothing) ends the walk; anything else is
        // another class asset. The two share one string field and cannot
        // collide: an asset path always carries a '/' and a '.hasset'.
        if (base.empty() || findEngineClass(base)) { out.engineBase = base; break; }
        cur = base;
    }
    if (keys.empty()) return out;

    out.keysRootFirst.assign(keys.rbegin(), keys.rend());
    out.graphsRootFirst.assign(std::make_move_iterator(graphs.rbegin()),
                               std::make_move_iterator(graphs.rend()));
    out.ok = true;
    return out;
}
} // namespace

ResolvedClass resolveClass(const std::string& key, const ClassLoader& load)
{
    ResolvedClass out;
    Ancestry a = walk(key, load);
    if (!a.ok) return out;

    out.engineBase = a.engineBase;
    // Nearest ancestor first, excluding the class itself — the order a Cast
    // reads it in, and the order the add menu offers overrides in.
    // The loop already walks size-2 down to 0, i.e. parent → root, which IS
    // nearest-first — no reversal.
    for (size_t i = a.keysRootFirst.size() - 1; i-- > 0; )
        out.chain.push_back(a.keysRootFirst[i]);

    out.graph = std::move(a.graphsRootFirst.front());
    for (size_t i = 1; i < a.graphsRootFirst.size(); ++i)
        mergeDerivedInto(out.graph, std::move(a.graphsRootFirst[i]));

    // A derived Call Function reaching a base's function needs the mirrored
    // signature, and the merge just brought the two into one graph for the
    // first time.
    syncFunctionSignatures(out.graph);
    out.ok = true;
    return out;
}

std::vector<OverridableMember> overridableMembers(const std::string& key,
                                                  const ClassLoader& load)
{
    std::vector<OverridableMember> out;
    Ancestry a = walk(key, load);
    if (!a.ok) return out;

    // Nearest ancestor first; the class ITSELF is skipped (you do not override
    // your own member, you edit it), and a name already taken by a nearer
    // declaration is not offered again.
    for (size_t i = a.keysRootFirst.size() - 1; i-- > 0; )
    {
        const std::string& from = a.keysRootFirst[i];
        for (const Node& n : a.graphsRootFirst[i].nodes)
        {
            if (!isEntryPoint(n) || !n.overridable || n.s.empty()) continue;
            const bool have = std::any_of(out.begin(), out.end(),
                [&](const OverridableMember& m) { return m.kind == n.type && m.name == n.s; });
            if (have) continue;
            out.push_back({ n.type, n.s, from, n });
        }
    }
    return out;
}

namespace
{
// The content system's answer to "what is this class's graph and base".
ClassLoader contentLoader(ContentManager& cm)
{
    return [&cm](const std::string& key, std::string& json, std::string& base)
    {
        const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(cm.loadAsset(key));
        if (!a) return false;
        json = a->graphJson;
        base = a->baseClass;
        return true;
    };
}
}

ResolvedClass resolveClassAsset(ContentManager& cm, const std::string& classPath)
{
    return resolveClass(classPath, contentLoader(cm));
}

std::vector<OverridableMember> overridableMembersOf(ContentManager& cm,
                                                    const std::string& classPath)
{
    return overridableMembers(classPath, contentLoader(cm));
}

} // namespace HorizonCode
