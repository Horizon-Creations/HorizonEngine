#include "CollabDocSync.h"

#include <HorizonCode/HorizonCode.h>
#include <MaterialGraph/MaterialGraph.h>
#include <ParticleGraph/ParticleGraph.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <UIWidget/UIWidgetTree.h>

#include <algorithm>
#include <memory>

namespace CollabDocSync
{
namespace
{
using Delta = HE::Net::CollabSession::DocDelta;

constexpr std::uint8_t kOpUpsert = 0;
constexpr std::uint8_t kOpRemove = 1;
// Reorder: `kind` names the list, `json` is its id sequence, `itemId` is unused.
constexpr std::uint8_t kOpOrder  = 2;

std::string joinIds(const std::vector<std::int64_t>& ids)
{
    std::string s;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        if (i) s += ',';
        s += std::to_string(ids[i]);
    }
    return s;
}

std::vector<std::int64_t> splitIds(const std::string& s)
{
    std::vector<std::int64_t> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        bool neg = false;
        if (s[i] == '-') { neg = true; ++i; }
        if (i >= s.size() || s[i] < '0' || s[i] > '9') break;
        std::int64_t acc = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') acc = acc * 10 + (s[i++] - '0');
        out.push_back(neg ? -acc : acc);
        if (i < s.size() && s[i] == ',') ++i;
    }
    return out;
}

// FNV-1a over a string, for the item ids that have no integer identity.
std::int64_t hashOf(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return static_cast<std::int64_t>(h);
}
} // namespace

std::int64_t linkKey(int srcNode, int srcPin, int dstNode, int dstPin)
{
    // Four 16-bit fields. Node ids and pin indices are small positive integers —
    // a graph with 65535 nodes is far past what the canvas can draw — and packing
    // them from the IDS (not vector indices) is what keeps the key stable when
    // nodes are reordered or something above them is deleted.
    const auto pack = [](int v) {
        return static_cast<std::uint64_t>(static_cast<std::uint16_t>(v));
    };
    return static_cast<std::int64_t>((pack(srcNode) << 48) | (pack(srcPin) << 32) |
                                     (pack(dstNode) << 16) |  pack(dstPin));
}

std::int64_t transitionKey(const std::string& from, const std::string& to,
                           const std::string& param)
{
    // Animator transitions name their endpoints by state NAME and have no id of
    // their own (the wart documented in AnimatorStateMachineGraph.h), so their
    // identity is the triple that makes them distinct in the editor.
    return hashOf(from + '\x1f' + to + '\x1f' + param);
}

// ─── Diff / apply ────────────────────────────────────────────────────────────

namespace
{
// One kind's items in DOCUMENT order. Order is preserved deliberately: the diff
// emits in it, so the peer's document ends up ordered the same way and both
// sides serialize to identical bytes. Anything hash-ordered here would make the
// two files differ for no semantic reason, and the whole-file autosave running
// underneath would then bounce that difference back and forth forever.
std::vector<std::pair<std::int64_t, std::string>> capture(const IDocAdapter& doc, Kind kind)
{
    std::vector<std::pair<std::int64_t, std::string>> out;
    doc.enumerate(kind, [&](std::int64_t id, std::string json) {
        out.emplace_back(id, std::move(json));
    });
    return out;
}
} // namespace

void seed(const IDocAdapter& doc, DocMirror& mirror, Scope)
{
    for (int k = 0; k < kKindCount; ++k)
    {
        mirror.items[k].clear();
        mirror.order[k].clear();
        for (auto& [id, json] : capture(doc, static_cast<Kind>(k)))
        {
            mirror.order[k].push_back(id);
            mirror.items[k].emplace(id, std::move(json));
        }
    }
    mirror.seeded = true;
}

