#pragma once
// RVT 2.0 (Thema 335, step 1): cutting a material graph at its RVT Write / RVT Read nodes.
//
// A material that uses a Runtime Virtual Texture is two programs. What feeds an RVT Write node is painted into the page pool
// once (a PAGE shader per connected Write input: unlit, world position in, one value out), and everything after the matching
// RVT Read node runs live per pixel. matSplitRvt does the cutting; it is pure (graph in, graphs out, no renderer, no
// ContentManager), so the page driver of the next steps and the tests share it.
//
// The page graph of one pin is exactly the nodes that pin depends on, copied (a node that also feeds the live side is in BOTH
// graphs, never moved) plus an unlit Output whose Base Color is the pin's source: the automatic version of what
// AutoLandscapeView::BaseColor/Normal/Surface build by hand. Its hash covers those nodes' type, parameters and wiring, the
// Write node's own settings and the pin, and nothing else: a change AFTER the Read node, cosmetic data (position, comments,
// tooltips) or another pin's chain never reaches it, so only its change repaints the pages. The hash does not look inside a
// Function Call (the called graph is not loaded here): a changed function asset needs the caller to fold the asset's own
// revision in.
#include "MaterialGraph/MaterialGraph.h"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace HE
{
// RVT Write input pins (the registry order, on disk). The first four are the ones a Read node gives back.
enum : int {
    kRvtWriteBaseColorPin = 0,
    kRvtWriteNormalPin    = 1,
    kRvtWriteRoughnessPin = 2,
    kRvtWriteAOPin        = 3,
    kRvtWriteSpecularPin  = 4,   // optional
    kRvtWriteHeightPin    = 5,   // optional
    kRvtWriteMaskPin      = 6,   // optional
    kRvtWritePinCount     = 7,
};
// RVT Read output pins.
enum : int {
    kRvtReadBaseColorPin = 0,
    kRvtReadNormalPin    = 1,
    kRvtReadRoughnessPin = 2,
    kRvtReadAOPin        = 3,
    kRvtReadValidPin     = 4,
};

// The name of the engine-filled parameter that carries volume vector A or B (RvtLookup.h) of the RVT called `rvtName`.
inline std::string matRvtVolumeParamName(const std::string& rvtName, char which)
{
    return "RVT " + rvtName + (which == 'A' ? " Volume A" : " Volume B");
}

struct MatRvtPage
{
    std::string rvtName;          // the Write node's `s`
    int         writeNode = 0;    // id in the SOURCE graph
    int         writePin  = 0;    // kRvtWrite*Pin
    std::string pinName;          // the registry's display name of that pin
    MaterialGraph graph;          // page shader graph: the pin's dependency nodes (source ids) + one unlit Output
    int         outputNode = 0;   // the Output's id in `graph`
    std::vector<int> sourceNodes; // ids (source graph) of the nodes the page depends on, ascending
    uint64_t    hash = 0;         // see the header comment; 0 is never a valid hash
};

struct MatRvtSplit
{
    bool hasRvtNodes = false;     // any Write or Read node at all
    std::vector<MatRvtPage> pages;// ordered by Write node id, then pin
    MaterialGraph live;           // the source minus the Write nodes and the nodes that only fed them
    std::vector<std::string> problems; // why a page was not made, or the graph is ambiguous
    bool ok() const { return problems.empty(); }
};

// Cut `graph`. Without RVT nodes: no pages, `live` is a copy of `graph`.
//   problems: a Write input whose chain contains an RVT Read (the page would read the pool it is painted into; the page is
//   skipped), and a second Write node of an RVT name (ignored, the lowest id wins).
HE_API MatRvtSplit matSplitRvt(const MaterialGraph& graph);

// RVT OFF (the global switch, or a backend that cannot hold a virtual texture): the graph without its RVT nodes. Every RVT Read
// output is replaced by the expression that feeds the matching Write input (same RVT name, lowest Write id), the Valid output by
// a constant 1 (0 without a matching Write), and the Write and Read nodes are dropped. A Write input nobody connected leaves the
// consumer on its own pin default. Node ids are kept, so the generated shader is the one a graph that never had the pair makes.
// Without RVT nodes the graph is returned as it is.
HE_API MaterialGraph matInlineRvt(const MaterialGraph& graph);

// The one global answer to "do RVT nodes page, or does the graph compute directly". The application sets it every tick from its
// switch AND the renderer's capability AND the HE_RVT override (RvtLandscape::wanted); generateFragment and the export cook read
// it, so the editor, the game and the export agree. Default: enabled (a process that never sets it, like hpak_packer, keeps the
// nodes). An atomic read on every call, never cached: the switch changes while the editor runs.
HE_API void matSetRvtEnabled(bool enabled);
HE_API bool matRvtEnabled();

// Ids of every node that is downstream of an RVT Read (reachable by following links from a Read's outputs forward), Read
// nodes included. A page may never depend on one of them.
HE_API std::set<int> matRvtDownstreamOfRead(const MaterialGraph& graph);

// Ids of the nodes `pinSource` depends on (itself included), following every input link. Ascending.
HE_API std::vector<int> matGraphDependencies(const MaterialGraph& graph, int pinSource);
} // namespace HE
