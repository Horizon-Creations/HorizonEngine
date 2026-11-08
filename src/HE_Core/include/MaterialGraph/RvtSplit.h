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
#include <array>
#include <cstdint>
#include <map>
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

// ---------------------------------------------------------------------------------------------------------------------------
// RVT 2.0 step 3: what makes a page stale (Thema 335). The engine derives it from the page subgraph, nothing is declared:
//   (a) the node parameters, material parameters, static switches and assets (textures, functions) BEFORE the Write node,
//   (b) the ground the subgraph reads (terrain height / normal / painted weights), so a sculpt stroke repaints the pages only
//       when a page reads the height, and a paint stroke only when it reads the weights,
//   (c) the entity's parameter overrides (they are part of the parameter values below).
// Whatever sits AFTER the matching Read node (snow height, sand, weather, tracks, time of day) is not in a page subgraph and
// so can never be in a page hash: changing it paints no page.
enum : uint32_t {
    kRvtReadsHeight  = 1u << 0,   // the ground's height: World Position (its Y), and everything cut from it
    kRvtReadsNormal  = 1u << 1,   // the ground's normal: Normal WS, the normal-map nodes (screen-space frame of the position)
    kRvtReadsWeights = 1u << 2,   // the painted layer weights: Landscape Layer Blend, Landscape Group Pick
    kRvtReadsGround  = kRvtReadsHeight | kRvtReadsNormal | kRvtReadsWeights,
};
// What a terrain edit changes, in the same flag space: a sculpt stroke changes height and (so) normal, a paint stroke the weights.
inline constexpr uint32_t kRvtEditSculpt = kRvtReadsHeight | kRvtReadsNormal;
inline constexpr uint32_t kRvtEditPaint  = kRvtReadsWeights;

// The values the graph's parameters and switches take for one landscape, as the material system resolves them: the material
// instance's own, then the entity's overrides on top. A name that is not in the map takes the node's own default.
struct MatRvtEnv
{
    std::map<std::string, std::array<float, 4>> params;   // Param* node name -> effective value (x, y, z, w)
    std::map<std::string, bool>                 switches; // Static Switch name -> effective on/off
    // Revision of an asset a page subgraph names (a texture path, a function path): anything that changes when the asset's
    // content does. A name missing from the map counts as revision 0.
    std::map<std::string, uint64_t>             assetRevision;
    // What a Function Call reads of the ground, by function path (the caller loaded the function and ran
    // matRvtPageDeps over it); a call that is not listed is assumed to read everything.
    std::map<std::string, uint32_t>             functionReads;
};

struct MatRvtPageDeps
{
    uint32_t reads = 0;                          // kRvtReads* the page subgraph needs from the ground
    std::vector<int> nodes;                      // the dependency nodes that are LIVE for these switch values, ascending
    std::vector<std::string> params;             // Param* names the page reads, sorted
    std::vector<std::string> switches;           // Static Switch names the page reads, sorted
    std::vector<std::string> assets;             // texture and function paths the page reads, sorted
    // Nodes whose value changes per frame (Time, Camera, Weather, Panner ...): the page is painted once, so they are frozen
    // at the moment of painting and never repaint it. Listed (node type names, sorted) so the editor can warn about them.
    std::vector<std::string> frozen;
};

// What one node type reads of the ground (kRvtReads*), without looking at its inputs. A Function Call is not decided here.
HE_API uint32_t matRvtNodeGroundReads(MatNodeType type);

// The live dependency set of one page: like page.sourceNodes, but a Static Switch follows only its taken input, so a parameter
// or texture behind the untaken side is not a dependency of this permutation.
HE_API MatRvtPageDeps matRvtPageDeps(const MaterialGraph& source, const MatRvtPage& page, const MatRvtEnv& env);

// The hash of one page's inputs: page.hash's content for the live nodes plus the parameter, switch and asset values of `env`
// that those nodes read. Equal hash = the page would be painted identically. A parameter of a node that is not a page
// dependency, in particular anything after the Read node, is never mixed in. 0 is never a valid hash.
HE_API uint64_t matRvtPageStateHash(const MaterialGraph& source, const MatRvtPage& page, const MatRvtEnv& env);

// One material's answer to "what do my pages depend on, right now": the split (computed by the caller once per graph edit) under
// the current parameter values. `pages` is parallel to split.pages; `state` folds all of them, and is what a driver compares from
// tick to tick: unchanged = no page is stale, whatever else moved. `reads` is the union of what the pages read of the ground.
struct MatRvtMaterialState
{
    std::vector<uint64_t>       pageHashes;
    std::vector<MatRvtPageDeps> pageDeps;
    uint64_t                    state = 0;
    uint32_t                    reads = 0;
};
HE_API MatRvtMaterialState matRvtMaterialState(const MaterialGraph& source, const MatRvtSplit& split, const MatRvtEnv& env);

// Whether a terrain edit (kRvtEditSculpt / kRvtEditPaint, or both) can change a page that reads `reads`.
inline bool matRvtEditReaches(uint32_t reads, uint32_t editKinds) { return (reads & editKinds) != 0; }
} // namespace HE