void diffInto(const IDocAdapter& doc, DocMirror& mirror, Scope scope,
              std::vector<Delta>& out)
{
    // An unseeded mirror would report the whole document as newly created. That
    // is never what the peer needs — they already have it — so treat the first
    // sight of a document as the agreed baseline instead.
    if (!mirror.seeded) { seed(doc, mirror, scope); return; }

    for (int k = 0; k < kKindCount; ++k)
    {
        const Kind kind = static_cast<Kind>(k);
        auto& known = mirror.items[k];

        const auto current = capture(doc, kind);
        std::vector<std::int64_t> ids;
        ids.reserve(current.size());

        for (const auto& [id, json] : current)
        {
            ids.push_back(id);
            const auto it = known.find(id);
            if (it != known.end() && it->second == json) continue;   // untouched
            Delta d;
            d.scope  = static_cast<std::uint8_t>(scope);
            d.kind   = static_cast<std::uint8_t>(kind);
            d.op     = kOpUpsert;
            d.itemId = id;
            d.json   = json;
            out.push_back(std::move(d));
        }

        // Removals in the mirror's own order, so a batch is reproducible.
        for (std::int64_t id : mirror.order[k])
        {
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
            Delta d;
            d.scope  = static_cast<std::uint8_t>(scope);
            d.kind   = static_cast<std::uint8_t>(kind);
            d.op     = kOpRemove;
            d.itemId = id;
            out.push_back(std::move(d));
        }

        // A pure reorder changes no item's payload, so nothing above would catch
        // it — and in the UI designer sibling order is DRAW order, i.e. a real
        // edit. One delta carries the whole sequence; it is only sent when the
        // sequence actually moved, which a drag or a delete alone does not do.
        if (ids != mirror.order[k])
        {
            const bool sameSet = ids.size() == mirror.order[k].size();
            Delta d;
            d.scope  = static_cast<std::uint8_t>(scope);
            d.kind   = static_cast<std::uint8_t>(kind);
            d.op     = kOpOrder;
            d.itemId = 0;
            d.json   = joinIds(ids);
            // Skip when the difference is only that items were added or removed:
            // apply already appends in batch order, so the sequences agree.
            std::vector<std::int64_t> shrunk = mirror.order[k];
            shrunk.erase(std::remove_if(shrunk.begin(), shrunk.end(), [&](std::int64_t id) {
                return std::find(ids.begin(), ids.end(), id) == ids.end();
            }), shrunk.end());
            const bool onlyAppended =
                shrunk.size() <= ids.size() &&
                std::equal(shrunk.begin(), shrunk.end(), ids.begin());
            if (!(sameSet && ids == mirror.order[k]) && !onlyAppended)
                out.push_back(std::move(d));
        }

        known.clear();
        for (const auto& [id, json] : current) known.emplace(id, json);
        mirror.order[k] = std::move(ids);
    }
}

bool applyDeltas(IDocAdapter& doc, DocMirror& mirror, Scope scope,
                 const std::vector<Delta>& batch)
{
    // Ordered so the document is never momentarily inconsistent: links go before
    // the nodes they reference on the way out, and after them on the way in. A
    // node removed while a link still points at it is a dangling link, which the
    // canvas and the interpreter both have to be defended against otherwise.
    const auto pass = [&](std::uint8_t op, bool linksFirst) {
        bool any = false;
        for (int step = 0; step < 2; ++step)
        {
            for (const Delta& d : batch)
            {
                if (d.scope != static_cast<std::uint8_t>(scope) || d.op != op) continue;
                if (d.kind >= kKindCount) continue;   // a kind from a newer peer
                const Kind kind = static_cast<Kind>(d.kind);
                const bool isLink = (kind == Kind::Link);
                if ((step == 0) != (isLink == linksFirst)) continue;

                const bool ok = (op == kOpUpsert)
                    ? doc.upsert(kind, d.itemId, d.json)
                    : doc.remove(kind, d.itemId);
                if (!ok) continue;   // unusable payload — skip the item, not the batch
                any = true;

                auto& known = mirror.items[d.kind];
                if (op == kOpUpsert) known[d.itemId] = d.json;
                else                 known.erase(d.itemId);
            }
        }
        return any;
    };

    bool any = pass(kOpRemove, /*linksFirst=*/true);
    any = pass(kOpUpsert, /*linksFirst=*/false) || any;

    // Order last: the sequence a reorder names includes items the upserts above
    // have only just created.
    for (const Delta& d : batch)
    {
        if (d.scope != static_cast<std::uint8_t>(scope) || d.op != kOpOrder) continue;
        if (d.kind >= kKindCount) continue;
        if (doc.reorder(static_cast<Kind>(d.kind), splitIds(d.json))) any = true;
    }

    // The mirror has to end up describing what the document now holds, order
    // included — otherwise the very next diff would "discover" the peer's
    // ordering as a local change and send it straight back.
    if (any)
    {
        doc.afterApply();
        for (int k = 0; k < kKindCount; ++k)
        {
            mirror.order[k].clear();
            for (auto& [id, json] : capture(doc, static_cast<Kind>(k)))
            {
                mirror.order[k].push_back(id);
                mirror.items[k][id] = std::move(json);
            }
        }
    }
    return any;
}

