// Shared node/link container primitives for the engine's node-graph systems.
//
// Users: HE::MaterialGraph, HE::ParticleGraph, HorizonCode::Graph (nodes+links)
// and HE::UIWidgetTree (findNodeById/appendNode only — it is a parent-id TREE,
// it has no links at all).
//
// HE::AnimatorStateMachineGraph deliberately does NOT use any of this: it stores
// no `nextId`, allocates no link ids, and its transitions reference states BY
// NAME rather than by id (see AnimatorStateMachineGraph.h). There is nothing of
// this file it could share without first migrating that format.
//
// ── WHY connect() IS NOT HERE ────────────────────────────────────────────────
// The three link graphs agree exactly on find / add / remove-with-links, and on
// "a data INPUT pin holds at most one link" — those live here so they cannot
// drift. Their connect() VALIDATION however is genuinely different and must
// stay different:
//   MaterialGraph  — dynamic pin counts (FunctionCall pins come from the loaded
//                    function asset, LandscapeLayerBlend from its layer names),
//                    so whole node types are exempt from the registry check.
//   ParticleGraph  — strict static registry check on both ends.
//   HorizonCode    — unified exec+data pin ranges from signatureOf(), a data
//                    TYPE + isArray equality check, and single-fanout exec
//                    outputs (an exec-out is one "what runs next" pointer).
// A single connect() would need a predicate soup that hides those rules instead
// of sharing them, so each system keeps its own and calls the primitives below.
#pragma once

#include <algorithm>
#include <memory>
#include <vector>

namespace HE::graph
{

// Element access: the graphs store nodes by value, UIWidgetTree by unique_ptr.
template <class T> inline const T* nodeAddr(const T& n) { return &n; }
template <class T> inline       T* nodeAddr(T& n)       { return &n; }
template <class T> inline const T* nodeAddr(const std::unique_ptr<T>& p) { return p.get(); }
template <class T> inline       T* nodeAddr(std::unique_ptr<T>& p)       { return p.get(); }

// Linear scan by id. Graphs are small (tens of nodes) and the vectors are the
// serialization order, so no index is kept — the id→node map would have to be
// rebuilt on every load/undo anyway.
template <class NodeT>
inline const NodeT* findNodeById(const std::vector<NodeT>& nodes, int id)
{
    for (const auto& n : nodes)
        if (const NodeT* p = nodeAddr(n); p && p->id == id) return p;
    return nullptr;
}
template <class NodeT>
inline NodeT* findNodeById(std::vector<NodeT>& nodes, int id)
{
    for (auto& n : nodes)
        if (NodeT* p = nodeAddr(n); p && p->id == id) return p;
    return nullptr;
}
template <class NodeT>
inline const NodeT* findNodeById(const std::vector<std::unique_ptr<NodeT>>& nodes, int id)
{
    for (const auto& n : nodes)
        if (const NodeT* p = nodeAddr(n); p && p->id == id) return p;
    return nullptr;
}
template <class NodeT>
inline NodeT* findNodeById(std::vector<std::unique_ptr<NodeT>>& nodes, int id)
{
    for (auto& n : nodes)
        if (NodeT* p = nodeAddr(n); p && p->id == id) return p;
    return nullptr;
}

// Assign the next free id, append, return it. ids are NEVER reused: nextId only
// moves forward (loaders raise it past everything they read, see
// GraphJson.h/bumpNextId), so a stale id left in an undo buffer, a saved link or
// an editor selection cannot silently re-bind to a different node later.
template <class NodeT>
inline int appendNode(std::vector<NodeT>& nodes, int& nextId, NodeT n)
{
    n.id = nextId++;
    const int id = n.id;
    nodes.push_back(std::move(n));
    return id;
}
template <class NodeT>
inline int appendNode(std::vector<std::unique_ptr<NodeT>>& nodes, int& nextId,
                      std::unique_ptr<NodeT> n)
{
    n->id = nextId++;
    const int id = n->id;
    nodes.push_back(std::move(n));
    return id;
}

// A data/value INPUT pin holds at most ONE link: wiring an occupied input pin
// REPLACES the old wire, it never appends a second one. Every link graph in the
// engine already agreed on this — it lives here so they cannot drift.
template <class LinkT>
inline void disconnectInput(std::vector<LinkT>& links, int dstNode, int dstPin)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const LinkT& l){ return l.dstNode == dstNode && l.dstPin == dstPin; }),
        links.end());
}

// An EXEC output is a single "what runs next" pointer, so wiring an occupied
// exec-out replaces as well. HorizonCode only: Material/Particle have no exec
// pins, and their DATA outputs deliberately fan out to many inputs.
template <class LinkT>
inline void disconnectOutput(std::vector<LinkT>& links, int srcNode, int srcPin)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const LinkT& l){ return l.srcNode == srcNode && l.srcPin == srcPin; }),
        links.end());
}

// The single link feeding an input pin, or null. (Pairs with disconnectInput:
// because an input holds at most one link, "the" feeding link is well defined.)
template <class LinkT>
inline const LinkT* linkToInput(const std::vector<LinkT>& links, int dstNode, int dstPin)
{
    for (const auto& l : links)
        if (l.dstNode == dstNode && l.dstPin == dstPin) return &l;
    return nullptr;
}

// Remove a node and every link attached to it, in either direction — a removed
// node must never leave dangling wires behind. The erase ORDER between the two
// vectors is irrelevant (they are independent), which is why the three systems'
// historically different orderings were observationally identical.
//
// Policy stays at the call site: MaterialGraph/ParticleGraph refuse to remove
// their fixed sink node at all, and HorizonCode additionally drops a deleted
// function's local variables.
template <class NodeT, class LinkT>
inline void removeNodeAndLinks(std::vector<NodeT>& nodes, std::vector<LinkT>& links, int id)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const LinkT& l){ return l.srcNode == id || l.dstNode == id; }),
        links.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const NodeT& n){ return n.id == id; }),
        nodes.end());
}

} // namespace HE::graph
