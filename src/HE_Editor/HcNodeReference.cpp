#include "HcNodeReference.h"

#include "DocsLibrary.h"
#include "HcEditorUtil.h"
#include "HcNodeDocs.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace HE::Ed::NodeReference
{
namespace
{
	namespace HC   = HorizonCode;
	namespace Docs = HE::Ed::Docs;

	Docs::Run text(std::string s, Docs::Style style = Docs::Style::Body)
	{
		Docs::Run r;
		r.text  = std::move(s);
		r.style = style;
		return r;
	}

	Docs::Block paragraph(std::string s)
	{
		Docs::Block b;
		b.kind = Docs::BlockKind::Paragraph;
		b.runs.push_back(text(std::move(s)));
		return b;
	}

	// The pin rows of one node, in the order the canvas lays them out: exec
	// first, then data, inputs before outputs.
	void appendPins(Docs::Block& block, const std::vector<HC::PinDesc>& execPins,
	                const std::vector<HC::PinDesc>& dataPins,
	                const char* execFallback, bool input, std::string& searchText)
	{
		for (const HC::PinDesc& p : execPins)
		{
			Docs::PinRow row;
			row.name    = (p.name && *p.name) ? p.name : execFallback;
			row.type    = "Exec";
			row.color   = HcEditorUtil::pinTypeColor(HC::PinType::Exec);
			row.isExec  = true;
			row.isInput = input;
			searchText += " " + row.name;
			block.pins.push_back(std::move(row));
		}
		for (const HC::PinDesc& p : dataPins)
		{
			Docs::PinRow row;
			row.name        = (p.name && *p.name) ? p.name : "";
			// Spelled out ("Map<String, Bool>"), not punctuated: a PinRow carries
			// ONE colour, so the reference page cannot lean on the key's colour the
			// way the canvas tooltip does — the words have to carry it alone.
			// typeName/keyTypeName are borrowed and null for every built-in type.
			row.type        = HcEditorUtil::typeLabel(p.type, p.isArray, p.container, p.keyType,
			                                          p.typeName ? p.typeName : "",
			                                          p.keyTypeName ? p.keyTypeName : "");
			row.color       = HcEditorUtil::pinTypeColor(p.type);
			row.isContainer = HC::containerKindOf(p.isArray, p.container) != HC::ContainerKind::None;
			row.isInput     = input;
			searchText += " " + row.name + " " + row.type;
			block.pins.push_back(std::move(row));
		}
	}

	// One section per callable thing. Per FUNCTION rather than per category
	// because that is what the hover tooltip's F1 has to land on: an anchor per
	// category would put the reader sixteen entries above the call they asked
	// about.
	Docs::Section sectionFor(const HC::Node& node, std::string id, std::string title,
	                         std::string category, std::string description,
	                         std::string extra)
	{
		Docs::Section sec;
		sec.id      = std::move(id);
		sec.title   = std::move(title);
		sec.eyebrow = std::move(category);

		std::string search = sec.title + " " + sec.id + " " + description + " " + extra;
		sec.blocks.push_back(paragraph(description));
		if (!extra.empty())
		{
			Docs::Block b;
			b.kind = Docs::BlockKind::Paragraph;
			b.runs.push_back(text(extra, Docs::Style::Italic));
			sec.blocks.push_back(std::move(b));
		}

		// A picture of the node plus its pins. The title and header colour are
		// the ones the canvas uses, so the drawing in the manual and the node in
		// the graph are recognisably the same object.
		const HC::NodeSig sig = HC::signatureOf(node);
		Docs::Block pins;
		pins.kind   = Docs::BlockKind::NodePreview;
		pins.title  = sec.title;
		pins.accent = HcEditorUtil::nodeHeaderColor(node);
		appendPins(pins, sig.execIns,  sig.dataIns,  "In",  true,  search);
		appendPins(pins, sig.execOuts, sig.dataOuts, "Out", false, search);
		// A node with no pins at all is still worth drawing: the shape and the
		// colour of the header are half of what makes it findable.
		sec.blocks.push_back(std::move(pins));

		sec.text = std::move(search);
		return sec;
	}
} // namespace

void install(Docs::Library& lib)
{
	Docs::Page page;
	page.id    = kPageId;
	// No `file`: this page has no counterpart on the website any more, and the
	// reader's "Online" button falls back to the documentation root for it.
	page.title = "HorizonCode Node Reference";
	page.summary =
		"Every node a graph can hold, generated from the engine itself: the "
		"built-in nodes and all of the engine API. What each one does, and the "
		"pins it has.";

	// ── How to read this page ────────────────────────────────────────────────
	// First, because the reference is three hundred entries long and the fastest
	// route through it is not reading it: it is hovering the node.
	{
		Docs::Section sec;
		sec.id      = "how-to-read";
		sec.eyebrow = "Start here";
		sec.title   = "Hovering beats reading";
		sec.text    = "hover tooltip F1 pins exec pure colour how to read this reference";
		sec.blocks.push_back(paragraph(
			"Every entry below is also on the node itself: rest the cursor on a "
			"node in a graph and the same description appears, with its pins. F1 "
			"while that tooltip is up opens this page at that node."));
		{
			Docs::Block fig;
			fig.kind = Docs::BlockKind::Figure;
			fig.src  = "doc-node-tooltip.jpg";
			fig.alt  = "Hovering a node: what it does, and its pins in the colours "
			           "of the wires that fit them";
			sec.blocks.push_back(std::move(fig));
		}
		sec.blocks.push_back(paragraph(
			"A pin's shape says what it carries: a triangle is execution order, a "
			"filled circle a value, a 2x2 grid a container of that value. The "
			"colour is the type, and it is the same colour on the wire — two pins "
			"of one colour connect."));
		sec.blocks.push_back(paragraph(
			"An exec node runs when the chain reaches it. A pure node has no exec "
			"pins at all and is evaluated wherever its output is read, which can "
			"be more than once in a frame or not at all."));
		page.sections.push_back(std::move(sec));
	}

	// ── Built-in nodes ───────────────────────────────────────────────────────
	// The flow, literal and container nodes — the ones the enum knows about.
	for (HC::NodeType t : HC::nodeRegistry())
	{
		const char* name = HC::nodeDisplayName(t);
		if (!name || !*name) continue;

		HC::Node probe;
		probe.type = t;

		std::string id = name;
		id.erase(std::remove(id.begin(), id.end(), ' '), id.end());

		const char* cat = HC::nodeCategory(t);
		const char* doc = HC::nodeTooltip(t);
		page.sections.push_back(sectionFor(
			probe, "node." + id, name,
			(cat && *cat) ? cat : "Nodes",
			(doc && *doc) ? doc : "",
			{}));
	}

	// ── Engine API ───────────────────────────────────────────────────────────
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		HC::Node probe;
		probe.type   = HC::NodeType::EngineCall;
		probe.s      = fn.id;
		probe.hasArg = fn.isExec;
		for (const auto& p : fn.params)  probe.params.push_back({ p.name, p.type, p.isArray });
		for (const auto& r : fn.results) probe.results.push_back({ r.name, r.type, r.isArray });

		// The Target rule is written once, here, for every row that has it —
		// rather than repeated into sixty hand-written descriptions, where it
		// would be right in some of them and stale in the rest.
		std::string extra = fn.isExec ? "Runs when executed."
		                              : "Pure - evaluated whenever an output is used.";
		if (!fn.params.empty() && fn.params[0].selfDefault)
			extra += " Entity left empty means the entity this object sits on, so a "
			         "character calling this on itself wires nothing.";

		page.sections.push_back(sectionFor(
			probe, fn.id, fn.displayName ? fn.displayName : fn.id,
			fn.category ? fn.category : "Engine",
			NodeDocs::engineCall(fn.id),
			std::move(extra)));
	}

	// Group by category, keeping the order within each. The enum's order is the
	// order node types were ADDED to the engine, which interleaves the
	// categories — the sidebar groups a page's sections by their eyebrow, and
	// ungrouped input turns that into "Functions (1) … Functions (1)" four rows
	// apart. Stable, so a category's own entries stay in registry order.
	std::stable_sort(page.sections.begin(), page.sections.end(),
	                 [](const Docs::Section& a, const Docs::Section& b) {
		// The "how to read this" card stays first whatever else moves.
		if ((a.id == "how-to-read") != (b.id == "how-to-read")) return a.id == "how-to-read";
		// Built-in nodes next: they are what a graph is made of, and the engine
		// API is the reference you go looking for something specific in.
		const bool builtinA = a.id.rfind("node.", 0) == 0;
		const bool builtinB = b.id.rfind("node.", 0) == 0;
		if (builtinA != builtinB) return builtinA;
		return a.eyebrow < b.eyebrow;
	});

	lib.appendPage(std::move(page));
}

} // namespace HE::Ed::NodeReference