// ─── Adapters ────────────────────────────────────────────────────────────────
namespace
{
namespace HC = HorizonCode;

// Keep a document's id allocator ahead of anything a peer introduced, or the
// next locally created item reuses an id that is already taken over there.
void bumpNextId(int& nextId, std::int64_t id)
{
    if (id >= nextId) nextId = static_cast<int>(id) + 1;
}


// Reorder `items` to match `ids`, keeping anything unmentioned at the end in its
// current relative order. Shared by every adapter: the rule is the same whatever
// the element type, and each writing its own is how the three would drift.
template <class Vec, class IdOf>
bool applyOrder(Vec& items, const std::vector<std::int64_t>& ids, IdOf idOf)
{
    Vec reordered;
    reordered.reserve(items.size());
    std::vector<bool> taken(items.size(), false);
    for (std::int64_t want : ids)
    {
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (taken[i] || idOf(items[i]) != want) continue;
            reordered.push_back(std::move(items[i]));
            taken[i] = true;
            break;
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!taken[i]) reordered.push_back(std::move(items[i]));
    if (reordered.size() != items.size()) return false;   // paranoia; never hit
    items = std::move(reordered);
    return true;
}

// ── HorizonCode graph ──
class HcAdapter final : public IDocAdapter
{
public:
    explicit HcAdapter(HC::Graph& g) : m_g(g) {}

    void enumerate(Kind kind, const ItemFn& fn) const override
    {
        switch (kind)
        {
            case Kind::Node:
                for (const HC::Node& n : m_g.nodes) fn(n.id, HC::nodeToJson(n));
                break;
            case Kind::Link:
                for (const HC::Link& l : m_g.links)
                    fn(linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin), linkJson(l));
                break;
            case Kind::Variable:
                // A variable is keyed by NAME (that is its identity everywhere in
                // the graph — Get/SetVariable nodes reference it by name).
                for (const HC::Variable& v : m_g.variables)
                    fn(hashOf(v.name), HC::variableToJson(v));
                break;
            default: break;
        }
    }

    bool upsert(Kind kind, std::int64_t id, const std::string& json) override
    {
        switch (kind)
        {
            case Kind::Node:
            {
                HC::Node n;
                if (!HC::nodeFromJson(json, n)) return false;
                if (HC::Node* existing = m_g.findNode(n.id)) *existing = std::move(n);
                else { bumpNextId(m_g.nextId, n.id); m_g.nodes.push_back(std::move(n)); }
                return true;
            }
            case Kind::Link:
            {
                HC::Link l;
                if (!linkFromJson(json, l)) return false;
                // Endpoints must exist — a link into nothing stalls the
                // interpreter's chain walk (the same rule fromJson applies).
                if (!m_g.findNode(l.srcNode) || !m_g.findNode(l.dstNode)) return false;
                if (findLink(l) == m_g.links.end()) m_g.links.push_back(l);
                return true;
            }
            case Kind::Variable:
            {
                HC::Variable v;
                if (!HC::variableFromJson(json, v)) return false;
                if (HC::Variable* existing = m_g.findVariable(v.name)) *existing = std::move(v);
                else m_g.variables.push_back(std::move(v));
                return true;
            }
            default: return false;
        }
    }

    bool remove(Kind kind, std::int64_t id) override
    {
        switch (kind)
        {
            case Kind::Node:
                if (!m_g.findNode(static_cast<int>(id))) return false;
                m_g.removeNode(static_cast<int>(id));   // takes its links with it
                return true;
            case Kind::Link:
            {
                const auto it = std::find_if(m_g.links.begin(), m_g.links.end(),
                    [id](const HC::Link& l) {
                        return linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin) == id;
                    });
                if (it == m_g.links.end()) return false;
                m_g.links.erase(it);
                return true;
            }
            case Kind::Variable:
            {
                const auto it = std::find_if(m_g.variables.begin(), m_g.variables.end(),
                    [id](const HC::Variable& v) { return hashOf(v.name) == id; });
                if (it == m_g.variables.end()) return false;
                m_g.variables.erase(it);
                return true;
            }
            default: return false;
        }
    }

    bool reorder(Kind kind, const std::vector<std::int64_t>& ids) override
    {
        if (kind == Kind::Node)
            return applyOrder(m_g.nodes, ids, [](const HC::Node& n) {
                return static_cast<std::int64_t>(n.id); });
        if (kind == Kind::Variable)
            return applyOrder(m_g.variables, ids, [](const HC::Variable& v) {
                return hashOf(v.name); });
        return false;
    }

    // Function interfaces live on the FunctionEntry; calls and returns mirror
    // them. A batch that moved an entry has to re-propagate, or the peer's call
    // nodes keep the old pin list.
    void afterApply() override { HC::syncFunctionSignatures(m_g); }

private:
    static std::string linkJson(const HC::Link& l)
    {
        return "[" + std::to_string(l.srcNode) + "," + std::to_string(l.srcPin) + "," +
                     std::to_string(l.dstNode) + "," + std::to_string(l.dstPin) + "]";
    }
    static bool linkFromJson(const std::string& json, HC::Link& out)
    {
        return parseFourInts(json, out.srcNode, out.srcPin, out.dstNode, out.dstPin);
    }
    std::vector<HC::Link>::iterator findLink(const HC::Link& l)
    {
        return std::find_if(m_g.links.begin(), m_g.links.end(), [&l](const HC::Link& o) {
            return o.srcNode == l.srcNode && o.srcPin == l.srcPin &&
                   o.dstNode == l.dstNode && o.dstPin == l.dstPin;
        });
    }
    HC::Graph& m_g;

public:
    // "[a,b,c,d]" — the array link form both HorizonCode and this layer use.
    // Hand-parsed rather than pulled through nlohmann so the adapters stay free
    // of a JSON dependency for the one shape that is four integers.
    static bool parseFourInts(const std::string& s, int& a, int& b, int& c, int& d)
    {
        int v[4] = {}; int n = 0; std::size_t i = 0;
        while (i < s.size() && n < 4)
        {
            while (i < s.size() && (s[i] == '[' || s[i] == ',' || s[i] == ' ')) ++i;
            if (i >= s.size()) break;
            bool neg = false;
            if (s[i] == '-') { neg = true; ++i; }
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            int acc = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') acc = acc * 10 + (s[i++] - '0');
            v[n++] = neg ? -acc : acc;
        }
        if (n != 4) return false;
        a = v[0]; b = v[1]; c = v[2]; d = v[3];
        return true;
    }
};

// ── Material graph ──
class MatAdapter final : public IDocAdapter
{
public:
    MatAdapter(HE::MaterialGraph& g, std::function<void()> onChanged)
        : m_g(g), m_onChanged(std::move(onChanged)) {}

    void enumerate(Kind kind, const ItemFn& fn) const override
    {
        switch (kind)
        {
            case Kind::Node:
                for (const HE::MatGraphNode& n : m_g.nodes) fn(n.id, HE::matNodeToJson(n));
                break;
            case Kind::Link:
                for (const HE::MatGraphLink& l : m_g.links)
                    fn(linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin), linkJson(l));
                break;
            case Kind::Comment:
                for (const HE::MatGraphComment& c : m_g.comments) fn(c.id, HE::matCommentToJson(c));
                break;
            default: break;
        }
    }

    bool upsert(Kind kind, std::int64_t id, const std::string& json) override
    {
        switch (kind)
        {
            case Kind::Node:
            {
                HE::MatGraphNode n;
                if (!HE::matNodeFromJson(json, n)) return false;
                if (HE::MatGraphNode* e = m_g.findNode(n.id)) *e = n;
                else { bumpNextId(m_g.nextId, n.id); m_g.nodes.push_back(n); }
                return true;
            }
            case Kind::Link:
            {
                HE::MatGraphLink l;
                if (!HcAdapter::parseFourInts(json, l.srcNode, l.srcPin, l.dstNode, l.dstPin))
                    return false;
                const auto it = std::find_if(m_g.links.begin(), m_g.links.end(),
                    [&l](const HE::MatGraphLink& o) {
                        return o.srcNode == l.srcNode && o.srcPin == l.srcPin &&
                               o.dstNode == l.dstNode && o.dstPin == l.dstPin;
                    });
                if (it == m_g.links.end()) m_g.links.push_back(l);
                return true;
            }
            case Kind::Comment:
            {
                HE::MatGraphComment c;
                if (!HE::matCommentFromJson(json, c)) return false;
                const auto it = std::find_if(m_g.comments.begin(), m_g.comments.end(),
                    [&c](const HE::MatGraphComment& o) { return o.id == c.id; });
                if (it != m_g.comments.end()) *it = c;
                else { bumpNextId(m_g.nextId, c.id); m_g.comments.push_back(c); }
                return true;
            }
            default: return false;
        }
    }

    bool remove(Kind kind, std::int64_t id) override
    {
        switch (kind)
        {
            case Kind::Node:
                if (!m_g.findNode(static_cast<int>(id))) return false;
                m_g.removeNode(static_cast<int>(id)); // refuses the Output node, by design
                return true;
            case Kind::Link:
            {
                const auto it = std::find_if(m_g.links.begin(), m_g.links.end(),
                    [id](const HE::MatGraphLink& l) {
                        return linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin) == id;
                    });
                if (it == m_g.links.end()) return false;
                m_g.links.erase(it);
                return true;
            }
            case Kind::Comment:
            {
                const auto it = std::find_if(m_g.comments.begin(), m_g.comments.end(),
                    [id](const HE::MatGraphComment& c) { return c.id == id; });
                if (it == m_g.comments.end()) return false;
                m_g.comments.erase(it);
                return true;
            }
            default: return false;
        }
    }

    bool reorder(Kind kind, const std::vector<std::int64_t>& ids) override
    {
        if (kind == Kind::Node)
            return applyOrder(m_g.nodes, ids, [](const HE::MatGraphNode& n) {
                return static_cast<std::int64_t>(n.id); });
        if (kind == Kind::Comment)
            return applyOrder(m_g.comments, ids, [](const HE::MatGraphComment& c) {
                return static_cast<std::int64_t>(c.id); });
        return false;
    }

    void afterApply() override { if (m_onChanged) m_onChanged(); }

private:
    static std::string linkJson(const HE::MatGraphLink& l)
    {
        return "[" + std::to_string(l.srcNode) + "," + std::to_string(l.srcPin) + "," +
                     std::to_string(l.dstNode) + "," + std::to_string(l.dstPin) + "]";
    }
    HE::MaterialGraph&    m_g;
    std::function<void()> m_onChanged;
};

// ── Particle graph ──
class ParticleAdapter final : public IDocAdapter
{
public:
    explicit ParticleAdapter(HE::ParticleGraph& g) : m_g(g) {}

    void enumerate(Kind kind, const ItemFn& fn) const override
    {
        if (kind == Kind::Node)
            for (const HE::ParticleGraphNode& n : m_g.nodes) fn(n.id, HE::particleNodeToJson(n));
        else if (kind == Kind::Link)
            for (const HE::ParticleGraphLink& l : m_g.links)
                fn(linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin),
                   "[" + std::to_string(l.srcNode) + "," + std::to_string(l.srcPin) + "," +
                         std::to_string(l.dstNode) + "," + std::to_string(l.dstPin) + "]");
    }

    bool upsert(Kind kind, std::int64_t, const std::string& json) override
    {
        if (kind == Kind::Node)
        {
            HE::ParticleGraphNode n;
            if (!HE::particleNodeFromJson(json, n)) return false;
            if (HE::ParticleGraphNode* e = m_g.findNode(n.id)) *e = n;
            else { bumpNextId(m_g.nextId, n.id); m_g.nodes.push_back(n); }
            return true;
        }
        if (kind == Kind::Link)
        {
            HE::ParticleGraphLink l;
            if (!HcAdapter::parseFourInts(json, l.srcNode, l.srcPin, l.dstNode, l.dstPin))
                return false;
            const auto it = std::find_if(m_g.links.begin(), m_g.links.end(),
                [&l](const HE::ParticleGraphLink& o) {
                    return o.srcNode == l.srcNode && o.srcPin == l.srcPin &&
                           o.dstNode == l.dstNode && o.dstPin == l.dstPin;
                });
            if (it == m_g.links.end()) m_g.links.push_back(l);
            return true;
        }
        return false;
    }

    bool remove(Kind kind, std::int64_t id) override
    {
        if (kind == Kind::Node)
        {
            if (!m_g.findNode(static_cast<int>(id))) return false;
            m_g.removeNode(static_cast<int>(id)); // refuses EmitterOutput, by design
            return true;
        }
        if (kind == Kind::Link)
        {
            const auto it = std::find_if(m_g.links.begin(), m_g.links.end(),
                [id](const HE::ParticleGraphLink& l) {
                    return linkKey(l.srcNode, l.srcPin, l.dstNode, l.dstPin) == id;
                });
            if (it == m_g.links.end()) return false;
            m_g.links.erase(it);
            return true;
        }
        return false;
    }

    bool reorder(Kind kind, const std::vector<std::int64_t>& ids) override
    {
        if (kind != Kind::Node) return false;
        return applyOrder(m_g.nodes, ids, [](const HE::ParticleGraphNode& n) {
            return static_cast<std::int64_t>(n.id); });
    }

private:
    HE::ParticleGraph& m_g;
};

// ── Animator state machine ──
class AnimatorAdapter final : public IDocAdapter
{
public:
    explicit AnimatorAdapter(HE::AnimatorStateMachineGraph& g) : m_g(g) {}

    void enumerate(Kind kind, const ItemFn& fn) const override
    {
        if (kind == Kind::Node)
            for (const HE::AnimationState& s : m_g.states)
                fn(s.id, HE::animationStateToJson(s));
        else if (kind == Kind::Link)
            for (const HE::AnimationTransition& t : m_g.transitions)
                fn(transitionKey(t.fromState, t.toState, t.paramName),
                   HE::animationTransitionToJson(t));
    }

    bool upsert(Kind kind, std::int64_t id, const std::string& json) override
    {
        if (kind == Kind::Node)
        {
            HE::AnimationState s;
            if (!HE::animationStateFromJson(json, s)) return false;
            const auto it = std::find_if(m_g.states.begin(), m_g.states.end(),
                [&s](const HE::AnimationState& o) { return o.id == s.id; });
            if (it != m_g.states.end()) *it = std::move(s);
            else m_g.states.push_back(std::move(s));
            return true;
        }
        if (kind == Kind::Link)
        {
            HE::AnimationTransition t;
            if (!HE::animationTransitionFromJson(json, t)) return false;
            const auto it = std::find_if(m_g.transitions.begin(), m_g.transitions.end(),
                [id](const HE::AnimationTransition& o) {
                    return transitionKey(o.fromState, o.toState, o.paramName) == id;
                });
            if (it != m_g.transitions.end()) *it = std::move(t);
            else m_g.transitions.push_back(std::move(t));
            return true;
        }
        return false;
    }

    bool remove(Kind kind, std::int64_t id) override
    {
        if (kind == Kind::Node)
        {
            const auto it = std::find_if(m_g.states.begin(), m_g.states.end(),
                [id](const HE::AnimationState& s) { return s.id == id; });
            if (it == m_g.states.end()) return false;
            m_g.states.erase(it);
            return true;
        }
        if (kind == Kind::Link)
        {
            const auto it = std::find_if(m_g.transitions.begin(), m_g.transitions.end(),
                [id](const HE::AnimationTransition& t) {
                    return transitionKey(t.fromState, t.toState, t.paramName) == id;
                });
            if (it == m_g.transitions.end()) return false;
            m_g.transitions.erase(it);
            return true;
        }
        return false;
    }

    bool reorder(Kind kind, const std::vector<std::int64_t>& ids) override
    {
        if (kind != Kind::Node) return false;
        return applyOrder(m_g.states, ids, [](const HE::AnimationState& s) {
            return static_cast<std::int64_t>(s.id); });
    }

private:
    HE::AnimatorStateMachineGraph& m_g;
};

// ── UI element tree ──
class UITreeAdapter final : public IDocAdapter
{
public:
    explicit UITreeAdapter(HE::UIWidgetTree& t) : m_t(t) {}

    void enumerate(Kind kind, const ItemFn& fn) const override
    {
        if (kind != Kind::Element) return;
        for (const auto& e : m_t.elements) fn(e->id, HE::uiElementToJson(*e));
    }

    bool upsert(Kind kind, std::int64_t, const std::string& json) override
    {
        if (kind != Kind::Element) return false;
        std::unique_ptr<HE::UIElement> e = HE::uiElementFromJson(json);
        if (!e) return false;
        const int id = e->id;
        // Replaced whole, not field-by-field: the widget TYPE is part of an
        // element's identity, so a Button that became a Slider cannot be patched
        // in place. Its slot in the vector is kept, because sibling order is the
        // designer's draw order.
        const auto it = std::find_if(m_t.elements.begin(), m_t.elements.end(),
            [id](const std::unique_ptr<HE::UIElement>& o) { return o->id == id; });
        if (it != m_t.elements.end()) *it = std::move(e);
        else { bumpNextId(m_t.nextId, id); m_t.elements.push_back(std::move(e)); }
        return true;
    }

    bool remove(Kind kind, std::int64_t id) override
    {
        if (kind != Kind::Element) return false;
        if (!m_t.find(static_cast<int>(id))) return false;
        // NOT removeSubtree: the sender diffed every element, so it already
        // emitted a removal for each child. Taking the subtree here as well would
        // delete elements that a concurrent reparent had just moved out of it.
        const auto it = std::find_if(m_t.elements.begin(), m_t.elements.end(),
            [id](const std::unique_ptr<HE::UIElement>& o) { return o->id == id; });
        if (it == m_t.elements.end()) return false;
        m_t.elements.erase(it);
        return true;
    }

    // The designer's sibling order IS the draw order, so this is not cosmetic
    // here the way it is for the graphs.
    bool reorder(Kind kind, const std::vector<std::int64_t>& ids) override
    {
        if (kind != Kind::Element) return false;
        return applyOrder(m_t.elements, ids,
                          [](const std::unique_ptr<HE::UIElement>& e) {
                              return static_cast<std::int64_t>(e->id); });
    }

private:
    HE::UIWidgetTree& m_t;
};
} // namespace

std::unique_ptr<IDocAdapter> forHorizonCodeGraph(HC::Graph& g)
{
    return std::make_unique<HcAdapter>(g);
}
std::unique_ptr<IDocAdapter> forMaterialGraph(HE::MaterialGraph& g, std::function<void()> onChanged)
{
    return std::make_unique<MatAdapter>(g, std::move(onChanged));
}
std::unique_ptr<IDocAdapter> forParticleGraph(HE::ParticleGraph& g)
{
    return std::make_unique<ParticleAdapter>(g);
}
std::unique_ptr<IDocAdapter> forAnimatorGraph(HE::AnimatorStateMachineGraph& g)
{
    return std::make_unique<AnimatorAdapter>(g);
}
std::unique_ptr<IDocAdapter> forUIWidgetTree(HE::UIWidgetTree& t)
{
    return std::make_unique<UITreeAdapter>(t);
}

} // namespace CollabDocSync
