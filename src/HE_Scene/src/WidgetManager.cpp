#include <HorizonScene/WidgetManager.h>
#include <HorizonScene/UISystem.h>   // sortKey — one painter-order rule for both UI paths
#include <HorizonCode/HcCompiledLoader.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Renderer/UIFont.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <cmath>      // std::abs — the border stamp compares rects
#include <functional> // std::function — the tab order walks the tree recursively

namespace
{
	// Sort key inside one widget: layer (major) + nesting depth (minor), the
	// same rule (and the same formula, UISystem::sortKey) the entity UI path
	// uses — children draw over their parents. Only the depth walk differs:
	// a widget tree nests by parentId, the entity path by HierarchyComponent.
	int elementSortKey(const HE::UIWidgetTree& tree, const HE::UIElement& e)
	{
		int depth = 0;
		for (const HE::UIElement* c = &e; c->parentId != 0 && depth < 255; ++depth)
		{
			const HE::UIElement* p = tree.find(c->parentId);
			if (!p) break;
			c = p;
		}
		return UISystem::sortKey(e.layer, depth);
	}
}

WidgetManager::Instance* WidgetManager::find(int id)
{
	for (auto& w : m_instances) if (w.id == id) return &w;
	return nullptr;
}
const WidgetManager::Instance* WidgetManager::find(int id) const
{
	for (const auto& w : m_instances) if (w.id == id) return &w;
	return nullptr;
}

WidgetManager::Instance* WidgetManager::findByScript(HorizonCode::InstanceId scriptId)
{
	for (auto& w : m_instances) if (w.scriptId == scriptId) return &w;
	return nullptr;
}

WidgetManager::ScriptTarget WidgetManager::scriptTargetFor(const Instance& w, int elemId) const
{
	// Innermost first: nested embeds have larger offsets, and the last one whose
	// range contains this id is the deepest widget that owns it.
	for (auto it = w.embeds.rbegin(); it != w.embeds.rend(); ++it)
		if (elemId > it->idOffset && elemId <= it->idMax)
			return { it->scriptId, elemId - it->idOffset };
	return { w.scriptId, elemId };
}

WidgetManager::Instance* WidgetManager::resolveScriptOwner(HorizonCode::InstanceId scriptId,
                                                           int& idOffset)
{
	idOffset = 0;
	for (auto& w : m_instances)
	{
		if (w.scriptId == scriptId) return &w;
		for (const Instance::Embed& em : w.embeds)
			if (em.scriptId == scriptId) { idOffset = em.idOffset; return &w; }
	}
	return nullptr;
}

// The element called `name` INSIDE what `scriptId` stands for — the widget
// itself, or just the part of the host tree an embedded component occupies.
//
// The scoping is the whole point. A grafted component's elements live in the
// host's tree with their ids shifted, but they keep their NAMES, and a page
// that embeds two cards both containing a "Title" is the normal case rather
// than a strange one. Searching the whole tree would hand the first "Title" to
// everyone who asked, which is the id-offset trap wearing a different hat.
const HE::UIElement* WidgetManager::elementOfScript(HorizonCode::InstanceId scriptId,
                                                    const std::string& name)
{
	if (name.empty()) return nullptr;
	int off = 0;
	const Instance* w = resolveScriptOwner(scriptId, off);
	if (!w) return nullptr;
	// The embed's slice of the host tree, or the whole thing for the widget's
	// own graph. idMax is inclusive; 0 means "no upper bound".
	int lo = 0, hi = 0;
	for (const Instance::Embed& em : w->embeds)
		if (em.scriptId == scriptId) { lo = em.idOffset; hi = em.idMax; break; }
	for (const auto& ep : w->tree.elements)
	{
		if (!ep || ep->name != name) continue;
		if (ep->id <= lo) continue;
		if (hi > 0 && ep->id > hi) continue;
		return ep.get();
	}
	return nullptr;
}

// Host bindings shared by every widget: the central runtime owns the graph +
// variable state and hands back the InstanceId, so one binding set serves all
// widgets. Property access + show/hide resolve the widget from the id and act on
// its live tree / visibility. Variables are handled by the runtime, not here.
HorizonCode::HostBindings WidgetManager::makeBindings()
{
	HorizonCode::HostBindings b;
	b.getProperty = [this](HorizonCode::InstanceId id, int elem, const std::string& prop) -> HorizonCode::Value
	{
		// An embedded widget's graph knows its elements by the ids they had in
		// its own asset; the offset turns those into ids in the host tree.
		int off = 0;
		Instance* w = resolveScriptOwner(id, off);
		const HE::UIElement* e = w ? w->tree.find(elem + off) : nullptr;
		// getPropAny/setPropAny: base properties (Visible, Hit Testable,
		// Position, Size, Layer, Hover Cursor, Material, Font) plus the
		// type-specific ones — every property is both gettable and settable.
		return e ? HE::uiPropToHcValue(e->getPropAny(prop)) : HorizonCode::Value{};
	};
	b.setProperty = [this](HorizonCode::InstanceId id, int elem, const std::string& prop, const HorizonCode::Value& v)
	{
		int off = 0;
		Instance* w = resolveScriptOwner(id, off);
		HE::UIElement* e = w ? w->tree.find(elem + off) : nullptr;
		if (!e) return;
		e->setPropAny(prop, HE::uiHcValueToProp(v, e->getPropAny(prop).type));
		// Every property here is something the element draws with — text, colour,
		// position, visibility. This is the single busiest route by which a
		// script changes the picture, so it is the one that matters most for the
		// event-driven loop (see consumeVisualDirty).
		m_visualDirty = true;
		// Asset-path properties change what the element draws with — re-resolve
		// immediately so the set is visible this frame, not on the next reload.
		if (prop == "Material" || prop == "Font")
			refreshElementAssets(*w, *e);
	};
	// The same two through a reference, addressing the element by NAME. Same
	// resolution as above — the target's own script owner and offset — so a page
	// reaching into a component it embeds gets that component's elements, and
	// its local numbering never leaks out. A name that is not there reads as
	// nothing and writes nothing: a graph pointed at the wrong widget must not
	// be able to invent an element in it.
	b.getPropertyOn = [this](HorizonCode::InstanceId target, const std::string& elemName,
	                         const std::string& prop) -> HorizonCode::Value
	{
		const HE::UIElement* e = elementOfScript(target, elemName);
		return e ? HE::uiPropToHcValue(e->getPropAny(prop)) : HorizonCode::Value{};
	};
	b.setPropertyOn = [this](HorizonCode::InstanceId target, const std::string& elemName,
	                         const std::string& prop, const HorizonCode::Value& v)
	{
		int off = 0;
		Instance* w = resolveScriptOwner(target, off);
		HE::UIElement* e = const_cast<HE::UIElement*>(elementOfScript(target, elemName));
		if (!e || !w) return;
		e->setPropAny(prop, HE::uiHcValueToProp(v, e->getPropAny(prop).type));
		m_visualDirty = true;
		if (prop == "Material" || prop == "Font") refreshElementAssets(*w, *e);
	};
	// "Self" for an EMBEDDED widget is that widget, not the whole page it sits
	// on: showing itself shows its WidgetRef element, and nothing around it.
	auto setSelfVisible = [this](HorizonCode::InstanceId id, bool vis)
	{
		int off = 0;
		Instance* w = resolveScriptOwner(id, off);
		if (!w) return;
		m_visualDirty = true;
		// Hiding a whole widget from its own graph is hiding it, so it lets go
		// of any layer it was holding — see hideWidget for what the alternative
		// costs. An EMBEDDED widget hides only its own slot and holds nothing.
		if (off == 0 && !vis) releaseGrabsOf(w->id);
		if (off == 0) { w->visible = vis; return; }
		for (const Instance::Embed& em : w->embeds)
			if (em.scriptId == id)
				if (HE::UIElement* ref = w->tree.find(em.rootElem)) ref->visible = vis;
	};
	b.showSelf = [setSelfVisible](HorizonCode::InstanceId id){ setSelfVisible(id, true); };
	b.hideSelf = [setSelfVisible](HorizonCode::InstanceId id){ setSelfVisible(id, false); };
	return b;
}

void WidgetManager::refreshElementAssets(Instance& w, HE::UIElement& e)
{
	if (!m_content) return;
	// Material path → UUID.
	if (e.material.empty())
		w.materials.erase(e.id);
	else
	{
		const HE::UUID mid = m_content->loadAsset(e.material);
		if (mid == HE::UUID{}) w.materials.erase(e.id);
		else                   w.materials[e.id] = mid;
	}
	// Texture path → UUID, straight on the element: unlike the material (which
	// the backends look up per draw) the picture is part of what the element
	// emits, so render() needs it without a signature that knows about assets.
	e.textureAssetId = e.texture.empty() ? HE::UUID{} : m_content->loadAsset(e.texture);
	// …and its source size, which is what turns 9-slice margins in pixels into
	// the UVs the quads need. Unknown (not loaded) leaves 0 and the image draws
	// stretched rather than sliced wrongly.
	e.textureW = e.textureH = 0;
	if (e.textureAssetId != HE::UUID{})
		if (const TextureAsset* ta = m_content->getTexture(e.textureAssetId))
		{ e.textureW = ta->width; e.textureH = ta->height; }

	// Font path → baked atlas key (0 = shared default font).
	e.fontAtlasKey = 0;
	if (!e.font.empty())
	{
		const HE::UUID fid = m_content->loadAsset(e.font);
		if (const FontAsset* fa = fid == HE::UUID{} ? nullptr : m_content->getFont(fid);
		    fa && !fa->fontData.empty())
		{
			const float bakePx = fa->size > 0 ? (float)fa->size : 48.0f;
			e.fontAtlasKey = HE::UIFontCache::keyFor(fid.hi ^ fid.lo, fa->fontData, bakePx);
		}
	}
}

void WidgetManager::embedWidgetRefs(Instance& w, ContentManager& content,
                                    const std::vector<std::string>& rootChain)
{
	// A widget that embeds itself, directly or around a circle, would expand
	// forever. Both guards are cheap and both are needed: the chain catches the
	// circle, the depth catches an honest but absurd nesting.
	constexpr size_t kMaxDepth = 8;

	// ── Every reference carries its OWN ancestry ─────────────────────────────
	// This used to be one shared chain, pushed before recursing and popped
	// after. It read like a depth-first walk and was not one: the recursive call
	// re-scanned the WHOLE tree, so it also met the siblings of the reference it
	// had just expanded — with that reference's path still on the chain.
	//
	// Two cards of the same component on one page was therefore enough: the
	// second one found the first's path in a chain it never descended through
	// and was refused as a circle, with its content silently dropped. A chain
	// that describes "what is currently open" answers a different question from
	// "what am I inside of", and only the second one is a circle.
	struct Pending { int refId; std::vector<std::string> chain; };
	std::vector<Pending> queue;
	for (const auto& ep : w.tree.elements)
		if (ep && ep->type() == HE::UIWidgetType::WidgetRef)
			queue.push_back({ ep->id, rootChain });

	for (size_t qi = 0; qi < queue.size(); ++qi)
	{
		const int refId = queue[qi].refId;
		// A COPY, not a reference into `queue`: the loop below pushes into it,
		// and a vector that grows moves what it holds — a reference taken here
		// would be read after the move.
		const std::vector<std::string> chain = queue[qi].chain;
		auto* ref = dynamic_cast<HE::UIWidgetRef*>(w.tree.find(refId));
		if (!ref || ref->embedded || ref->widgetPath.empty()) continue;

		if (chain.size() > kMaxDepth)
		{
			HE_LOG_ERROR(Widget, "Widget '%s' is nested more than %zu deep — that "
			                     "reference is left empty", ref->widgetPath.c_str(), kMaxDepth);
			continue;
		}
		if (std::find(chain.begin(), chain.end(), ref->widgetPath) != chain.end())
		{
			HE_LOG_ERROR(Widget, "Widget '%s' embeds itself (directly or in a circle) — "
			                     "that reference is left empty", ref->widgetPath.c_str());
			continue;
		}

		const HE::UUID id = content.loadAsset(ref->widgetPath);
		const UIWidgetAsset* asset = id == HE::UUID{} ? nullptr : content.getWidget(id);
		if (!asset)
		{
			HE_LOG_WARN(Widget, "WidgetRef: no widget asset at '%s'", ref->widgetPath.c_str());
			continue;
		}
		HE::UIWidgetTree sub;
		if (!HE::uiWidgetTreeFromJson(asset->treeJson, sub))
		{
			HE_LOG_ERROR(Widget, "WidgetRef: unreadable tree in '%s'", ref->widgetPath.c_str());
			continue;
		}

		// What this copy was told, written in BEFORE the renumbering below: the
		// declarations address elements by the ids they carry in their own
		// asset, and one line further down those ids mean something else.
		if (!ref->paramValues.empty() &&
		    HE::uiApplyWidgetParams(sub, ref->paramValues) == 0)
			// Every value dropped means the component no longer declares any of
			// them — it was rebuilt, or this ref was pointed at a different
			// widget. Silence here looks exactly like a component that ignores
			// what it is told, which is the hardest kind of nothing to explain.
			HE_LOG_WARN(Widget, "WidgetRef '%s': none of its %d set parameters are "
			                    "declared by that widget any more — it will show its "
			                    "own defaults", ref->widgetPath.c_str(),
			            (int)ref->paramValues.size());

		// Renumber into this tree. The offset is the host's nextId - 1, so a
		// local id 1 becomes the host's next free id; two copies of the same
		// widget therefore never share an element id.
		const int offset = w.tree.nextId - 1;
		Instance::Embed em;
		em.rootElem = refId;
		em.idOffset = offset;
		em.idMax    = offset;
		for (auto& sp : sub.elements)
		{
			if (!sp) continue;
			sp->id += offset;
			// A root of the embedded asset hangs under the ref element; anything
			// else keeps its parent, shifted.
			sp->parentId = sp->parentId == 0 ? refId : sp->parentId + offset;
			em.idMax = std::max(em.idMax, sp->id);
			w.tree.elements.push_back(std::move(sp));
		}
		w.tree.nextId = em.idMax + 1;

		// Its logic runs as its own instance, exactly like a top-level widget's
		// — same compiled-first lookup, same class identity by asset path.
		HorizonCode::Graph graph;
		if (!asset->graphJson.empty() && !HorizonCode::fromJson(asset->graphJson, graph))
			HE_LOG_ERROR(Widget, "WidgetRef '%s' has an unparsable graph — it will render "
			                     "but have no logic", ref->widgetPath.c_str());
		const HorizonCode::ClassIdentity cls{ ref->widgetPath, "Object" };
		if (auto compiled = HorizonCode::compiledClasses().create(ref->widgetPath))
			em.scriptId = rt().addCompiled(std::move(compiled), makeBindings(), cls);
		else
			em.scriptId = rt().add(std::move(graph), makeBindings(), cls);

		// Its animations come with it, unchanged. Their tracks name elements by
		// the LOCAL ids of the asset they were authored in, and em.idOffset is
		// what turns those into this tree's — the same translation its graph's
		// element references get, and for the same reason.
		em.animations = sub.animations;

		w.embeds.push_back(em);
		ref->embedded = true;
		// What its elements measure in, and by which rule that meets this slot.
		// Without it a widget authored for 1920x1080 dropped into a 400x300 slot
		// would keep its absolute offsets and hang out of its own frame.
		ref->contentW    = sub.canvasWidth;
		ref->contentH    = sub.canvasHeight;
		ref->contentMode = sub.scaleMode;

		// …and the widget it just brought in may embed further widgets. ONLY the
		// elements this graft added are queued, each with THIS reference's
		// ancestry plus this reference — which is what makes the chain describe
		// where a reference sits rather than what happens to be in flight.
		{
			std::vector<std::string> childChain = chain;
			childChain.push_back(ref->widgetPath);
			for (const auto& ep : w.tree.elements)
				if (ep && ep->id > offset && ep->id <= em.idMax &&
				    ep->type() == HE::UIWidgetType::WidgetRef)
					queue.push_back({ ep->id, childChain });
		}
	}
}

int WidgetManager::createWidget(ContentManager& content, const std::string& assetPath)
{
	m_content = &content; // kept for runtime Material/Font re-resolution
	const HE::UUID assetId = content.loadAsset(assetPath);
	const UIWidgetAsset* asset = content.getWidget(assetId);
	if (!asset)
	{
		HE_LOG_WARN(Widget, "%s",
			("WidgetManager: widget asset not found: " + assetPath).c_str());
		return 0;
	}

	Instance w;
	w.assetPath = assetPath;
	if (!HE::uiWidgetTreeFromJson(asset->treeJson, w.tree))
	{
		HE_LOG_ERROR(Widget, "%s",
			("WidgetManager: invalid widget tree JSON in " + assetPath).c_str());
		return 0;
	}
	HorizonCode::Graph graph;
	if (!asset->graphJson.empty() && !HorizonCode::fromJson(asset->graphJson, graph))
		// Widget shows up but is completely inert — the exact symptom of a broken
		// graph, previously with nothing in the log to say so.
		HE_LOG_ERROR(Widget, "Widget '%s' has an unparsable HorizonCode graph — it will "
		                     "render but have no logic", assetPath.c_str());

	// Graft in every embedded widget FIRST: what they bring is part of this
	// tree from here on, so material/font resolution below covers it too.
	{
		embedWidgetRefs(w, content, { assetPath });
	}

	// A widget asset may name its OWN theme, overriding the application's for
	// this instance and everything grafted into it. Loaded before the theme is
	// applied, obviously, and left null when the asset names none or the theme
	// cannot be read — an unreadable override falls back to the application's
	// rather than to nothing, which would be a widget drawn in white.
	if (!w.tree.themeAsset.empty())
	{
		const HE::UUID themeId = content.loadAsset(w.tree.themeAsset);
		const ThemeAsset* ta = themeId == HE::UUID{} ? nullptr : content.getTheme(themeId);
		if (ta)
		{
			auto t = std::make_shared<HE::UITheme>();
			// Parsed from a copy of the JSON, not from the asset's own string: the
			// getter points into the manager's dense vector and any load below
			// would move it.
			const std::string json = ta->json;
			if (HE::uiThemeFromJson(json, *t)) w.theme = std::move(t);
		}
		if (!w.theme)
			HE_LOG_WARN(UI, "Widget '%s' names theme '%s', which could not be read",
			            assetPath.c_str(), w.tree.themeAsset.c_str());
	}

	// Whatever this widget bound to a theme role takes the theme's colour now,
	// before anything looks at it. Assignment, not lookup-at-draw: from here on
	// the runtime, the designer and the thumbnails all read plain fields.
	HE::uiApplyTheme(w.tree, themeFor(w), themeMode());

	// Resolve per-element material references once (paths → UUIDs) and bake each
	// element's Font asset → a stable atlas key its text emits with (0 = the
	// shared default font). Exactly the resolution refreshElementAssets does for
	// one element when a script changes Material/Font at runtime, so it is done
	// there and only there.
	for (const auto& e : w.tree.elements)
		refreshElementAssets(w, *e);

	// Register the widget's logic with the central runtime, which takes the graph
	// and seeds the private variable store from its declared defaults. The
	// runtime instance id doubles as the widget's public handle (widget id ==
	// scriptId), so a widget is a first-class Ref object. Packaged builds may
	// carry this widget's script compiled to native C++ (keyed by the same asset
	// path); a table miss runs the graph interpreted, exactly as before.
	// A widget's class key is its asset path, like any other class; it derives
	// from nothing (a widget lives outside the entity world), so it stays Object.
	const HorizonCode::ClassIdentity widgetCls{ assetPath, "Object" };
	if (auto compiled = HorizonCode::compiledClasses().create(assetPath))
		w.scriptId = rt().addCompiled(std::move(compiled), makeBindings(), widgetCls);
	else
		w.scriptId = rt().add(std::move(graph), makeBindings(), widgetCls);
	w.id = (int)w.scriptId;
	m_instances.push_back(std::move(w));

	// Fire Construct AFTER the widget is in m_instances, so host callbacks can
	// resolve it by scriptId during construction.
	//
	// Everything still needed is COPIED OUT FIRST, because Construct runs user
	// graph code and that code may create or destroy widgets — and m_instances is
	// a plain vector, so a single Create Widget inside a Construct reallocates it
	// and any reference into it points at freed memory. A page widget that builds
	// its own sub-widget is the ordinary case here, not an exotic one, which is
	// why this was reachable without anything unusual happening.
	const int                     widgetId = m_instances.back().id;
	const HorizonCode::InstanceId scriptId = m_instances.back().scriptId;
	std::vector<HorizonCode::InstanceId> embedScripts;
	embedScripts.reserve(m_instances.back().embeds.size());
	for (const Instance::Embed& em : m_instances.back().embeds)
		embedScripts.push_back(em.scriptId);

	HE_LOG_INFO(Widget, "Created widget '%s' (id %d, %zu element(s), %s logic)",
	            assetPath.c_str(), widgetId, m_instances.back().tree.elements.size(),
	            graph.nodes.empty() ? "compiled/no" : "interpreted");
	rt().fireConstruct(scriptId);
	// Embedded widgets construct too, innermost last — an embed may only be
	// spoken to once the widget holding it has run its own Construct.
	for (const HorizonCode::InstanceId embed : embedScripts)
		rt().fireConstruct(embed);
	// A new widget is a new picture, and in an app nothing else asks for one:
	// the frame is only drawn when something says it changed (A2).
	m_visualDirty = true;
	return widgetId;
}

void WidgetManager::setTheme(const HE::UITheme& theme)
{
    m_theme = theme;
    // themeFor, not m_theme: an instance whose asset named its own theme keeps
    // it. The MODE is the application's either way — light and dark is a
    // decision about the machine, not about one widget.
    for (Instance& w : m_instances) HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
    m_visualDirty = true;
}

void WidgetManager::setThemePreference(HE::UIThemePreference pref)
{
    if (pref >= HE::UIThemePreference::COUNT) return;
    const HE::UIThemeMode before = themeMode();
    m_themePref = pref;
    // Only redraw when the RESULT changed: asking for Dark on a dark desktop
    // that was already following the system changes nothing on screen, and an
    // event-driven application should not wake up for it.
    if (themeMode() == before) return;
    // themeFor, not m_theme: an instance whose asset named its own theme keeps
    // it. The MODE is the application's either way — light and dark is a
    // decision about the machine, not about one widget.
    for (Instance& w : m_instances) HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
    m_visualDirty = true;
}

void WidgetManager::setSystemThemeMode(HE::UIThemeMode mode)
{
    if (mode >= HE::UIThemeMode::COUNT || mode == m_systemMode) return;
    const HE::UIThemeMode before = themeMode();
    m_systemMode = mode;
    if (themeMode() == before) return;   // the preference overrides it anyway
    // themeFor, not m_theme: an instance whose asset named its own theme keeps
    // it. The MODE is the application's either way — light and dark is a
    // decision about the machine, not about one widget.
    for (Instance& w : m_instances) HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
    m_visualDirty = true;
}

HorizonCode::InstanceId WidgetManager::addChild(ContentManager& content, int widgetId,
                                                const std::string& parentName,
                                                const std::string& assetPath)
{
	m_content = &content;
	Instance* w = find(widgetId);
	if (!w)
	{
		HE_LOG_WARN(Widget, "addChild: no widget with id %d", widgetId);
		return 0;
	}
	// By NAME, not by element id: the name is what the designer shows and what
	// the author can type into a graph. An id is a number nobody can look up.
	const HE::UIElement* parent = nullptr;
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == parentName) { parent = ep.get(); break; }
	if (!parent)
	{
		HE_LOG_WARN(Widget, "addChild: widget %d has no element named '%s'",
		            widgetId, parentName.c_str());
		return 0;
	}
	const HorizonCode::InstanceId child =
		graftChildRef(*w, content, parent->id, assetPath, /*rowIndex=*/-1);
	if (child != 0)
		HE_LOG_DEBUG(Widget, "addChild: '%s' under '%s' of widget %d (instance %llu)",
		             assetPath.c_str(), parentName.c_str(), widgetId,
		             static_cast<unsigned long long>(child));
	return child;
}

// The graft itself, with the parent already found. Split out of addChild because
// a list realizes its rows with exactly this and nothing else — if the two had
// their own copies, a row grafted by a list and a row grafted by a script would
// eventually differ in how they are themed, sized or constructed.
HorizonCode::InstanceId WidgetManager::graftChildRef(Instance& w, ContentManager& content,
                                                     int parentElem,
                                                     const std::string& assetPath,
                                                     int rowIndex)
{
	if (assetPath.empty())
	{
		HE_LOG_WARN(Widget, "%s", "addChild: no widget asset given");
		return 0;
	}

	// The new child is a WidgetRef under that parent, and then the ordinary graft
	// path does the rest — the same code an authored WidgetRef goes through, so
	// a runtime row and an authored one are the same thing by construction.
	auto ref = std::make_unique<HE::UIWidgetRef>();
	const int refId = w.tree.nextId++;
	ref->id       = refId;
	ref->parentId = parentElem;
	ref->name     = rowIndex >= 0 ? "Row" : "Child";
	ref->widgetPath = assetPath;
	// A list row is placed by the ITEM it shows, not by stacking (listSlotRect).
	ref->rowIndex = rowIndex;
	ref->rowBound = -1;      // never bound yet, whatever index it starts on
	// Inside a layout box only the size along the axis is read, and it is the
	// row's own height that should decide it. Filled in from the embedded
	// asset's canvas below, once it is known; until then the type's default.
	w.tree.elements.push_back(std::move(ref));

	const std::size_t embedsBefore = w.embeds.size();
	{
		// Seeded with THIS widget's own asset, exactly as createWidget seeds it:
		// grafting a widget into itself is the circle the guard exists for, and
		// an empty chain would let it through and expand until the depth cap.
		embedWidgetRefs(w, content, { w.assetPath });
	}
	if (w.embeds.size() == embedsBefore)
	{
		// The graft refused (missing asset, unreadable tree, a circle). Leave no
		// empty slot behind: a row that is not there must not take up space.
		w.tree.elements.erase(
			std::remove_if(w.tree.elements.begin(), w.tree.elements.end(),
				[refId](const std::unique_ptr<HE::UIElement>& e){ return e && e->id == refId; }),
			w.tree.elements.end());
		return 0;
	}

	// The embed the graft just made for THIS ref (a row may itself embed more,
	// which appended further entries — the one with rootElem == refId is ours).
	HorizonCode::InstanceId child = 0;
	for (std::size_t i = embedsBefore; i < w.embeds.size(); ++i)
		if (w.embeds[i].rootElem == refId) { child = w.embeds[i].scriptId; break; }
	if (child == 0) child = w.embeds[embedsBefore].scriptId;

	if (auto* placed = dynamic_cast<HE::UIWidgetRef*>(w.tree.find(refId));
	    placed && placed->contentW > 0.0f && placed->contentH > 0.0f)
	{
		// The slot is as big as the row was authored — a row drawn for 400x48
		// takes 48 units in a vertical box rather than the ref type's 200. A LIST
		// row's height is the list's Row Height instead (listSlotRect), so only
		// its width is taken from the asset there.
		placed->sizeX = placed->contentW;
		if (rowIndex < 0) placed->sizeY = placed->contentH;
	}

	// The theme, then materials and fonts, for what just arrived — a row grafted
	// in at run time is themed like one that was there from the start, and by
	// the HOST widget's theme: what is grafted in becomes part of this instance,
	// so it wears what the instance wears.
	HE::uiApplyTheme(w.tree, themeFor(w), themeMode());
	for (const auto& e : w.tree.elements)
		if (e && e->id > 0) refreshElementAssets(w, *e);

	// Construct last, like createWidget does: the row is fully in the tree
	// before its own logic can look at it.
	for (std::size_t i = embedsBefore; i < w.embeds.size(); ++i)
		rt().fireConstruct(w.embeds[i].scriptId);
	m_visualDirty = true;
	return child;
}

HorizonCode::InstanceId WidgetManager::instanceOfChild(const Instance& w, int refElemId) const
{
	for (const Instance::Embed& em : w.embeds)
		if (em.rootElem == refElemId) return em.scriptId;
	return 0;
}

HorizonCode::InstanceId WidgetManager::childInstance(int widgetId,
                                                     const std::string& elementName) const
{
	const Instance* w = find(widgetId);
	if (!w || elementName.empty()) return 0;
	// The WidgetRef element that carries the name — the slot, not what is in it.
	// Its instance is the component's own script, which is what a reference to a
	// component MEANS: the same thing a list row hands out (listRow), and the
	// same thing Call Function and Get (Ref) expect on their Target.
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == elementName && ep->type() == HE::UIWidgetType::WidgetRef)
			return instanceOfChild(*w, ep->id);
	return 0;
}

bool WidgetManager::removeChild(int widgetId, HorizonCode::InstanceId child)
{
	Instance* w = find(widgetId);
	if (!w || child == 0) return false;
	const auto it = std::find_if(w->embeds.begin(), w->embeds.end(),
		[child](const Instance::Embed& em){ return em.scriptId == child; });
	if (it == w->embeds.end())
	{
		HE_LOG_WARN(Widget, "removeChild: widget %d has no child instance %llu",
		            widgetId, static_cast<unsigned long long>(child));
		return false;
	}

	// Everything that came in with it: the ref element it hangs under, and the
	// id RANGE the graft renumbered into this tree. Nested embeds live inside
	// that range too, so they are caught by the same test.
	const int rootElem = it->rootElem;
	const int lo = it->idOffset, hi = it->idMax;
	std::vector<HorizonCode::InstanceId> dying{ child };
	for (const Instance::Embed& em : w->embeds)
		if (em.scriptId != child && em.idOffset >= lo && em.idMax <= hi)
			dying.push_back(em.scriptId);

	w->tree.elements.erase(
		std::remove_if(w->tree.elements.begin(), w->tree.elements.end(),
			[&](const std::unique_ptr<HE::UIElement>& e)
			{
				if (!e) return true;
				return e->id == rootElem || (e->id > lo && e->id <= hi);
			}),
		w->tree.elements.end());
	w->embeds.erase(std::remove_if(w->embeds.begin(), w->embeds.end(),
		[&](const Instance::Embed& em)
		{
			return std::find(dying.begin(), dying.end(), em.scriptId) != dying.end();
		}), w->embeds.end());

	// Innermost first, so a nested row's script never outlives the tree it acts
	// on — the same order destroyWidget uses.
	for (auto d = dying.rbegin(); d != dying.rend(); ++d) rt().destroy(*d);

	// A destroyed row must not keep the hover, the press or the focus. Nothing
	// says which element they were on any more, and a stale id would light up
	// whatever gets that number next.
	auto forget = [&](int& elem){ if (elem == rootElem || (elem > lo && elem <= hi)) elem = 0; };
	forget(w->hoveredElem); forget(w->pressedElem); forget(w->focusedElem);
	forget(w->draggingSlider); forget(w->draggingText); forget(w->draggingSplit);
	if (w->focusedElem == 0 && m_focusWidget == w->id) m_focusWidget = 0;

	m_visualDirty = true;
	return true;
}

int WidgetManager::clearChildren(int widgetId, const std::string& parentName)
{
	Instance* w = find(widgetId);
	if (!w) return 0;
	int parentId = 0;
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == parentName) { parentId = ep->id; break; }
	if (parentId == 0) return 0;

	// Collected first, removed after: removeChild rewrites both vectors, so
	// walking them while erasing would skip half the rows.
	std::vector<HorizonCode::InstanceId> mine;
	for (const Instance::Embed& em : w->embeds)
		if (const HE::UIElement* root = w->tree.find(em.rootElem);
		    root && root->parentId == parentId)
			mine.push_back(em.scriptId);
	int removed = 0;
	for (const HorizonCode::InstanceId id : mine)
		if (removeChild(widgetId, id)) ++removed;
	return removed;
}

// ── Layers: dialogs, popups, menus (docs/he-apps-plan.md B4) ─────────────────

bool WidgetManager::takesInput(int widgetId) const
{
	// Nothing is up: everybody hears everything, which is how it was before
	// layers existed and is still the overwhelmingly common case.
	if (m_grabs.empty()) return true;
	// Otherwise exactly one widget does. Not "the modal and everything above
	// it": a popup opened from a dialog pushes its own grab, so the top of the
	// stack is always the right answer, and a widget that is merely drawn on top
	// without having asked for the input does not get it.
	return widgetId == m_grabs.back().widget;
}

bool WidgetManager::hasModal() const
{
	for (const Grab& g : m_grabs) if (g.kind == Grab::Kind::Modal) return true;
	return false;
}

void WidgetManager::popGrab(bool notify)
{
	if (m_grabs.empty()) return;
	const Grab g = m_grabs.back();
	m_grabs.pop_back();
	m_visualDirty = true;

	if (g.kind == Grab::Kind::Dropdown)
	{
		if (Instance* w = find(g.widget))
			if (auto* cb = dynamic_cast<HE::UIComboBox*>(w->tree.find(g.elem)))
			{ cb->open = false; cb->hoverIndex = -1; }
		return;
	}

	Instance* w = find(g.widget);
	if (w)
	{
		w->visible = false;
		if (g.kind == Grab::Kind::Modal) w->zOrder = g.prevZOrder;
	}
	// The focus goes back where it was. A dialog that leaves it wherever it
	// happened to end up is the classic dialog bug: the page behind reappears
	// with nothing selected, and the next arrow key starts from scratch.
	m_focusWidget = g.prevFocusWidget;
	if (Instance* prev = find(g.prevFocusWidget)) prev->focusedElem = g.prevFocusElem;
	// Last, because a graph that reacts to it may open the next dialog — and it
	// must land on a stack this one has already left.
	if (notify && w) rt().fireOnDismissed(w->scriptId);
}

void WidgetManager::showModal(int widgetId)
{
	Instance* w = find(widgetId);
	if (!w) { HE_LOG_WARN(Widget, "showModal: no widget with id %d", widgetId); return; }

	Grab g;
	g.kind = Grab::Kind::Modal;
	g.widget = widgetId;
	g.prevFocusWidget = m_focusWidget;
	if (const Instance* pf = find(m_focusWidget)) g.prevFocusElem = pf->focusedElem;
	g.prevZOrder = w->zOrder;

	// On TOP, always. A dialog that blocks the input while drawing behind
	// something is the worst of both, and it is exactly what happens when an
	// author sets a z-order once and forgets about it.
	int top = w->zOrder;
	for (const Instance& other : m_instances) top = std::max(top, other.zOrder);
	w->zOrder = top + 1;
	w->visible = true;
	// The focus belongs to the dialog from here on — the trap is that
	// takesInput() refuses everything else, and this is the other half: the
	// keyboard has to START inside it, not wherever it was.
	m_focusWidget = widgetId;
	w->focusedElem = 0;
	m_grabs.push_back(g);
	m_visualDirty = true;
}

void WidgetManager::openPopupAt(int widgetId, float x, float y)
{
	Instance* w = find(widgetId);
	if (!w) { HE_LOG_WARN(Widget, "openPopup: no widget with id %d", widgetId); return; }

	Grab g;
	g.kind = Grab::Kind::Popup;
	g.widget = widgetId;
	g.prevFocusWidget = m_focusWidget;
	if (const Instance* pf = find(m_focusWidget)) g.prevFocusElem = pf->focusedElem;
	g.prevZOrder = w->zOrder;

	// Where it goes is decided HERE, not by how the asset was authored. A
	// convention ("anchor your root top-left") is a convention the first user
	// breaks; re-anchoring the roots makes the placement the same for every
	// popup that was ever drawn.
	const HE::UIWidgetCanvas canvas =
		HE::uiResolveCanvas(w->tree, m_lastViewportW, m_lastViewportH);
	const float sx = canvas.scaleX > 0.0f ? canvas.scaleX : 1.0f;
	const float sy = canvas.scaleY > 0.0f ? canvas.scaleY : 1.0f;
	for (auto& ep : w->tree.elements)
	{
		if (!ep || ep->parentId != 0) continue;
		HE::UIElement& e = *ep;
		// MEASURED first, then re-anchored. On a stretched axis sizeX/sizeY are
		// not a size at all — they are the difference to the anchored span, and
		// for the commonest root of all (a panel filling its widget) that is
		// zero or negative. Reading them here would place a popup 0x0 wide.
		const HE::UIWidgetRect had = HE::uiElementRect(w->tree, e, &canvas);
		e.anchorMinX = e.anchorMaxX = 0.0f;
		e.anchorMinY = e.anchorMaxY = 0.0f;
		e.pivotX = e.pivotY = 0.0f;
		e.sizeX = had.w; e.sizeY = had.h;
		// Clamped so the whole thing stays on screen: a context menu opened near
		// the bottom right must come UP from the pointer, not off the edge.
		const float maxX = std::max(0.0f, canvas.width  - had.w);
		const float maxY = std::max(0.0f, canvas.height - had.h);
		e.posX = std::clamp(x / sx, 0.0f, maxX);
		e.posY = std::clamp(y / sy, 0.0f, maxY);
	}

	int top = w->zOrder;
	for (const Instance& other : m_instances) top = std::max(top, other.zOrder);
	w->zOrder = top + 1;
	w->visible = true;
	m_focusWidget = widgetId;
	w->focusedElem = 0;
	m_grabs.push_back(g);
	m_visualDirty = true;
}

void WidgetManager::openPopupAtPointer(int widgetId)
{
	openPopupAt(widgetId, m_pointerX, m_pointerY);
}

bool WidgetManager::closeTopLayer()
{
	if (m_grabs.empty()) return false;
	popGrab(/*notify=*/true);
	return true;
}

// ── Lists (docs/he-apps-plan.md B2) ──────────────────────────────────────────

HE::UIListView* WidgetManager::findList(int widgetId, const std::string& listName)
{
	Instance* w = find(widgetId);
	if (!w) return nullptr;
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == listName)
			return dynamic_cast<HE::UIListView*>(ep.get());
	return nullptr;
}
const HE::UIListView* WidgetManager::findList(int widgetId, const std::string& listName) const
{
	const Instance* w = find(widgetId);
	if (!w) return nullptr;
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == listName)
			return dynamic_cast<const HE::UIListView*>(ep.get());
	return nullptr;
}

std::vector<HE::UIWidgetRef*> WidgetManager::listRowsOf(Instance& w, int listId)
{
	std::vector<HE::UIWidgetRef*> rows;
	for (auto& ep : w.tree.elements)
		if (auto* r = dynamic_cast<HE::UIWidgetRef*>(ep.get());
		    r && r->parentId == listId && r->rowIndex >= 0)
			rows.push_back(r);
	return rows;
}

bool WidgetManager::setListCount(int widgetId, const std::string& listName, int count)
{
	HE::UIListView* lv = findList(widgetId, listName);
	if (!lv) return false;
	const int n = count > 0 ? count : 0;
	if (lv->itemCount == n) return true;
	// The count and everything that follows from it live on the element, so a
	// Set Property node does exactly this much housekeeping too.
	lv->setItemCount(n);
	m_visualDirty = true;
	if (m_content && !m_syncingLists)
		if (Instance* w = find(widgetId)) syncLists(*w);
	return true;
}

int WidgetManager::listCount(int widgetId, const std::string& listName) const
{
	const HE::UIListView* lv = findList(widgetId, listName);
	return lv ? lv->itemCount : 0;
}

HorizonCode::InstanceId WidgetManager::listRow(int widgetId, const std::string& listName,
                                               int index) const
{
	const Instance* w = find(widgetId);
	const HE::UIListView* lv = findList(widgetId, listName);
	if (!w || !lv || index < 0) return 0;
	for (const auto& ep : w->tree.elements)
		if (const auto* r = dynamic_cast<const HE::UIWidgetRef*>(ep.get());
		    r && r->parentId == lv->id && r->rowIndex == index)
			return instanceOfChild(*w, r->id);
	return 0;
}

bool WidgetManager::refreshList(int widgetId, const std::string& listName)
{
	Instance* w = find(widgetId);
	HE::UIListView* lv = findList(widgetId, listName);
	if (!w || !lv) return false;
	// "Bound to nothing" is how a rebind is asked for: syncLists fills in every
	// row whose bound item differs from the one it stands on, and -1 differs
	// from all of them.
	for (HE::UIWidgetRef* r : listRowsOf(*w, lv->id)) r->rowBound = -1;
	m_visualDirty = true;
	if (m_content && !m_syncingLists) syncLists(*w);
	return true;
}

bool WidgetManager::setListSelected(int widgetId, const std::string& listName,
                                    int index, bool on)
{
	Instance* w = find(widgetId);
	HE::UIListView* lv = findList(widgetId, listName);
	if (!w || !lv) return false;
	const bool changed = index < 0 ? lv->clearSelection() : lv->setSelected(index, on);
	if (!changed) return true;
	m_visualDirty = true;
	const ScriptTarget t = scriptTargetFor(*w, lv->id);
	rt().fireOnSelectionChanged(t.scriptId, t.elem, lv->firstSelected());
	return true;
}

int WidgetManager::listSelected(int widgetId, const std::string& listName) const
{
	const HE::UIListView* lv = findList(widgetId, listName);
	return lv ? lv->firstSelected() : -1;
}

bool WidgetManager::scrollListToItem(int widgetId, const std::string& listName, int index)
{
	Instance* w = find(widgetId);
	HE::UIListView* lv = findList(widgetId, listName);
	if (!w || !lv) return false;
	if (!lv->scrollToItem(index)) return true;   // already in view: nothing to do
	m_visualDirty = true;
	if (m_content && !m_syncingLists) syncLists(*w);
	return true;
}

void WidgetManager::selectListRow(Instance& w, HE::UIListView& lv, int item)
{
	if (lv.selectionMode == 0 || item < 0 || item >= lv.itemCount) return;
	// Multiple mode TOGGLES on a plain press, rather than needing a held Ctrl or
	// Shift. Two reasons, and the second is the real one: this call is not given
	// the modifier keys at all, and a list where a second row can only be picked
	// with a chord is worse than one where it cannot — the mode was chosen by the
	// author precisely because more than one row is meant to be picked.
	const bool changed = lv.selectionMode == 2
		? lv.setSelected(item, !lv.isSelected(item))
		: lv.setSelected(item, true);
	if (!changed) return;
	m_visualDirty = true;
	const ScriptTarget t = scriptTargetFor(w, lv.id);
	rt().fireOnSelectionChanged(t.scriptId, t.elem, lv.firstSelected());
}

void WidgetManager::syncLists()
{
	if (!m_content) return;
	for (Instance& w : m_instances) syncLists(w);
}

void WidgetManager::syncLists(Instance& w)
{
	if (!m_content || m_syncingLists) return;
	// Everything below grafts rows, removes rows and runs the owner's graph. All
	// three can come back in here (see m_syncingLists), so the door is shut for
	// the duration and whatever they ask for is picked up by the next sync.
	struct Latch
	{
		bool& f;
		explicit Latch(bool& b) : f(b) { f = true; }
		~Latch() { f = false; }
	} latch{ m_syncingLists };

	// Ids first: realizing a row appends to tree.elements, and iterating it while
	// that happens walks off the end of the vector it is holding.
	std::vector<int> listIds;
	for (const auto& ep : w.tree.elements)
		if (ep && ep->type() == HE::UIWidgetType::ListView) listIds.push_back(ep->id);
	if (listIds.empty()) return;

	// A guard, not a design: a Row Height of one unit in a tall list would
	// otherwise ask for thousands of rows and defeat the whole point. Anything
	// above this is a list whose rows are too small to read anyway.
	constexpr int kMaxRealizedRows = 256;

	for (const int listId : listIds)
	{
		HE::UIListView* lv = dynamic_cast<HE::UIListView*>(w.tree.find(listId));
		if (!lv) continue;

		// ── Which items can be seen ──────────────────────────────────────────
		int first = 0, count = 0;
		const float step = lv->rowStep();
		if (lv->itemCount > 0 && step > 0.0f && !lv->rowWidget.empty())
		{
			lv->scrollOffset = std::clamp(lv->scrollOffset, 0.0f, lv->maxScroll());
			first = static_cast<int>(std::floor(lv->scrollOffset / step));
			first = std::clamp(first, 0, lv->itemCount - 1);
			// Rows the view touches, plus one: while the list is scrolled between
			// two rows there is a sliver of a further one at the bottom edge, and
			// a list that realizes it only after the scroll has finished shows a
			// gap for exactly as long as the finger is down.
			const float reach = lv->scrollOffset + lv->innerHeight();
			count = static_cast<int>(std::ceil(reach / step)) - first + 1;
			count = std::clamp(count, 1, kMaxRealizedRows);
			count = std::min(count, lv->itemCount - first);
		}
		// ── Exactly that many rows, no more ──────────────────────────────────
		// A COPY of the template path: grafting fires the new row's Construct,
		// which is the owner's code and could take this very list out of the
		// tree. Everything below is written so that nothing reaches back through
		// `lv` after a graft or a removal without finding it again.
		const std::string rowAsset = lv->rowWidget;
		std::vector<HE::UIWidgetRef*> rows = listRowsOf(w, listId);
		// Rows whose template no longer matches are not recyclable — they are the
		// wrong widget entirely — so they go before anything is counted.
		std::vector<HorizonCode::InstanceId> doomed;
		for (HE::UIWidgetRef* r : rows)
			if (r->widgetPath != rowAsset)
				if (const HorizonCode::InstanceId id = instanceOfChild(w, r->id)) doomed.push_back(id);
		for (const HorizonCode::InstanceId id : doomed) removeChild(w.id, id);
		if (!doomed.empty()) rows = listRowsOf(w, listId);

		while (static_cast<int>(rows.size()) > count)
		{
			const HorizonCode::InstanceId id = instanceOfChild(w, rows.back()->id);
			if (id == 0 || !removeChild(w.id, id)) break;   // stop rather than spin
			const std::size_t before = rows.size();
			rows = listRowsOf(w, listId);
			if (rows.size() >= before) break;               // nothing went: give up
		}
		for (int i = static_cast<int>(rows.size()); i < count; ++i)
		{
			// Grafted at the item it will show, so it is placed correctly on the
			// very first frame rather than at row 0 for one of them.
			if (graftChildRef(w, *m_content, listId, rowAsset, first + i) == 0)
			{
				// The template is missing or circular; the graft already said so.
				// Stop asking for it this frame instead of failing once per row.
				count = i;
				break;
			}
			// A row's Construct could have taken the list away. Nothing below
			// this line is meaningful if it did.
			if (!w.tree.find(listId)) break;
		}
		lv = dynamic_cast<HE::UIListView*>(w.tree.find(listId));
		if (!lv) continue;
		rows = listRowsOf(w, listId);
		if (rows.empty()) continue;
		count = std::min(count, static_cast<int>(rows.size()));
		const int last = first + count - 1;

		// ── Point them at the right items ────────────────────────────────────
		// Rows already standing on a wanted item keep it. That is the whole of
		// the recycling: scrolling by one moves ONE row, and the other nine are
		// not touched, not rebuilt and not asked again.
		std::vector<bool> taken(static_cast<std::size_t>(count), false);
		for (HE::UIWidgetRef* r : rows)
			if (r->rowIndex >= first && r->rowIndex <= last)
			{
				const std::size_t slot = static_cast<std::size_t>(r->rowIndex - first);
				if (!taken[slot]) taken[slot] = true;
				else r->rowIndex = -1;    // a duplicate; it will take a free item below
			}
			else r->rowIndex = -1;
		std::size_t next = 0;
		for (HE::UIWidgetRef* r : rows)
		{
			if (r->rowIndex >= 0) continue;
			while (next < taken.size() && taken[next]) ++next;
			if (next >= taken.size()) break;   // more rows than items: cannot happen
			taken[next] = true;
			r->rowIndex = first + static_cast<int>(next);
		}

		// ── …and tell the owner about the ones that moved ────────────────────
		// By ELEMENT ID, not by pointer, and re-found before every fire. The
		// handler is the owner's graph and it may legitimately remove a row it
		// was just handed (widget.removeChild on the Ref from Get List Row) —
		// that erases elements, and a pointer taken before the call would be
		// dangling for the rest of the loop. The latch above stops the list from
		// being resized underneath us; this stops one row from taking the others
		// with it.
		std::vector<std::pair<int, int>> pending;   // (ref element, item)
		for (HE::UIWidgetRef* r : rows)
			if (r->rowIndex >= 0 && r->rowBound != r->rowIndex)
			{
				r->rowBound = r->rowIndex;
				pending.emplace_back(r->id, r->rowIndex);
			}
		if (!pending.empty()) m_visualDirty = true;
		const ScriptTarget t = scriptTargetFor(w, listId);
		for (const auto& [refElem, item] : pending)
		{
			if (!w.tree.find(refElem)) continue;   // a previous bind took it out
			rt().fireOnRowBind(t.scriptId, t.elem, item);
		}
	}
}

void WidgetManager::destroyWidget(int id)
{
	m_visualDirty = true;
	if (m_focusWidget == id) m_focusWidget = 0;
	// A dialog that is destroyed rather than closed still has to let go of the
	// input — otherwise takesInput answers for a widget that no longer exists
	// and the whole application is inert with nothing on screen to blame.
	// Silently: OnDismissed goes to a graph that is being torn down.
	while (!m_grabs.empty() && m_grabs.back().widget == id) popGrab(/*notify=*/false);
	m_grabs.erase(std::remove_if(m_grabs.begin(), m_grabs.end(),
		[id](const Grab& g){ return g.widget == id; }), m_grabs.end());
	if (m_tooltipWidget == id)
	{
		m_tooltipWidget = m_tooltipElem = 0;
		m_tooltipHeld = 0.0f; m_tooltipUp = false;
	}
	if (Instance* w = find(id))
	{
		HE_LOG_DEBUG(Widget, "Destroying widget id %d", id);
		// Copied for createWidget's reason, in the mirror image: destroy() fires
		// Destruct, Destruct is user graph code, and a Destruct that creates or
		// destroys a widget moves m_instances out from under `w` — after which
		// the very next loop iteration reads a freed embed list.
		std::vector<HorizonCode::InstanceId> scripts;
		scripts.reserve(w->embeds.size() + 1);
		// Embedded widgets are instances of their own and have to go with it,
		// innermost first — otherwise their scripts outlive the tree they act on.
		for (auto it = w->embeds.rbegin(); it != w->embeds.rend(); ++it)
			scripts.push_back(it->scriptId);
		scripts.push_back(w->scriptId);
		for (const HorizonCode::InstanceId sid : scripts)
			rt().destroy(sid); // fires "Destruct", then drops it
	}
	else
	{
		HE_LOG_WARN(Widget, "destroyWidget(%d): no such widget (already destroyed?)", id);
	}
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
		[&](const Instance& w){ return w.id == id; }), m_instances.end());
}

void WidgetManager::clear()
{
	m_visualDirty = true;
	// Nothing is holding the input any more, because nothing is left to hold it.
	m_grabs.clear();
	m_tooltipWidget = m_tooltipElem = 0;
	m_tooltipHeld = 0.0f; m_tooltipUp = false;
	// Fire each widget's "Destruct" and unregister it from the shared runtime
	// (which may also host the level script / GameInstance — so tear down
	// per-instance, don't wipe). Snapshot the ids first: a Destruct handler may
	// itself destroy widgets, mutating m_instances mid-iteration.
	std::vector<HorizonCode::InstanceId> ids;
	ids.reserve(m_instances.size());
	for (const auto& w : m_instances)
	{
		// Embedded instances first: they act on the host's tree.
		for (auto it = w.embeds.rbegin(); it != w.embeds.rend(); ++it)
			ids.push_back(it->scriptId);
		ids.push_back(w.scriptId);
	}
	if (!ids.empty()) HE_LOG_DEBUG(Widget, "Clearing %zu live widget(s)", ids.size());
	for (const HorizonCode::InstanceId sid : ids) rt().destroy(sid);
	m_instances.clear();
	m_focusWidget = 0;
	// Nothing is left to hover, so the pointer is over nothing — otherwise the
	// last verdict would outlive the widgets it was about.
	m_pointerOverUI = false;
}

// Each of these changes what the next frame would look like, so each raises the
// visual-dirty flag an event-driven app sleeps on (see consumeVisualDirty).
void WidgetManager::showWidget(int id)  { if (Instance* w = find(id)) { w->visible = true;  m_visualDirty = true; } }
void WidgetManager::hideWidget(int id)
{
	// Hiding a dialog IS closing it. The natural OK button is
	// "OnClicked → Hide Widget (Get Self)", and without this line the grab
	// outlives the picture: takesInput keeps naming a widget the pointer scan
	// skips for being invisible, so every route in refuses everything with
	// nothing on screen to blame for it. Only Escape would get the user out.
	releaseGrabsOf(id);
	if (Instance* w = find(id)) { w->visible = false; m_visualDirty = true; }
}

// Every layer this widget was holding, closed the way closeTopLayer closes one:
// the focus goes back and the widget's own graph hears OnDismissed. Hiding and
// closing are the same event seen from two sides, so they must not differ in
// what they leave behind.
void WidgetManager::releaseGrabsOf(int widgetId)
{
	bool any = false;
	for (const Grab& g : m_grabs) if (g.widget == widgetId) { any = true; break; }
	if (!any) return;
	// Top down, so a popup opened FROM this dialog goes before the dialog does.
	for (int guard = 0; guard < 64 && !m_grabs.empty(); ++guard)
	{
		const bool mine = m_grabs.back().widget == widgetId;
		popGrab(/*notify=*/mine);
		bool more = false;
		for (const Grab& g : m_grabs) if (g.widget == widgetId) { more = true; break; }
		if (!more) break;
	}
}
void WidgetManager::setZOrder(int id, int z) { if (Instance* w = find(id)) { w->zOrder = z; m_visualDirty = true; } }

const HE::UIWidgetTree* WidgetManager::tree(int widgetId) const
{
	const Instance* w = find(widgetId);
	return w ? &w->tree : nullptr;
}

bool WidgetManager::isAlive(int id) const   { return find(id) != nullptr; }
bool WidgetManager::isVisible(int id) const
{
	const Instance* w = find(id);
	return w && w->visible;
}
int WidgetManager::zOrder(int id) const
{
	const Instance* w = find(id);
	return w ? w->zOrder : 0;
}

bool WidgetManager::callFunction(int id, const std::string& name)
{
	Instance* w = find(id);
	if (!w)
	{
		HE_LOG_WARN(Widget, "callWidgetFunction('%s') on widget id %d: no such widget",
		            name.c_str(), id);
		return false;
	}
	if (!rt().callFunction(w->scriptId, name, /*requirePublic=*/true))
	{
		// Either the function does not exist or it is not public — both look like
		// "the button does nothing" from the script side.
		HE_LOG_WARN(Widget, "Widget id %d has no public function '%s'", id, name.c_str());
		return false;
	}
	return true;
}

namespace
{
	// Can this kind of value be interpolated at all? A string or a bool has no
	// halfway, and an animation that "eased" one by snapping at the end would be
	// a duration that means nothing.
	bool animatable(HE::UIPropType t)
	{
		return t == HE::UIPropType::Float || t == HE::UIPropType::Color
		    || t == HE::UIPropType::Vec2;
	}

	// One step along the curve. OutBack leaves [0,1] on purpose, so a colour is
	// clamped at the write and a number is not: overshooting a position is the
	// point, overshooting a red channel is a colour nobody chose.
	HE::UIPropValue animLerp(const HE::UIPropValue& from, const HE::UIPropValue& to, float k)
	{
		switch (from.type)
		{
		case HE::UIPropType::Float:
			return HE::UIPropValue::ofFloat(from.f + (to.f - from.f) * k);
		case HE::UIPropType::Vec2:
			return HE::UIPropValue::ofVec2(from.v2 + (to.v2 - from.v2) * k);
		case HE::UIPropType::Color:
		{
			glm::vec4 c = from.col + (to.col - from.col) * k;
			c = glm::clamp(c, glm::vec4(0.0f), glm::vec4(1.0f));
			return HE::UIPropValue::ofColor(c);
		}
		default:
			return to;
		}
	}
}

bool WidgetManager::animate(int widgetId, int elemId, const std::string& prop,
                            const HE::UIPropValue& to, float seconds, HE::UIEase ease)
{
	Instance* w = find(widgetId);
	if (!w) return false;
	HE::UIElement* e = w->tree.find(elemId);
	if (!e) return false;
	const HE::UIPropValue cur = e->getPropAny(prop);
	// A property this element does not have reads back as a default-constructed
	// Float, which would happily "animate" and write nothing. The target's type
	// has to match what is actually there, which also catches "fade the Text".
	if (!animatable(cur.type) || cur.type != to.type) return false;

	// Whatever was running on this property stops here, from wherever it got to
	// — that is what makes a retarget smooth — and it reports nothing, because a
	// cancelled animation did not finish.
	stopAnimations(widgetId, elemId, prop);
	// Before the write below, for the same reason a clip does it before its
	// first frame: one line later this value is the animation's, not the
	// widget's (see Instance::originals).
	rememberOriginal(*w, elemId, prop);

	m_visualDirty = true;
	if (seconds <= 0.0f)
	{
		// A duration turned down to zero is still an instruction: write it, and
		// say it is done, so a graph that waits for the end is not left waiting.
		e->setPropAny(prop, to);
		const ScriptTarget t = scriptTargetFor(*w, elemId);
		rt().fireOnAnimationFinished(t.scriptId, t.elem, prop);
		return true;
	}
	Instance::Anim a;
	a.elem = elemId; a.prop = prop; a.from = cur; a.to = to;
	a.dur = seconds; a.ease = ease;
	w->anims.push_back(std::move(a));
	return true;
}

int WidgetManager::stopAnimations(int widgetId, int elemId, const std::string& prop)
{
	Instance* w = find(widgetId);
	if (!w) return 0;
	const std::size_t before = w->anims.size();
	w->anims.erase(std::remove_if(w->anims.begin(), w->anims.end(),
		[&](const Instance::Anim& a)
		{
			if (elemId != 0 && a.elem != elemId) return false;
			if (!prop.empty() && a.prop != prop) return false;
			return true;
		}), w->anims.end());
	const int stopped = static_cast<int>(before - w->anims.size());
	if (stopped) m_visualDirty = true;
	return stopped;
}

// The clips a playing entry draws from, and the id offset they need. One place,
// because "the widget's own or an embed's" is asked by play, by stop and by the
// tick, and three copies of that decision is three chances to forget the offset.
const std::vector<HE::UIAnimClip>* WidgetManager::clipsOf(const Instance& w, int embed,
                                                          int& offset) const
{
	offset = 0;
	if (embed < 0) return &w.tree.animations;
	if (embed >= static_cast<int>(w.embeds.size())) return nullptr;
	offset = w.embeds[embed].idOffset;
	return &w.embeds[embed].animations;
}

// Remember what a property looked like before an animation touched it, unless
// something already did. "Unless" is the whole rule: the second animation on a
// property must not record the first one's mid-flight value as the original.
void WidgetManager::rememberOriginal(Instance& w, int elem, const std::string& prop)
{
	for (const Instance::Original& o : w.originals)
		if (o.elem == elem && o.prop == prop) return;
	const HE::UIElement* e = w.tree.find(elem);
	// A track naming a property the element does not have records nothing:
	// getPropAny would hand back a default-constructed value, and restoring
	// THAT is writing a zero nobody authored. (setPropAny ignores the unknown
	// name on the way back anyway, but a list full of phantom rows is a list
	// nobody can read in a debugger.)
	if (!e) return;
	bool known = false;
	for (const HE::UIPropDesc& pd : e->allProperties()) if (pd.name == prop) { known = true; break; }
	if (!known) return;
	w.originals.push_back({ elem, prop, e->getPropAny(prop) });
}

bool WidgetManager::playAnimation(int widgetId, const std::string& clip, const bool* loop,
                                  HE::UIAnimDirection dir, bool restore)
{
	Instance* w = find(widgetId);
	if (!w || clip.empty()) return false;

	// The widget's own clips first, then each embed's — a page that names a clip
	// means its own, and a component's clips belong to the component.
	int embed = -1, offset = 0;
	const HE::UIAnimClip* c = HE::uiAnimFind(w->tree.animations, clip);
	if (!c)
		for (int i = 0; i < static_cast<int>(w->embeds.size()) && !c; ++i)
			if ((c = HE::uiAnimFind(w->embeds[i].animations, clip)))
			{ embed = i; offset = w->embeds[i].idOffset; }
	if (!c) return false;

	// Two writers on one property is the fight replace-on-same-property exists
	// to prevent, and between a clip and a tween the clip is the more specific
	// instruction. It takes every property it drives.
	for (const HE::UIAnimTrack& tr : c->tracks)
	{
		stopAnimations(widgetId, tr.element + offset, tr.prop);
		// Before the first frame writes anything: this is the state a Restore
		// puts back, and one tick later it would already be the animation's.
		rememberOriginal(*w, tr.element + offset, tr.prop);
	}

	// Already playing = rewind, not a second player on the same clip.
	for (Instance::Playing& p : w->playing)
		if (p.clip == clip && p.embed == embed)
		{
			p.t = 0.0f;
			p.loop = loop ? *loop : c->loop;
			p.dir = dir; p.restore = restore;
			m_visualDirty = true;
			return true;
		}
	Instance::Playing p;
	p.embed = embed; p.clip = clip; p.loop = loop ? *loop : c->loop;
	p.dir = dir; p.restore = restore;
	w->playing.push_back(std::move(p));
	m_visualDirty = true;
	return true;
}

// Put one property back and forget it. Forgetting matters: the entry is what
// makes the NEXT animation on that property record its own starting point, and
// a stale one would restore a value from two animations ago.
void WidgetManager::restoreOne(Instance& w, int elem, const std::string& prop)
{
	for (std::size_t i = 0; i < w.originals.size(); ++i)
	{
		const Instance::Original& o = w.originals[i];
		if (o.elem != elem || o.prop != prop) continue;
		if (HE::UIElement* e = w.tree.find(elem)) e->setPropAny(prop, o.value);
		w.originals.erase(w.originals.begin() + static_cast<long>(i));
		m_visualDirty = true;
		return;
	}
}

int WidgetManager::stopAllAnimations(int widgetId)
{
	return stopAnimationClip(widgetId) + stopAnimations(widgetId);
}

int WidgetManager::restoreOriginalState(int widgetId)
{
	Instance* w = find(widgetId);
	if (!w) return 0;
	// Stopped first, and not optionally: a clip still running would write its
	// own value over the restored one on the next tick, and the node would look
	// like it had done nothing.
	stopAllAnimations(widgetId);
	int n = 0;
	for (const Instance::Original& o : w->originals)
		if (HE::UIElement* e = w->tree.find(o.elem)) { e->setPropAny(o.prop, o.value); ++n; }
	w->originals.clear();
	if (n) m_visualDirty = true;
	return n;
}

int WidgetManager::widgetIdForScript(HorizonCode::InstanceId scriptId) const
{
	if (!scriptId) return 0;
	for (const Instance& w : m_instances)
	{
		if (w.scriptId == scriptId) return w.id;
		// An embedded component's graph is its own instance, and "this widget"
		// for it is the page it was grafted into — that page is what owns the
		// elements and the clips its offset points at.
		for (const Instance::Embed& em : w.embeds) if (em.scriptId == scriptId) return w.id;
	}
	return 0;
}

int WidgetManager::stopAnimationClip(int widgetId, const std::string& clip)
{
	Instance* w = find(widgetId);
	if (!w) return 0;
	const std::size_t before = w->playing.size();
	w->playing.erase(std::remove_if(w->playing.begin(), w->playing.end(),
		[&](const Instance::Playing& p){ return clip.empty() || p.clip == clip; }),
		w->playing.end());
	const int stopped = static_cast<int>(before - w->playing.size());
	if (stopped) m_visualDirty = true;
	return stopped;
}

bool WidgetManager::isPlayingAnimation(int widgetId, const std::string& clip) const
{
	const Instance* w = find(widgetId);
	if (!w) return false;
	if (clip.empty()) return !w->playing.empty();
	for (const Instance::Playing& p : w->playing) if (p.clip == clip) return true;
	return false;
}

float WidgetManager::animationTime(int widgetId, const std::string& clip) const
{
	const Instance* w = find(widgetId);
	if (!w) return -1.0f;
	for (const Instance::Playing& p : w->playing) if (p.clip == clip) return p.t;
	return -1.0f;
}

// The element of `w` called `name`, or 0. Same lookup findList does, without the
// type: anything can be animated.
int WidgetManager::elementIdByName(int widgetId, const std::string& name) const
{
	const Instance* w = find(widgetId);
	if (!w || name.empty()) return 0;
	for (const auto& ep : w->tree.elements)
		if (ep && ep->name == name) return ep->id;
	return 0;
}

bool WidgetManager::animateNamed(int widgetId, const std::string& elemName,
                                 const std::string& prop, const HE::UIPropValue& to,
                                 float seconds, HE::UIEase ease)
{
	const int elem = elementIdByName(widgetId, elemName);
	return elem != 0 && animate(widgetId, elem, prop, to, seconds, ease);
}

int WidgetManager::stopAnimationsNamed(int widgetId, const std::string& elemName,
                                       const std::string& prop)
{
	const int elem = elementIdByName(widgetId, elemName);
	return elem == 0 ? 0 : stopAnimations(widgetId, elem, prop);
}

bool WidgetManager::isAnimating() const
{
	for (const Instance& w : m_instances)
		if (!w.anims.empty() || !w.playing.empty()) return true;
	return false;
}

void WidgetManager::tick(float dt)
{
	// ── Authored clips ───────────────────────────────────────────────────────
	// Before the single-property ones, so that a tween started DURING a clip
	// (the "animate() replaces a clip's track" half of the rule) has the last
	// word on the property they share within the same frame.
	if (dt > 0.0f)
		for (auto& w : m_instances)
		{
			if (w.playing.empty()) continue;
			std::vector<std::string> ended;
			std::vector<HE::UIAnimSample> samples;
			for (auto& p : w.playing)
			{
				int offset = 0;
				const std::vector<HE::UIAnimClip>* clips = clipsOf(w, p.embed, offset);
				const HE::UIAnimClip* c = clips ? HE::uiAnimFind(*clips, p.clip) : nullptr;
				if (!c) { p.t = -1.0f; continue; }   // the asset changed under it

				// Playback ends at the LAST KEY, not at the authored length.
				// After it every track is holding a value that will not change
				// again, so the tail is a wait with nothing in it — and a clip
				// that reports finished a second after it visibly finished makes
				// every graph waiting on it late.
				const float end  = HE::uiAnimPlayEnd(*c);
				// A pass, which is the clip's length forwards or backwards and
				// twice it out and back. p.t counts the PASS; what gets sampled
				// is worked out from it below.
				const float span = HE::uiAnimPlaySpan(p.dir, end);
				p.t += dt;
				bool done = false;
				if (p.t >= span)
				{
					// A looping clip never ends, so it never reports one either.
					// Wrapping rather than resetting to 0 keeps a long frame from
					// swallowing the start of the next pass. With no keys there
					// is nothing to loop, and looping forever over nothing is a
					// widget that never stops asking for frames.
					if (p.loop && span > 0.0f) p.t = std::fmod(p.t, span);
					else                       { p.t = span; done = true; }
				}
				samples.clear();
				HE::uiAnimEvaluate(*c, HE::uiAnimDirectedTime(p.dir, p.t, end), samples);
				for (const HE::UIAnimSample& s : samples)
					if (HE::UIElement* e = w.tree.find(s.element + offset))
					{
						// The track's type has to match what is there, for the
						// same reason animate() checks: a clip authored against
						// an element that has since changed type must not write
						// a colour into a number.
						if (e->getPropAny(s.prop).type == s.value.type)
							e->setPropAny(s.prop, s.value);
					}
				if (done)
				{
					// "Play it and put it back": the properties this clip drove
					// go back to what they were before anything animated them,
					// BEFORE the finished event fires. A graph reacting to the
					// end must not see the last frame of an animation that, as
					// far as this widget is concerned, has been undone.
					if (p.restore)
						for (const HE::UIAnimTrack& tr : c->tracks)
							restoreOne(w, tr.element + offset, tr.prop);
					ended.push_back(p.clip);
					p.t = -1.0f;
				}
			}
			w.playing.erase(std::remove_if(w.playing.begin(), w.playing.end(),
				[](const Instance::Playing& p){ return p.t < 0.0f; }), w.playing.end());
			m_visualDirty = true;
			// Fired after the list is settled, like the tweens': a graph reacting
			// to the end may start the next clip on this same widget.
			for (const std::string& name : ended)
				rt().fireOnClipFinished(w.scriptId, name);
		}

	// ── Animations ───────────────────────────────────────────────────────────
	// Before the Tick events, so a graph that reads a property in its Tick reads
	// the value this frame DRAWS rather than the one before it.
	//
	// Hidden widgets animate too, on purpose: "fade it in" is started on the
	// frame the thing appears, and half of those start while it is still hidden.
	if (dt > 0.0f)
		for (auto& w : m_instances)
		{
			if (w.anims.empty()) continue;
			// Finished ones are collected and fired AFTER the loop: a graph
			// reacting to one may start the next animation on this same widget,
			// and appending to a vector being iterated is how that ends in a
			// dangling reference.
			std::vector<std::pair<int, std::string>> finished;
			for (auto& a : w.anims)
			{
				a.t += dt;
				const bool done = a.t >= a.dur;
				HE::UIElement* e = w.tree.find(a.elem);
				if (!e) { a.t = a.dur; continue; }   // element gone: drop it below
				// The last write is the target ITSELF rather than the curve's
				// value at t=1. Belt and braces: every curve here returns
				// exactly 1 at 1 (a test pins that) and the interpolation then
				// lands on the target for every value anyone would animate — so
				// this guards the rounding case nobody has produced, not a bug
				// that was ever seen. It costs a branch.
				e->setPropAny(a.prop, done ? a.to
					: animLerp(a.from, a.to, HE::uiEaseApply(a.ease, a.t / a.dur)));
				if (done) finished.emplace_back(a.elem, a.prop);
			}
			w.anims.erase(std::remove_if(w.anims.begin(), w.anims.end(),
				[](const Instance::Anim& a){ return a.t >= a.dur; }), w.anims.end());
			m_visualDirty = true;
			for (const auto& [elem, prop] : finished)
			{
				const ScriptTarget t = scriptTargetFor(w, elem);
				rt().fireOnAnimationFinished(t.scriptId, t.elem, prop);
			}
		}

	// ── The tooltip's wait ───────────────────────────────────────────────────
	// The delay IS the feature: a hint that appears the moment the pointer
	// crosses a button is a hint that gets in the way of using it.
	//
	// And this is the one place that has to raise the dirty flag itself. An
	// application draws when something CHANGED; the change here is time passing,
	// which nothing else reports — without this line the tooltip would appear
	// the next time the mouse moves, which is exactly when it is no longer
	// wanted (docs/he-apps-plan.md A2, and the risk list's "forgotten flag").
	if (m_tooltipElem != 0 && !m_tooltipUp)
	{
		constexpr float kTooltipDelay = 0.5f;   // seconds
		m_tooltipHeld += dt > 0.0f ? dt : 0.0f;
		if (m_tooltipHeld >= kTooltipDelay) { m_tooltipUp = true; m_visualDirty = true; }
	}

	for (auto& w : m_instances)
	{
		if (!w.visible) continue;
		rt().fireTick(w.scriptId, dt);
		// Embedded widgets tick as themselves — a health bar that animates has
		// its own Tick, and it must run while the page holding it is up.
		for (const Instance::Embed& em : w.embeds)
			rt().fireTick(em.scriptId, dt);
	}
}

namespace
{
	// The item under the pointer in a list, or -1 for padding, a gap or past the
	// end. The list's own numbers (row height, spacing, the offset) are in the
	// units of the widget it was authored in, so the pointer has to be brought
	// all the way into THOSE — canvas scale first, then the embed's scale.
	int listRowAtPointer(const HE::UIWidgetTree& tree, const HE::UIListView& lv,
	                     const HE::UIWidgetCanvas& canvas, float mouseY)
	{
		if (canvas.scaleY <= 0.0f) return -1;
		const HE::UIWidgetRect r = HE::uiElementRect(tree, lv, &canvas);
		float us = 1.0f, vs = 1.0f;
		HE::uiElementUnitScale(tree, lv, us, vs, &canvas);
		if (vs <= 0.0f) return -1;
		return lv.rowAt((mouseY / canvas.scaleY - r.y) / vs);
	}

	// What a modal dims the rest of the screen with. A plain constant and not a
	// theme role: a scrim is not a colour anybody designs, it is the absence of
	// one, and it has to work on a light page and a dark one alike.
	constexpr glm::vec4 kModalScrim{ 0.0f, 0.0f, 0.0f, 0.45f };

	// ── The open list of a ComboBox ──────────────────────────────────────────
	// Where the dropdown hangs, in the combo's own canvas units. It is computed
	// in ONE place because two things need it and they must not drift: the draw
	// puts the rows there, and the pointer decides which row it is over from the
	// same numbers. Below the box normally, above it when there is no room — a
	// menu that opens off the bottom of the screen is a menu with no last entry.
	HE::UIWidgetRect comboListRect(const HE::UIWidgetTree& tree, const HE::UIComboBox& cb,
	                               const HE::UIWidgetCanvas* canvas)
	{
		const HE::UIWidgetRect box = HE::uiElementRect(tree, cb, canvas);
		float us = 1.0f, vs = 1.0f;
		HE::uiElementUnitScale(tree, cb, us, vs, canvas);
		const float rowH  = cb.optionHeight() * vs;
		const float total = rowH * static_cast<float>(cb.options.size());
		// A CARD with a gap, not a lid glued to the box. The box may be a pill,
		// and a rectangle glued under a pill leaves two notches of background
		// where its bottom corners curve away — while a card that stands off it
		// reads correctly at every rounding, which is what a menu has to do when
		// the shape it hangs from is the author's to choose.
		const float gap   = 4.0f * vs;
		const float below = box.y + box.h + gap;
		const float space = (canvas ? canvas->height : tree.canvasHeight) - below;
		HE::UIWidgetRect r{ box.x, below, box.w, total };
		if (total > space && box.y - gap - total >= 0.0f) r.y = box.y - gap - total;
		return r;
	}

	// Which option the pointer is over, or -1 for "not on the list at all" —
	// which is the click that closes it without picking anything.
	int comboOptionAtPointer(const HE::UIWidgetTree& tree, const HE::UIComboBox& cb,
	                         const HE::UIWidgetCanvas& canvas, float mouseX, float mouseY)
	{
		if (cb.options.empty() || canvas.scaleX <= 0.0f || canvas.scaleY <= 0.0f) return -1;
		const HE::UIWidgetRect r = comboListRect(tree, cb, &canvas);
		const float cx = mouseX / canvas.scaleX, cy = mouseY / canvas.scaleY;
		if (cx < r.x || cx > r.x + r.w || cy < r.y || cy > r.y + r.h) return -1;
		const float rowH = r.h / static_cast<float>(cb.options.size());
		if (rowH <= 0.0f) return -1;
		const int i = static_cast<int>((cy - r.y) / rowH);
		return (i >= 0 && i < static_cast<int>(cb.options.size())) ? i : -1;
	}

	// Is `elemId` inside `ancestorId` (or is it that element)? What "the pointer
	// is still on this list" means when it is really on a button in one of its
	// rows.
	bool isSelfOrDescendant(const HE::UIWidgetTree& tree, int ancestorId, int elemId)
	{
		int guard = 0;
		for (const HE::UIElement* e = tree.find(elemId);
		     e && guard++ < static_cast<int>(tree.elements.size()) + 1;
		     e = tree.find(e->parentId))
		{
			if (e->id == ancestorId) return true;
			if (e->parentId == 0) break;
		}
		return false;
	}
}

bool WidgetManager::isInteractive(const Instance& w, const HE::UIElement& e) const
{
	if (e.interactive()) return true;
	// Bound by a pointer-event node? (elem 0 = any element.) eventBindingsOf
	// serves interpreted (Event nodes) and compiled (static tables) scripts alike.
	// The bindings to ask are the ones of the script that OWNS this element, and
	// they name it by its own id — for an embedded widget both differ.
	const ScriptTarget target = scriptTargetFor(w, e.id);
	for (const auto& b : rt().eventBindingsOf(target.scriptId))
		if (b.elem == 0 || b.elem == target.elem)
		{
			const std::string& n = b.name;
			if (n == "OnClicked" || n == "OnPressed"    || n == "OnReleased" ||
			    n == "OnHovered" || n == "OnUnhovered"  ||
			    n == "OnMouseEnter" || n == "OnMouseLeave")
				return true;
		}
	return false;
}

// Topmost hit-testable element under a point. The pointer asks this, and so
// does a file being dragged in — one arithmetic with two callers, because the
// day they are two is the day the drop lands somewhere other than the highlight
// promised.
WidgetManager::PointerHit WidgetManager::topmostHit(float vpWidth, float vpHeight,
                                                    float x, float y)
{
	PointerHit hit;
	long topKey = 0;
	for (auto& w : m_instances)
	{
		if (!w.visible) continue;
		// A layer is up and this is not it: inert, all the way down. The
		// same question every other way in asks (takesInput).
		if (!takesInput(w.id)) continue;
		// Same resolution the draw uses (see extract) — a hit test on a
		// differently-scaled canvas is a button that is not where it looks.
		const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
		const float sx = canvas.scaleX;
		const float sy = canvas.scaleY;
		for (const auto& ep : w.tree.elements)
		{
			const HE::UIElement& e = *ep;
			if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
			// hitTestable false = transparent to the pointer: the only way
			// out of the stack.
			if (!e.hitTestable) continue;
			// Disabled is inert, all the way down: a greyed-out button that
			// still hovers and clicks is the classic UI lie.
			if (!HE::uiElementEffectiveEnabled(w.tree, e)) continue;
			// Faded to nothing means gone — a menu at opacity 0 must not
			// keep swallowing the clicks meant for what is behind it.
			if (HE::uiElementEffectiveOpacity(w.tree, e) <= 0.001f) continue;
			HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
			// Rotated? Then the pointer is turned back into the element's
			// own unrotated space and the test stays a plain rectangle —
			// a tilted button has to be clickable where it LOOKS, and its
			// corners are the parts that move the furthest.
			float mcx = x / sx, mcy = y / sy;
			if (HE::UIRotation rot; HE::uiElementRotation(w.tree, e, rot, &canvas))
				HE::uiUnrotatePoint(rot, mcx, mcy, mcx, mcy);
			const float testX = mcx * sx, testY = mcy * sy;
			// A clipping ancestor cuts the hit area down with the picture:
			// the half of a list row that hangs out of its box is not
			// visible, so it must not be clickable either.
			HE::UIWidgetRect clip{};
			if (HE::uiElementClipRect(w.tree, e, clip, &canvas))
			{
				const float cx0 = std::max(r.x, clip.x), cy0 = std::max(r.y, clip.y);
				const float cx1 = std::min(r.x + r.w, clip.x + clip.w);
				const float cy1 = std::min(r.y + r.h, clip.y + clip.h);
				if (cx1 <= cx0 || cy1 <= cy0) continue;   // fully cut off
				r.x = cx0; r.y = cy0; r.w = cx1 - cx0; r.h = cy1 - cy0;
			}
			const float x0 = r.x * sx, y0 = r.y * sy;
			const float x1 = (r.x + r.w) * sx, y1 = (r.y + r.h) * sy;
			if (testX < x0 || testX > x1 || testY < y0 || testY > y1)
				continue;
			const long key = (long)w.zOrder * 1000000 + elementSortKey(w.tree, e);
			if (!hit.widget || key >= topKey)
			{
				hit.widget = &w; hit.elem = e.id;
				topKey = key; hit.cursor = e.hoverCursor;
				hit.interactive = isInteractive(w, e);
				// A text field asks for the I-beam by BEING a text field.
				// Only when the author picked nothing else: an explicit
				// hoverCursor is a decision and stays one.
				if (hit.cursor == HE::UICursor::Default &&
				    e.type() == HE::UIWidgetType::TextInput)
					hit.cursor = HE::UICursor::Text;
			}
		}
	}
	return hit;
}

bool WidgetManager::processPointer(float vpWidth, float vpHeight,
                                   float mouseX, float mouseY,
                                   bool primaryDown, bool valid,
                                   bool secondaryDown)
{
	// Remembered for the calls that get no frame with them: a context menu opens
	// at the pointer, a tooltip is drawn beside it, and a popup is placed in a
	// canvas that needs the viewport.
	m_lastViewportW = vpWidth; m_lastViewportH = vpHeight;
	// Did the POINTER move, or is this just the next frame? An application calls
	// this every frame whether the mouse stirred or not, so "where the mouse is"
	// arrives sixty times a second and would otherwise outvote the keyboard on
	// everything the two of them share — a menu's highlight, above all.
	const bool moved = valid && (mouseX != m_pointerX || mouseY != m_pointerY);
	if (valid) { m_pointerX = mouseX; m_pointerY = mouseY; }

	// ── An open dropdown owns the pointer ────────────────────────────────────
	// Its list hangs OUTSIDE the combo's own rectangle, and every rectangle the
	// scan below knows is an element's own — so the scan cannot see it, and
	// nothing short of owning the pointer outright would work. Nothing
	// underneath hears any of it.
	//
	// ── Everything here commits on RELEASE, never on the press ───────────────
	// Which is what a choice IS: pressing aims, letting go decides, and until
	// you let go you may still change your mind by moving. A dropdown that
	// picks the row the moment the button goes down takes the decision away
	// half a gesture early, and it feels exactly as wrong as it is — that was
	// the report this comes from.
	//
	// It also gets press-drag-release for nothing: hold the button, run down
	// the list, let go on the entry you want. That is how a menu has worked
	// since menus existed, and it falls out of committing on the release rather
	// than being a second code path.
	if (!m_grabs.empty() && m_grabs.back().kind == Grab::Kind::Dropdown)
	{
		const Grab g = m_grabs.back();
		Instance* w = find(g.widget);
		auto* cb = w ? dynamic_cast<HE::UIComboBox*>(w->tree.find(g.elem)) : nullptr;
		if (!w || !cb) popGrab(/*notify=*/false);   // it went away under us
		else
		{
			const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);
			const int over = valid
				? comboOptionAtPointer(w->tree, *cb, canvas, mouseX, mouseY) : -1;
			// Only a pointer that MOVED takes the highlight. Without that the
			// arrow keys were unusable: they set the highlighted row, this ran
			// on the very next frame with the mouse parked somewhere else, and
			// put it back — which looks exactly like the row flashing and the
			// selection snapping home, because that is what it was.
			//
			// It is also how every menu behaves: the keyboard has it until the
			// mouse is moved, and then the mouse has it.
			if (moved && cb->hoverIndex != over) { cb->hoverIndex = over; m_visualDirty = true; }
			m_hoverCursor = HE::UICursor::Default;
			// The row under the pointer when the button comes UP is the one
			// that is chosen — and a release with nothing under it closes the
			// list without choosing, which is the same decision made the other
			// way and therefore happens at the same moment.
			if (!primaryDown && m_wasDown)
			{
				if (over >= 0 && cb->selectedIndex != over)
				{
					cb->selectedIndex = over;
					const ScriptTarget t = scriptTargetFor(*w, cb->id);
					rt().fireOnSelectionChanged(t.scriptId, t.elem, over);
				}
				popGrab(/*notify=*/false);
			}
			// Right-click closes it too — on ITS release, for the same reason.
			// A menu that one button dismisses and the other does not is a menu
			// that behaves differently depending on which finger you used.
			if (!secondaryDown && m_wasSecondaryDown && !m_grabs.empty() &&
			    m_grabs.back().kind == Grab::Kind::Dropdown)
				popGrab(/*notify=*/false);
			m_wasDown = primaryDown;
			// BOTH edges settle here, not just the left one: a right button held
			// across the close would otherwise read as a fresh press next frame
			// and open a context menu nobody asked for.
			m_wasSecondaryDown = secondaryDown;
			m_pointerOverUI = true;
			return true;
		}
	}

	// Topmost hit-testable element under the pointer, across all visible
	// widgets: highest (widget zOrder, element sort key) wins — the same order
	// the draw paints in, so what you SEE on top is what the pointer meets.
	//
	// "Topmost" is the whole rule, and it is not "topmost among the ones that
	// react": an element that is hit-testable takes the pointer even when it
	// does nothing with it, and what lies under it stays untouched. Anything
	// else means a panel drawn over a button is a panel you can click straight
	// through. Decoration opts out by being hitTestable = false, which is what
	// that flag has always claimed to mean.
	// Rows a list can show may have changed since the last frame (the count was
	// set, it was scrolled, the window resized). Done before the hit test, not
	// after the draw, so a row that has just appeared is clickable in the same
	// frame it becomes visible.
	syncLists();

	const PointerHit ph = valid ? topmostHit(vpWidth, vpHeight, mouseX, mouseY)
	                            : PointerHit{};
	Instance* topW = ph.widget;
	int  topElem = ph.elem;
	// The element the pointer LANDED on, before the bubbling below moves the
	// event to whoever reacts. A list highlights the row under the pointer even
	// when what is under it is a button in that row, and only the raw hit can
	// answer that.
	const int topHit = ph.elem;
	bool topActs = ph.interactive;      // does the winner actually take events?
	HE::UICursor topCursor = ph.cursor;
	// The winner blocks either way. Who RECEIVES the hover, the press and the
	// focus is decided from there — and it bubbles UP, never down: a click on a
	// button's caption is a click on the button, and so is one on the icon next
	// to it. That is what makes a button out of several children behave like one
	// thing. Only when nothing on the way up reacts does the pointer stop dead,
	// which is the blocking half of the rule.
	if (!topActs && topW)
	{
		const HE::UIElement* hit = topW->tree.find(topElem);
		topElem = 0;
		for (int guard = 0; hit && hit->parentId != 0 && guard < 256; ++guard)
		{
			const HE::UIElement* p = topW->tree.find(hit->parentId);
			if (!p) break;
			if (isInteractive(*topW, *p))
			{
				topElem = p->id;
				// The cursor comes with the element that took the pointer,
				// unless the thing under it named one of its own.
				if (topCursor == HE::UICursor::Default) topCursor = p->hoverCursor;
				break;
			}
			hit = p;
		}
	}
	m_hoverCursor = topCursor; // app maps this to a system cursor

	// ── Which tooltip is being waited out ────────────────────────────────────
	// The nearest thing under the pointer that has something to say. A caption
	// on a button carries no tooltip and the button does — and the pointer being
	// on the caption is still the pointer being on the button, which is why this
	// walks up from the RAW hit rather than from whatever took the click.
	{
		int tipW = 0, tipE = 0;
		if (topW && topHit != 0)
		{
			int guard = 0;
			for (const HE::UIElement* e = topW->tree.find(topHit);
			     e && guard++ < static_cast<int>(topW->tree.elements.size()) + 1; )
			{
				if (!e->tooltip.empty()) { tipW = topW->id; tipE = e->id; break; }
				if (e->parentId == 0) break;
				e = topW->tree.find(e->parentId);
			}
		}
		if (tipW != m_tooltipWidget || tipE != m_tooltipElem)
		{
			// One that was already showing has to be taken off the screen, and
			// that is a change even though nothing was drawn yet this frame.
			if (m_tooltipUp) m_visualDirty = true;
			m_tooltipWidget = tipW; m_tooltipElem = tipE;
			m_tooltipHeld = 0.0f; m_tooltipUp = false;
		}
	}

	const bool pressEdge   = primaryDown && !m_wasDown;
	const bool releaseEdge = !primaryDown && m_wasDown;
	const bool secondEdge  = secondaryDown && !m_wasSecondaryDown;
	m_wasSecondaryDown = secondaryDown;

	// ── A press on nothing closes a popup ────────────────────────────────────
	// That IS the difference between a popup and a modal: one is dismissed by
	// looking somewhere else, the other has to be answered. `topW` can only be
	// the popup itself here — everything else was already refused by
	// takesInput — so "nothing was hit" means "nothing of the popup was hit".
	if ((pressEdge || secondEdge) && topW == nullptr && !m_grabs.empty() &&
	    m_grabs.back().kind == Grab::Kind::Popup)
	{
		closeTopLayer();
		m_wasDown = primaryDown;
		// The click belonged to the UI: it dismissed the menu, and it must not
		// also fire into the world behind it.
		m_pointerOverUI = true;
		return true;
	}

	// ── The other button ─────────────────────────────────────────────────────
	// Fired at the nearest element above the hit that is actually listening for
	// it, so a right-click on a row's label reaches the row. Its own event and
	// not a flag on OnClicked: a right-click opens things, it never presses one.
	if (secondEdge && topW && topHit != 0)
	{
		int guard = 0;
		for (const HE::UIElement* e = topW->tree.find(topHit);
		     e && guard++ < static_cast<int>(topW->tree.elements.size()) + 1; )
		{
			const ScriptTarget t = scriptTargetFor(*topW, e->id);
			bool listens = false;
			for (const auto& b : rt().eventBindingsOf(t.scriptId))
				if (b.name == "OnRightClicked" && (b.elem == 0 || b.elem == t.elem))
				{ listens = true; break; }
			if (listens) { rt().fireOnRightClicked(t.scriptId, t.elem); break; }
			if (e->parentId == 0) break;
			e = topW->tree.find(e->parentId);
		}
	}

	for (auto& w : m_instances)
	{
		const bool isTop = topW == &w;
		const int  hot   = isTop ? topElem : 0;
		// Hover, press and focus are DRAWN states (a button lights up, a field
		// takes the ring), so a change in any of them is a reason to redraw —
		// and mere pointer movement that changes none of them is not. Sampled
		// before this widget's block runs, compared after it.
		const int wasHovered = w.hoveredElem, wasPressed = w.pressedElem,
		          wasFocused = w.focusedElem;
		struct DirtyOnStateChange
		{
			WidgetManager& m; const Instance& w;
			int h, p, f;
			~DirtyOnStateChange()
			{
				if (w.hoveredElem != h || w.pressedElem != p || w.focusedElem != f)
					m.m_visualDirty = true;
			}
		} dirtyGuard{ *this, w, wasHovered, wasPressed, wasFocused };
		// Typed entry points: the compiled side takes a method, the interpreted
		// one the same named path as before, and both still reach everyone bound
		// to this widget's script.
		auto fireP = [&](void (HorizonCode::Runtime::*fn)(HorizonCode::InstanceId, int), int elem)
		{
			// An element that came in with a WidgetRef belongs to THAT widget's
			// script, under the id it has in its own asset.
			const ScriptTarget t = scriptTargetFor(w, elem);
			(rt().*fn)(t.scriptId, t.elem);
		};

		// ── Hover transitions ────────────────────────────────────────────────
		// Event names differ per type; fire BOTH candidate names — the Runner
		// only matches Event nodes that actually exist.
		if (w.hoveredElem != hot)
		{
			if (w.hoveredElem != 0)
			{
				fireP(&HorizonCode::Runtime::fireOnUnhovered, w.hoveredElem);
				fireP(&HorizonCode::Runtime::fireOnMouseLeave, w.hoveredElem);
			}
			if (hot != 0)
			{
				fireP(&HorizonCode::Runtime::fireOnHovered, hot);
				fireP(&HorizonCode::Runtime::fireOnMouseEnter, hot);
			}
			w.hoveredElem = hot;
		}

		// ── The hovered ROW of every list ────────────────────────────────────
		// Not part of the hover transition above and it cannot be: the pointer
		// stays on the same list element while it travels from row to row, so
		// the element the hover is ON never changes. What changes is the item
		// under it, which only this arithmetic knows.
		for (auto& ep : w.tree.elements)
		{
			auto* lv = dynamic_cast<HE::UIListView*>(ep.get());
			if (!lv) continue;
			const int was = lv->hoveredRow;
			// Still "on the list" when the pointer is on something INSIDE one of
			// its rows — a button in a row must not make the row go cold.
			lv->hoveredRow = (isTop && topHit != 0 &&
			                  isSelfOrDescendant(w.tree, lv->id, topHit))
				? listRowAtPointer(w.tree, *lv,
				                   HE::uiResolveCanvas(w.tree, vpWidth, vpHeight), mouseY)
				: -1;
			if (lv->hoveredRow != was) m_visualDirty = true;
		}

		// ── Press ────────────────────────────────────────────────────────────
		if (pressEdge)
		{
			w.pressedElem = hot;
			if (hot != 0)
			{
				const HE::UIElement* e = w.tree.find(hot);
				if (e && e->type() == HE::UIWidgetType::Button)
					fireP(&HorizonCode::Runtime::fireOnPressed, hot);
				// Slider: start dragging (value updated below).
				if (e && e->type() == HE::UIWidgetType::Slider)
					w.draggingSlider = hot;
				// Splitter: only the DIVIDER starts a drag. A press anywhere
				// else in the splitter's rect is a press on whatever pane is
				// there, and grabbing the whole container would make the panes
				// unclickable.
				if (const auto* sp = dynamic_cast<const HE::UISplitter*>(e))
				{
					const HE::UIWidgetCanvas canvas =
						HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
					const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *sp, &canvas);
					float us = 1.0f, vs = 1.0f;
					HE::uiElementUnitScale(w.tree, *sp, us, vs, &canvas);
					const float len = sp->vertical ? r.h : r.w;
					const float div = std::min(sp->dividerSize * (sp->vertical ? vs : us), len);
					const float first = sp->clampedRatio(len) * std::max(0.0f, len - div);
					const float p  = sp->vertical ? mouseY / canvas.scaleY : mouseX / canvas.scaleX;
					const float lo = (sp->vertical ? r.y : r.x) + first;
					if (p >= lo && p <= lo + div) w.draggingSplit = hot;
				}
				// A Tab Box: the strip switches pages, the rest is the page.
				if (const auto* tb = dynamic_cast<const HE::UITabBox*>(e))
				{
					const HE::UIWidgetCanvas canvas =
						HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
					const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *tb, &canvas);
					float us = 1.0f, vs = 1.0f;
					HE::uiElementUnitScale(w.tree, *tb, us, vs, &canvas);
					// The labels come from the TREE, not from the drawing cache:
					// what a click hits must never depend on whether a frame has
					// been drawn yet.
					std::vector<std::string> labels;
					for (const auto& cp : w.tree.elements)
						if (cp && cp->parentId == tb->id)
							labels.push_back(cp->name.empty() ? std::string("Page") : cp->name);
					// In the element's own pixel space, like everything the
					// shared layout answers in.
					const HE::UIWidgetRect pxr{ r.x * canvas.scaleX, r.y * canvas.scaleY,
					                            r.w * canvas.scaleX, r.h * canvas.scaleY };
					const int hit = HE::UITabBox::tabAtPoint(
						pxr, tb->fontSize * vs, tb->tabPadding * vs, tb->tabHeight * vs,
						labels, tb->fontAtlasKey, mouseX, mouseY);
					if (hit >= 0 && hit != tb->activeTab)
						if (auto* live = dynamic_cast<HE::UITabBox*>(w.tree.find(hot)))
						{
							live->activeTab = hit;
							m_visualDirty = true;
							const ScriptTarget t2 = scriptTargetFor(w, hot);
							rt().fireOnValueChanged(t2.scriptId, t2.elem,
							                        static_cast<float>(hit));
						}
				}
				// TextInput: focus it, and put the caret where the click was —
				// a field you can only ever append to is not a field.
				if (e && e->type() == HE::UIWidgetType::TextInput)
				{
					if (w.focusedElem != hot)
					{
						w.focusedElem = hot;
						m_focusWidget = w.id;
						fireP(&HorizonCode::Runtime::fireOnFocused, hot);
					}
					// A click IS the confirmation: pointing at a field and
					// pressing is nobody's idea of "select it for later". The
					// two-state rule is for the keyboard, where the same
					// gesture has to serve arriving and entering.
					m_focusEditing = true;
					setCaretFromPointer(vpWidth, vpHeight, mouseX, mouseY);
					// From here until the button comes up, pointer movement
					// extends the selection instead of moving the caret.
					w.draggingText = hot;
				}
				else
				{
					if (w.focusedElem != 0 && w.focusedElem != hot)
					{
						// Pressed something else in this widget → unfocus its field.
						fireP(&HorizonCode::Runtime::fireOnUnfocused, w.focusedElem);
						w.focusedElem = 0;
						if (m_focusWidget == w.id) m_focusWidget = 0;
					}
					// A press in a list picks the row under it — and takes the
					// keyboard focus, so the arrows step through the list rather
					// than hunting for the next button somewhere on the page.
					if (auto* lv = dynamic_cast<HE::UIListView*>(w.tree.find(hot)))
					{
						setFocus(w.id, hot);
						selectListRow(w, *lv,
							listRowAtPointer(w.tree, *lv,
								HE::uiResolveCanvas(w.tree, vpWidth, vpHeight), mouseY));
					}
				}
			}
			else if (w.focusedElem != 0)
			{
				// Pressed empty space → unfocus.
				fireP(&HorizonCode::Runtime::fireOnUnfocused, w.focusedElem);
				w.focusedElem = 0;
				if (m_focusWidget == w.id) m_focusWidget = 0;
			}
		}

		// ── Slider drag ──────────────────────────────────────────────────────
		if (w.draggingSlider != 0 && primaryDown)
		{
			if (auto* s = dynamic_cast<HE::UISlider*>(w.tree.find(w.draggingSlider)))
			{
				const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
				const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *s, &canvas);
				const float mouseCanvasX = mouseX / canvas.scaleX;
				float t = r.w > 0.0f ? (mouseCanvasX - r.x) / r.w : 0.0f;
				t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
				const float nv = s->minValue + t * (s->maxValue - s->minValue);
				if (nv != s->value)
				{
					s->value = nv;
					const ScriptTarget t2 = scriptTargetFor(w, w.draggingSlider);
					rt().fireOnValueChanged(t2.scriptId, t2.elem, nv);
				}
			}
		}

		// ── Splitter drag ────────────────────────────────────────────────────
		// Same shape as the slider above, and the same reason for it: the
		// pointer is turned into a fraction of the ELEMENT's own rect, in canvas
		// units, so a nested splitter measures against its own half and not
		// against the window.
		if (w.draggingSplit != 0 && primaryDown)
		{
			if (auto* sp = dynamic_cast<HE::UISplitter*>(w.tree.find(w.draggingSplit)))
			{
				const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
				const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *sp, &canvas);
				float us = 1.0f, vs = 1.0f;
				HE::uiElementUnitScale(w.tree, *sp, us, vs, &canvas);
				const float len = sp->vertical ? r.h : r.w;
				const float div = std::min(sp->dividerSize * (sp->vertical ? vs : us), len);
				const float usable = std::max(1.0f, len - div);
				const float p = (sp->vertical ? mouseY / canvas.scaleY - r.y
				                              : mouseX / canvas.scaleX - r.x) - div * 0.5f;
				float nv = p / usable;
				nv = nv < 0.0f ? 0.0f : (nv > 1.0f ? 1.0f : nv);
				// Stored raw and clamped by the layout, so the minimums are
				// enforced in ONE place — pushing against a minimum and letting
				// go must not leave the ratio somewhere the layout would move it.
				if (nv != sp->ratio)
				{
					sp->ratio = nv;
					m_visualDirty = true;
					const ScriptTarget t2 = scriptTargetFor(w, w.draggingSplit);
					rt().fireOnValueChanged(t2.scriptId, t2.elem, sp->clampedRatio(len));
				}
			}
		}

		// ── Text selection drag ──────────────────────────────────────────────
		// Held down after a press inside a field: the anchor stays put and the
		// caret follows the pointer, which is what selecting with the mouse is.
		// No hit test — dragging past the edge of the field must keep extending,
		// exactly like it does everywhere else.
		if (w.draggingText != 0 && primaryDown && m_focusWidget == w.id &&
		    w.focusedElem == w.draggingText)
			dragCaretFromPointer(vpWidth, vpHeight, mouseX, mouseY);

		// ── Release ──────────────────────────────────────────────────────────
		if (releaseEdge)
		{
			if (w.pressedElem != 0 && w.pressedElem == hot)
				activateElement(w, hot);
			w.pressedElem    = 0;
			w.draggingSlider = 0;
			w.draggingText   = 0;
			w.draggingSplit  = 0;
		}
	}

	m_wasDown = primaryDown;
	// Remembered, not just returned: both apps discard the return value, and a
	// script asking "is the pointer on the UI?" runs long after this call.
	// A modal's dim covers the whole screen and is drawn by THIS class, not by
	// any element — so nothing but this line can say that a click on it belongs
	// to the UI. Without it, clicking beside a pause dialog fires into the game.
	m_pointerOverUI = topW != nullptr || hasModal();
	return m_pointerOverUI;
}

// ── What a drop would land on ────────────────────────────────────────────────
// The hit walks UP to the first element that accepts one. That is the same
// bubbling a click does and it is the same reason: a control people see as one
// thing is several, and a file dragged onto a card's caption is dragged onto the
// card. An element that never said "Accepts Drop" is simply not in the answer,
// which is what makes the flag mean something.
int WidgetManager::dropTargetAt(Instance& w, int hitElem) const
{
	int guard = 0;
	for (const HE::UIElement* e = w.tree.find(hitElem);
	     e && guard++ < static_cast<int>(w.tree.elements.size()) + 1; )
	{
		if (e->acceptsDrop) return e->id;
		if (e->parentId == 0) break;
		e = w.tree.find(e->parentId);
	}
	return 0;
}

bool WidgetManager::dropHover(float vpWidth, float vpHeight, float x, float y,
                              bool active)
{
	m_lastViewportW = vpWidth; m_lastViewportH = vpHeight;
	int newW = 0, newE = 0;
	if (active)
	{
		// Rows a list can show may have moved since the last frame, exactly as
		// in processPointer — a drag hovering over a list that has just been
		// scrolled must highlight the row that is there NOW.
		syncLists();
		const PointerHit ph = topmostHit(vpWidth, vpHeight, x, y);
		if (ph.widget)
		{
			const int t = dropTargetAt(*ph.widget, ph.elem);
			if (t != 0) { newW = ph.widget->id; newE = t; }
		}
	}
	// Only a CHANGE is a redraw. A drag sends its position continuously, and an
	// application that repaints on every one of them is the idle-CPU problem
	// (A2) wearing a different hat.
	if (newW != m_dropWidget || newE != m_dropElem)
	{
		m_dropWidget = newW; m_dropElem = newE;
		m_visualDirty = true;
	}
	return m_dropElem != 0;
}

bool WidgetManager::processDrop(float vpWidth, float vpHeight, float x, float y,
                                const std::vector<std::string>& paths)
{
	m_lastViewportW = vpWidth; m_lastViewportH = vpHeight;
	// The highlight is over either way — the gesture ended, whatever it hit.
	if (m_dropWidget != 0 || m_dropElem != 0)
	{
		m_dropWidget = 0; m_dropElem = 0;
		m_visualDirty = true;
	}
	if (paths.empty()) return false;

	// Resolved HERE and not from the remembered hover: the two agree in the
	// normal case, and where they do not it is because the drag moved after the
	// last position we were told about. Where the file was let go is the truth.
	syncLists();
	Instance* w = nullptr;
	int elem = 0;
	{
		const PointerHit ph = topmostHit(vpWidth, vpHeight, x, y);
		if (ph.widget)
		{
			const int t = dropTargetAt(*ph.widget, ph.elem);
			if (t != 0) { w = ph.widget; elem = t; }
		}
	}

	if (w && elem != 0)
	{
		const ScriptTarget t = scriptTargetFor(*w, elem);
		for (const std::string& p : paths)
			rt().fireOnFileDropped(t.scriptId, t.elem, p);
		return true;
	}

	// Nothing accepted it, so the WINDOW did. An application that opens what it
	// is handed wants one place to say so, and the GameInstance is the one thing
	// that is always there — elem 0, because no element took it.
	if (const HorizonCode::InstanceId gi = rt().gameInstance())
		for (const std::string& p : paths)
			rt().fireOnFileDropped(gi, 0, p);
	return false;
}

// The focused text field, or nullptr. Every editing entry point starts here,
// and each one re-clamps: a script can have rewritten the text since the last
// keystroke, leaving the caret pointing past the end of it.
HE::UITextInput* WidgetManager::focusedTextField(Instance*& outWidget)
{
	outWidget = nullptr;
	// Focused is not editing. Every entry point below this one is a keystroke,
	// and a field that has merely been tabbed onto does not take keystrokes —
	// that is the whole of the two-state rule, kept in ONE place so no entry
	// point can be added that forgets it.
	if (!m_focusEditing) return nullptr;
	if (m_focusWidget == 0) return nullptr;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return nullptr;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti) return nullptr;
	ti->clampCaret();
	outWidget = w;
	return ti;
}

void WidgetManager::inputText(const std::string& utf8)
{
	// Typing, deleting, moving the caret and selecting all change the picture —
	// the glyphs, the caret bar, the selection quad. Raised up front rather than
	// on each success path: these are keystrokes, and a frame drawn for one that
	// turned out to be a no-op costs nothing anybody can measure.
	m_visualDirty = true;
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti || utf8.empty()) return;
	// A read-only field shows its text and lets it be selected and copied, but
	// takes nothing in.
	if (!ti->editable) return;

	// Committed text ends the composition that produced it: the input method has
	// handed over the finished characters, and leaving the preedit run up would
	// draw them twice.
	ti->composition.clear();
	ti->compositionCursor = -1;

	// Typing over a selection replaces it — the thing every text field does and
	// this one could not, because it only ever appended.
	ti->deleteSelection();

	std::string add = utf8;

	// The input filter, applied character by character BEFORE the length limit.
	// Per character rather than per call, so a paste keeps what fits instead of
	// being refused whole — and judged against the text as it grows, because
	// whether a '-' or a '.' is allowed depends on what is already there.
	if (ti->inputFilter != HE::UITextInput::FilterAny)
	{
		std::string kept;
		kept.reserve(add.size());
		for (size_t i = 0; i < add.size(); )
		{
			const size_t next = HE::uiUtf8Next(add, i);
			const std::string ch = add.substr(i, next - i);
			// Where this character would land: the caret, plus what we have
			// already accepted from this same paste.
			if (ti->acceptsCharacter(ch, ti->caret + kept.size()))
			{
				// Appended to the field's own text as we go, so the NEXT character
				// is judged against a field that already contains this one — that
				// is what stops "1-2" and "1.2.3" from slipping through a paste.
				ti->text.insert(ti->caret + kept.size(), ch);
				kept += ch;
			}
			i = next;
		}
		// Put the text back the way it was; the insert below is the real one.
		ti->text.erase(ti->caret, kept.size());
		add.swap(kept);
		if (add.empty()) return;
	}

	if (ti->maxLength > 0)
	{
		// Counted in CHARACTERS: a limit of 8 that lets through two accented
		// letters fewer is a limit nobody can explain.
		int room = ti->maxLength - ti->charCount();
		if (room <= 0) return;
		size_t cut = 0;
		while (cut < add.size() && room > 0) { cut = HE::uiUtf8Next(add, cut); --room; }
		add.resize(cut);
		if (add.empty()) return;
	}
	ti->text.insert(ti->caret, add);
	ti->caret += add.size();
	ti->selAnchor = ti->caret;
	const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);
	rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
}

void WidgetManager::inputComposition(const std::string& utf8, int cursorByte)
{
	m_visualDirty = true;   // the preedit run is drawn, so it changes the picture
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti) return;
	// A read-only field takes no composition either — otherwise it would show
	// preedit text that can never land in it.
	if (!ti->editable) { ti->composition.clear(); return; }
	ti->composition       = utf8;
	ti->compositionCursor = cursorByte;
}

bool WidgetManager::hasComposition() const
{
	const Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return false;
	const auto* ti = dynamic_cast<const HE::UITextInput*>(w->tree.find(w->focusedElem));
	return ti && !ti->composition.empty();
}

bool WidgetManager::focusedFieldRect(float vpWidth, float vpHeight, HE::UIWidgetRect& out) const
{
	const Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return false;
	const HE::UIElement* e = w->tree.find(w->focusedElem);
	if (!e) return false;
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);
	const HE::UIWidgetRect r = HE::uiElementRect(w->tree, *e, &canvas);
	out.x = r.x * canvas.scaleX; out.y = r.y * canvas.scaleY;
	out.w = r.w * canvas.scaleX; out.h = r.h * canvas.scaleY;
	return true;
}

void WidgetManager::inputBackspace()
{
	m_visualDirty = true;   // see inputText
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti || !ti->editable) return;
	if (!ti->deleteSelection())
	{
		if (ti->caret == 0) return;               // nothing before it
		const size_t from = HE::uiUtf8Prev(ti->text, ti->caret);
		ti->text.erase(from, ti->caret - from);
		ti->caret = ti->selAnchor = from;
	}
	const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);
	rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
}

namespace
{
// ── Word boundaries ──────────────────────────────────────────────────────────
// "Word" here is the editor convention every text field uses: runs of
// letters/digits/underscore are words, everything else is a separator, and a
// jump first skips the separators it is standing in. Byte-based on purpose —
// the callers only ever hand in offsets that are already on UTF-8 boundaries,
// and a multi-byte character is never one of the ASCII separators below, so it
// falls on the "word" side and the offsets stay valid.
bool isWordByte(unsigned char ch)
{
	return std::isalnum(ch) != 0 || ch == '_' || ch >= 0x80;
}

size_t wordStartBefore(const std::string& s, size_t pos)
{
	while (pos > 0 && !isWordByte(static_cast<unsigned char>(s[pos - 1]))) --pos;
	while (pos > 0 &&  isWordByte(static_cast<unsigned char>(s[pos - 1]))) --pos;
	return pos;
}

size_t wordEndAfter(const std::string& s, size_t pos)
{
	while (pos < s.size() && !isWordByte(static_cast<unsigned char>(s[pos]))) ++pos;
	while (pos < s.size() &&  isWordByte(static_cast<unsigned char>(s[pos]))) ++pos;
	return pos;
}

// The word AROUND a position, for a double-click. Standing on a separator
// selects that run of separators instead of silently jumping to a neighbour.
void wordAround(const std::string& s, size_t pos, size_t& from, size_t& to)
{
	if (s.empty()) { from = to = 0; return; }
	if (pos >= s.size()) pos = s.size() - 1;
	const bool word = isWordByte(static_cast<unsigned char>(s[pos]));
	from = pos;
	while (from > 0 && isWordByte(static_cast<unsigned char>(s[from - 1])) == word) --from;
	to = pos;
	while (to < s.size() && isWordByte(static_cast<unsigned char>(s[to])) == word) ++to;
}
} // namespace

bool WidgetManager::editFocusedText(TextEdit op, bool extendSelection)
{
	m_visualDirty = true;   // see inputText
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti) return false;
	const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);

	// Selection off: the caret still moves, it just never drags an anchor
	// behind it, so Select All and shift-arrows have nothing to do.
	const bool extend = extendSelection && ti->selectable;

	switch (op)
	{
	case TextEdit::Delete:
	{
		if (!ti->editable) return false;
		if (!ti->deleteSelection())
		{
			if (ti->caret >= ti->text.size()) return false;   // nothing after it
			const size_t to = HE::uiUtf8Next(ti->text, ti->caret);
			ti->text.erase(ti->caret, to - ti->caret);
		}
		rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
		return true;
	}
	case TextEdit::SelectAll:
		if (ti->text.empty() || !ti->selectable) return false;
		ti->selAnchor = 0;
		ti->caret     = ti->text.size();
		return true;
	case TextEdit::DeleteWordLeft:
	{
		if (!ti->editable) return false;
		// A selection wins: Ctrl+Backspace over a selection deletes exactly the
		// selection, like every other destructive key here.
		if (!ti->deleteSelection())
		{
			if (ti->caret == 0) return false;
			const size_t from = wordStartBefore(ti->text, ti->caret);
			if (from == ti->caret) return false;
			ti->text.erase(from, ti->caret - from);
			ti->caret = ti->selAnchor = from;
		}
		rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
		return true;
	}
	case TextEdit::Left:
	case TextEdit::Right:
	case TextEdit::Home:
	case TextEdit::End:
	case TextEdit::WordLeft:
	case TextEdit::WordRight:
	case TextEdit::Up:
	case TextEdit::Down:
	{
		const size_t before = ti->caret;
		// Without shift, a plain arrow COLLAPSES a selection to its near end
		// rather than moving off the caret — what every text field does.
		if (!extend && ti->hasSelection() &&
		    (op == TextEdit::Left || op == TextEdit::Right))
		{
			ti->caret = op == TextEdit::Left ? ti->selMin() : ti->selMax();
			ti->selAnchor = ti->caret;
			ti->preferredCaretX = -1.0f;
			return true;
		}
		// Where the lines are. Cheap and only asked for by the four operations
		// below that mean something per line; a single-line field gets one range
		// covering everything, so Home and End keep their old answers exactly.
		const std::vector<HE::UITextLineRange> lines = HE::uiTextLineRanges(ti->text);
		const size_t li = HE::uiLineOfOffset(lines, ti->caret);

		switch (op)
		{
		case TextEdit::Left:      ti->caret = HE::uiUtf8Prev(ti->text, ti->caret); break;
		case TextEdit::Right:     ti->caret = HE::uiUtf8Next(ti->text, ti->caret); break;
		// Per LINE, not per field. In a single-line field that is the same
		// thing, which is why there is no `multiline` test here.
		case TextEdit::Home:      ti->caret = lines[li].begin; break;
		case TextEdit::End:       ti->caret = lines[li].end;   break;
		case TextEdit::WordLeft:  ti->caret = wordStartBefore(ti->text, ti->caret); break;
		case TextEdit::WordRight: ti->caret = wordEndAfter(ti->text, ti->caret); break;
		case TextEdit::Up:
		case TextEdit::Down:
		{
			if (!ti->multiline) return false;
			const size_t target = op == TextEdit::Up
				? (li == 0 ? li : li - 1)
				: (li + 1 >= lines.size() ? li : li + 1);
			if (target == li)
			{
				// Already at the top or the bottom. Go to that line's very
				// start or end instead of doing nothing — the same thing every
				// editor does, and it is how you reach the ends with two keys.
				ti->caret = op == TextEdit::Up ? lines[li].begin : lines[li].end;
				break;
			}
			// The COLUMN, in characters, remembered across the move: stepping
			// down through a short line and on again has to come back to where
			// it started. Counted in characters rather than pixels because that
			// is what survives a proportional font honestly — the caret lands on
			// the same character position, which is what a person is aiming at.
			const size_t col = ti->preferredCaretX >= 0.0f
				? static_cast<size_t>(ti->preferredCaretX)
				: [&] {
					size_t n = 0;
					for (size_t i = lines[li].begin; i < ti->caret; i = HE::uiUtf8Next(ti->text, i))
						++n;
					return n;
				}();
			ti->preferredCaretX = static_cast<float>(col);
			size_t at = lines[target].begin;
			for (size_t n = 0; n < col && at < lines[target].end; ++n)
				at = HE::uiUtf8Next(ti->text, at);
			ti->caret = std::min(at, lines[target].end);
			break;
		}
		default: break;
		}
		// Every move except up and down forgets the column those two aim for.
		if (op != TextEdit::Up && op != TextEdit::Down) ti->preferredCaretX = -1.0f;
		if (!extend) ti->selAnchor = ti->caret;
		return ti->caret != before || (!extend && ti->selAnchor != before);
	}
	}
	return false;
}

std::string WidgetManager::focusedSelection() const
{
	const Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return {};
	const auto* ti = dynamic_cast<const HE::UITextInput*>(w->tree.find(w->focusedElem));
	return ti ? ti->selectedText() : std::string();
}

bool WidgetManager::deleteFocusedSelection()
{
	m_visualDirty = true;   // see inputText
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti || !ti->editable || !ti->deleteSelection()) return false;
	const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);
	rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
	return true;
}

// Byte offset in the focused field that a pointer at (mouseX, mouseY) points at.
// The one place the canvas/rect/padding arithmetic lives, so click, drag and
// double-click cannot drift apart the way image and hit-area would if extract
// and processPointer used different canvases.
bool WidgetManager::caretOffsetAtPointer(float vpWidth, float vpHeight,
                                         float mouseX, float mouseY, size_t& outOffset)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	if (!ti) return false;
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);
	const HE::UIWidgetRect r = HE::uiElementRect(w->tree, *ti, &canvas);
	float us = 1.0f, vs = 1.0f;
	HE::uiElementUnitScale(w->tree, *ti, us, vs, &canvas);
	// Same 6-unit padding the field draws its text with, in pixels.
	constexpr float kPad = 6.0f;
	const float localX = mouseX - (r.x * canvas.scaleX + kPad);
	// Y is measured from the top of the text area, which is where render() puts
	// the first line. Single-line fields ignore it.
	const float localY = mouseY - (r.y * canvas.scaleY + kPad);
	outOffset = ti->caretAtPoint(localX, localY, canvas.scaleY * vs);
	return true;
}

bool WidgetManager::setCaretFromPointer(float vpWidth, float vpHeight,
                                        float mouseX, float mouseY)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !caretOffsetAtPointer(vpWidth, vpHeight, mouseX, mouseY, at)) return false;
	ti->caret = ti->selAnchor = at;
	// Any move that is not an up/down arrow forgets the column those arrows were
	// aiming for — see UITextInput::preferredCaretX.
	ti->preferredCaretX = -1.0f;
	return true;
}

bool WidgetManager::dragCaretFromPointer(float vpWidth, float vpHeight,
                                         float mouseX, float mouseY)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !ti->selectable) return false;
	if (!caretOffsetAtPointer(vpWidth, vpHeight, mouseX, mouseY, at)) return false;
	if (at == ti->caret) return false;
	// The ANCHOR stays where the press put it — that is what makes this a drag
	// rather than a second click.
	ti->caret = at;
	ti->preferredCaretX = -1.0f;
	return true;
}

bool WidgetManager::selectWordAtPointer(float vpWidth, float vpHeight,
                                        float mouseX, float mouseY)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !ti->selectable || ti->text.empty()) return false;
	if (!caretOffsetAtPointer(vpWidth, vpHeight, mouseX, mouseY, at)) return false;
	size_t from = 0, to = 0;
	wordAround(ti->text, at, from, to);
	if (from == to) return false;
	ti->selAnchor = from;
	ti->caret     = to;
	return true;
}

std::vector<int> WidgetManager::liveIds() const
{
	std::vector<int> ids;
	ids.reserve(m_instances.size());
	for (const Instance& w : m_instances) ids.push_back(w.id);
	return ids;
}

// ── Keeping what the preview holds across a reload (plan E4, Stufe 3) ────────
namespace
{
// What a person can put INTO an element, per type. Deliberately short: a label
// a script wrote is output, not state, and carrying it across a reload would
// paste the old answer over a freshly computed one.
//
// One table rather than a chain of dynamic_casts, because "which properties are
// state" is a question about the TYPE and this is the only place that answers
// it — a new widget type adds a row here or is simply not carried, which is the
// safe direction to be wrong in.
const std::vector<std::string>* statePropsOf(HE::UIWidgetType t)
{
	static const std::vector<std::string> kText   = { "Text" };
	static const std::vector<std::string> kCheck  = { "Checked" };
	static const std::vector<std::string> kSlider = { "Value" };
	static const std::vector<std::string> kCombo  = { "Selected Index" };
	static const std::vector<std::string> kList   = { "Selection" };
	// Which tab you were on and where you dragged the divider are things a
	// PERSON put there, so a save must not throw them away — the same rule the
	// text in a field follows.
	static const std::vector<std::string> kTab    = { "Active Tab" };
	static const std::vector<std::string> kSplit  = { "Ratio" };
	switch (t)
	{
		case HE::UIWidgetType::TabBox:      return &kTab;
		case HE::UIWidgetType::Splitter:    return &kSplit;
		case HE::UIWidgetType::TextInput:   return &kText;
		case HE::UIWidgetType::CheckBox:    return &kCheck;
		case HE::UIWidgetType::Slider:      return &kSlider;
		case HE::UIWidgetType::ComboBox:    return &kCombo;
		case HE::UIWidgetType::ListView:    return &kList;
		default:                            return nullptr;
	}
}
} // namespace

WidgetManager::StateSnapshot WidgetManager::captureState() const
{
	StateSnapshot snap;
	std::unordered_map<std::string, int> seen;   // asset path → copies so far

	for (const Instance& w : m_instances)
	{
		StateSnapshot::Key key;
		key.asset = w.assetPath;
		key.copy  = seen[w.assetPath]++;

		for (const auto& ep : w.tree.elements)
		{
			if (!ep) continue;
			const HE::UIElement& e = *ep;
			StateSnapshot::Element row;
			row.key       = key;
			row.elementId = e.id;

			if (const std::vector<std::string>* props = statePropsOf(e.type()))
				for (const std::string& p : *props)
					row.props.emplace_back(p, e.getPropAny(p));

			if (const auto* ti = dynamic_cast<const HE::UITextInput*>(&e))
				row.caret = static_cast<long long>(ti->caret);
			// Asked through the same virtual the scrolling itself goes through,
			// so a scroll box and a list are one case and a container added
			// later is carried without touching this.
			if (const float* off = const_cast<HE::UIElement&>(e).scrollOffsetPtr())
			{ row.scroll = *off; row.hasScroll = true; }

			if (!row.props.empty() || row.caret >= 0 || row.hasScroll)
				snap.elements.push_back(std::move(row));
		}

		snap.focus.emplace_back(key, w.focusedElem);
		if (w.scriptId != 0)
			snap.vars.emplace_back(key, rt().variablesSnapshot(w.scriptId));
	}
	return snap;
}

int WidgetManager::restoreState(const StateSnapshot& snapshot)
{
	int landed = 0;
	std::unordered_map<std::string, int> seen;
	// Which live widget answers to which key. Built once rather than searched
	// per row: a page with a list of a hundred rows is a hundred widgets, and
	// the snapshot has an entry for each of them.
	std::vector<std::pair<StateSnapshot::Key, Instance*>> byKey;
	for (Instance& w : m_instances)
		byKey.push_back({ StateSnapshot::Key{ w.assetPath, seen[w.assetPath]++ }, &w });

	auto findWidget = [&](const StateSnapshot::Key& k) -> Instance*
	{
		for (auto& [key, w] : byKey) if (key == k) return w;
		return nullptr;
	};

	for (const StateSnapshot::Element& row : snapshot.elements)
	{
		Instance* w = findWidget(row.key);
		if (!w) continue;
		HE::UIElement* e = w->tree.find(row.elementId);
		// The id is gone, or now belongs to a different KIND of element — the
		// widget was restructured. Dropped rather than written into whatever is
		// there now, which is the same rule a component parameter follows.
		if (!e) continue;

		for (const auto& [prop, value] : row.props)
		{
			// The property has to still exist AND still mean the same type. A
			// CheckBox turned into a Slider keeps neither.
			const HE::UIPropValue cur = e->getPropAny(prop);
			if (cur.type != value.type) continue;
			e->setPropAny(prop, value);
			++landed;
		}
		if (row.caret >= 0)
			if (auto* ti = dynamic_cast<HE::UITextInput*>(e))
			{
				// Clamped to the text that is actually there now, and moved to a
				// character boundary: the text may have been restored from the
				// snapshot or may be the asset's own, and a caret in the middle
				// of a UTF-8 sequence is a crash waiting for the next keypress.
				const size_t want = static_cast<size_t>(row.caret);
				ti->caret = HE::uiUtf8Clamp(ti->text, want > ti->text.size() ? ti->text.size() : want);
				ti->selAnchor = ti->caret;
				++landed;
			}
		if (row.hasScroll)
			if (float* off = e->scrollOffsetPtr()) { *off = row.scroll; ++landed; }
	}

	for (const auto& [key, elem] : snapshot.focus)
		if (Instance* w = findWidget(key))
			if (elem != 0 && w->tree.find(elem))
			{
				// Only a REAL focus counts. Restoring "nothing was focused" onto
				// a widget that already has nothing focused is not a piece of
				// state that survived, and counting it would keep `landed` above
				// zero for every widget — which is exactly the number the editor
				// uses to decide whether to say the state was lost.
				w->focusedElem = elem;
				++landed;
			}

	for (const auto& [key, values] : snapshot.vars)
	{
		Instance* w = findWidget(key);
		if (!w || w->scriptId == 0) continue;
		// What the REBUILT instance declares. Taken right after OnInit, this is
		// the graph's own variable set seeded from its defaults — so membership
		// in it is the only honest answer to "does this variable still exist".
		//
		// Asking getVariable instead does NOT work, and the difference is easy
		// to miss: an undeclared name reads back as a default-constructed Value,
		// whose type is Float. A Float variable that was renamed or deleted
		// therefore type-MATCHES that accidental default and would be recreated
		// in the store — a value nothing reads, nothing clears, and a later
		// rename could collide with. The type check below still earns its place
		// for a variable that stayed but changed type.
		const auto declared = rt().variablesSnapshot(w->scriptId);
		for (const auto& [name, value] : values)
		{
			const auto it = declared.find(name);
			if (it == declared.end()) continue;      // renamed or removed
			if (it->second.type != value.type) continue;
			rt().setVariable(w->scriptId, name, value);
			++landed;
		}
	}

	// Everything above changed what is on screen, and in an app nothing else
	// asks for a frame.
	if (landed > 0) m_visualDirty = true;
	return landed;
}

bool WidgetManager::selectAllFocused()
{
	return editFocusedText(TextEdit::SelectAll, false);
}

void WidgetManager::inputSubmit()
{
	if (m_focusWidget == 0) return;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti) return;
	const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);

	// In a MULTILINE field, Return is a new line and not a commit — decided
	// here, in the one place Return arrives, rather than by asking every host to
	// learn the difference. There is no key left to mean "done" once Enter means
	// "new line"; such a field commits when it loses focus (see UITextInput).
	if (ti->multiline)
	{
		if (!ti->editable) return;
		ti->deleteSelection();
		// Through the same filter every other character goes through: a field
		// that only accepts digits must not gain a newline through the one key
		// that skipped the check.
		if (!ti->acceptsCharacter("\n", ti->caret)) return;
		ti->text.insert(ti->caret, 1, '\n');
		ti->caret = ti->selAnchor = ti->caret + 1;
		ti->preferredCaretX = -1.0f;
		m_visualDirty = true;
		rt().fireOnTextChanged(t.scriptId, t.elem, ti->text);
		return;
	}
	rt().fireOnTextCommitted(t.scriptId, t.elem, ti->text);
}

void WidgetManager::activateElement(Instance& w, int elemId)
{
	HE::UIElement* e = w.tree.find(elemId);
	if (!e) return;
	const ScriptTarget target = scriptTargetFor(w, elemId);
	auto fireP = [&](void (HorizonCode::Runtime::*fn)(HorizonCode::InstanceId, int))
	{ (rt().*fn)(target.scriptId, target.elem); };

	switch (e->type())
	{
	case HE::UIWidgetType::Button:
		fireP(&HorizonCode::Runtime::fireOnClicked);
		fireP(&HorizonCode::Runtime::fireOnReleased);
		break;
	case HE::UIWidgetType::Panel:
	case HE::UIWidgetType::Image:
		fireP(&HorizonCode::Runtime::fireOnClicked);
		break;
	case HE::UIWidgetType::CheckBox:
		if (auto* cb = dynamic_cast<HE::UICheckBox*>(e))
		{
			cb->checked = !cb->checked;
			rt().fireOnCheckChanged(target.scriptId, target.elem, cb->checked);
		}
		break;
	case HE::UIWidgetType::ComboBox:
		// It OPENS. It used to advance to the next option, which was usable with
		// three entries, unusable with twenty, and is not what a dropdown means
		// anywhere else in computing. The open list belongs to the manager (see
		// the Dropdown grab) because it hangs outside this element's rect.
		if (auto* combo = dynamic_cast<HE::UIComboBox*>(e))
			if (!combo->options.empty() && !combo->open)
			{
				combo->open = true;
				combo->hoverIndex = combo->selectedIndex;
				Grab g;
				g.kind = Grab::Kind::Dropdown;
				g.widget = w.id;
				g.elem   = elemId;
				g.prevFocusWidget = m_focusWidget;
				if (const Instance* pf = find(m_focusWidget)) g.prevFocusElem = pf->focusedElem;
				m_grabs.push_back(g);
				m_visualDirty = true;
			}
		break;
	default:
		break;
	}
}

bool WidgetManager::isFocusable(const Instance& w, const HE::UIElement& e,
                                const HE::UIWidgetCanvas& canvas) const
{
	if (!isInteractive(w, e)) return false;
	if (!HE::uiElementEffectiveVisible(w.tree, e)) return false;
	if (!HE::uiElementEffectiveEnabled(w.tree, e)) return false;
	if (HE::uiElementEffectiveOpacity(w.tree, e) <= 0.001f) return false;
	// Scrolled out of its own list, say: not on screen, so not reachable.
	HE::UIWidgetRect clip{};
	if (HE::uiElementClipRect(w.tree, e, clip, &canvas))
	{
		const HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
		if (r.x + r.w <= clip.x || r.x >= clip.x + clip.w ||
		    r.y + r.h <= clip.y || r.y >= clip.y + clip.h) return false;
	}
	return true;
}

int WidgetManager::focusedElement() const
{
	const Instance* w = find(m_focusWidget);
	return w ? w->focusedElem : 0;
}

bool WidgetManager::setFocus(int widgetId, int elementId)
{
	// The focus ring is four quads in the frame, so moving it is a visual change
	// even when nothing else about the element is.
	m_visualDirty = true;
	Instance* w = find(widgetId);
	if (!w) return false;
	// Moving the focus always leaves editing behind: the keyboard belongs to
	// the field somebody confirmed into, and the moment the focus is somewhere
	// else that is nobody.
	m_focusEditing = false;
	// Focus events go to whichever script owns the element (see scriptTargetFor).
	auto fireFocus = [&](void (HorizonCode::Runtime::*fn)(HorizonCode::InstanceId, int), int elem)
	{
		const ScriptTarget t = scriptTargetFor(*w, elem);
		(rt().*fn)(t.scriptId, t.elem);
	};
	if (elementId == 0)
	{
		if (w->focusedElem != 0)
			fireFocus(&HorizonCode::Runtime::fireOnUnfocused, w->focusedElem);
		w->focusedElem = 0;
		if (m_focusWidget == widgetId) m_focusWidget = 0;
		return true;
	}
	const HE::UIElement* e = w->tree.find(elementId);
	if (!e) return false;
	if (w->focusedElem == elementId) return true;
	if (w->focusedElem != 0) fireFocus(&HorizonCode::Runtime::fireOnUnfocused, w->focusedElem);
	w->focusedElem = elementId;
	m_focusWidget  = widgetId;
	fireFocus(&HorizonCode::Runtime::fireOnFocused, elementId);
	return true;
}

bool WidgetManager::isEditingText() const
{
	return m_focusEditing && hasFocusedTextField();
}

bool WidgetManager::stopEditingText()
{
	if (!m_focusEditing) return false;
	m_focusEditing = false;
	m_visualDirty = true;   // the caret goes away
	return true;
}

bool WidgetManager::hasFocusedTextField() const
{
	const Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return false;
	const auto* ti = dynamic_cast<const HE::UITextInput*>(w->tree.find(w->focusedElem));
	// Editable OR selectable: a read-only field still takes the arrows to move
	// its selection and Ctrl+C to copy out of it, so it owns the keyboard just
	// as much as one being typed into. A field that is neither is inert and
	// keeps nothing.
	return ti && (ti->editable || ti->selectable);
}

bool WidgetManager::hasOpenDropdown() const
{
	return !m_grabs.empty() && m_grabs.back().kind == Grab::Kind::Dropdown;
}

HE::UIComboBox* WidgetManager::openDropdown(Instance** owner)
{
	if (!hasOpenDropdown()) return nullptr;
	Instance* w = find(m_grabs.back().widget);
	if (!w) return nullptr;
	auto* cb = dynamic_cast<HE::UIComboBox*>(w->tree.find(m_grabs.back().elem));
	if (cb && owner) *owner = w;
	return cb;
}

bool WidgetManager::activateFocused()
{
	m_visualDirty = true;   // a press changes the element's drawn state

	// With a list open, Enter takes the entry the arrows walked to — the same
	// thing releasing the button over it does, and the only thing the key can
	// mean while it hangs there. Before the focus is even looked at: the focus
	// is on the combo, and activating THAT would try to open a list that is
	// already open.
	if (Instance* dw = nullptr; HE::UIComboBox* cb = openDropdown(&dw))
	{
		const int pick = cb->hoverIndex;
		if (pick >= 0 && pick < static_cast<int>(cb->options.size()) &&
		    cb->selectedIndex != pick)
		{
			cb->selectedIndex = pick;
			const ScriptTarget t = scriptTargetFor(*dw, cb->id);
			rt().fireOnSelectionChanged(t.scriptId, t.elem, pick);
		}
		popGrab(/*notify=*/false);
		return true;
	}

	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return false;
	// A focused element that has since been disabled or hidden does nothing —
	// the same rule the pointer obeys.
	const HE::UIElement* e = w->tree.find(w->focusedElem);
	if (!e || !HE::uiElementEffectiveEnabled(w->tree, *e) ||
	    !HE::uiElementEffectiveVisible(w->tree, *e)) return false;
	// A text field starts being EDITED. Reaching one with Tab does not give it
	// the keyboard — this is the confirmation that does, and until it comes the
	// arrows and Tab still belong to the form, which is what stops the tab order
	// from dying in the first search box it walks into.
	if (const auto* ti = dynamic_cast<const HE::UITextInput*>(w->tree.find(w->focusedElem));
	    ti && (ti->editable || ti->selectable))
	{
		m_focusEditing = true;
		return true;
	}
	// A list opens its picked row rather than clicking itself. Deliberately NOT
	// in activateElement: that one also runs on every pointer release, and a
	// list where a single click both picks and opens is a list you cannot browse.
	if (auto* lv = dynamic_cast<HE::UIListView*>(w->tree.find(w->focusedElem)))
	{
		const int item = lv->firstSelected();
		if (item < 0) return false;
		const ScriptTarget t = scriptTargetFor(*w, lv->id);
		rt().fireOnRowActivated(t.scriptId, t.elem, item);
		return true;
	}
	activateElement(*w, w->focusedElem);
	return true;
}

bool WidgetManager::activateAtPointer(float vpWidth, float vpHeight,
                                      float mouseX, float mouseY)
{
	syncLists();
	std::vector<Instance*> sorted;
	for (auto& w : m_instances) if (w.visible) sorted.push_back(&w);
	std::stable_sort(sorted.begin(), sorted.end(),
		[](const Instance* a, const Instance* b){ return a->zOrder > b->zOrder; });

	for (Instance* wp : sorted)
	{
		Instance& w = *wp;
		if (!takesInput(w.id)) continue;
		const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
		for (auto& ep : w.tree.elements)
		{
			auto* lv = dynamic_cast<HE::UIListView*>(ep.get());
			if (!lv) continue;
			if (!HE::uiElementEffectiveVisible(w.tree, *lv)) continue;
			if (!HE::uiElementEffectiveEnabled(w.tree, *lv)) continue;
			const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *lv, &canvas);
			if (mouseX < r.x * canvas.scaleX || mouseX > (r.x + r.w) * canvas.scaleX ||
			    mouseY < r.y * canvas.scaleY || mouseY > (r.y + r.h) * canvas.scaleY)
				continue;
			const int item = listRowAtPointer(w.tree, *lv, canvas, mouseY);
			if (item < 0) continue;
			const ScriptTarget t = scriptTargetFor(w, lv->id);
			rt().fireOnRowActivated(t.scriptId, t.elem, item);
			return true;
		}
	}
	return false;
}

bool WidgetManager::navigate(NavDir dir, float vpWidth, float vpHeight)
{
	m_visualDirty = true;   // the focus ring moves

	// ── The layer on top decides what the arrows MEAN ────────────────────────
	// A dropdown hanging open takes up and down for its own entries. Moving the
	// focus to the next button instead would be answering a question nobody
	// asked — and that button is not reachable anyway, since takesInput has
	// already given the whole input to this widget. Left and right belong to
	// nobody here, so they are refused rather than falling through to the
	// spatial walk underneath the open list.
	if (Instance* dw = nullptr; HE::UIComboBox* cb = openDropdown(&dw))
	{
		const int count = static_cast<int>(cb->options.size());
		if (count == 0 || (dir != NavDir::Up && dir != NavDir::Down)) return false;
		// Clamped, not wrapped: running off the end of a list and reappearing at
		// the other one is a jump you have to watch to understand.
		const int cur = (cb->hoverIndex >= 0 && cb->hoverIndex < count)
			? std::clamp(cb->hoverIndex + (dir == NavDir::Down ? 1 : -1), 0, count - 1)
			: std::clamp(cb->selectedIndex, 0, count - 1);
		if (cur == cb->hoverIndex) return false;   // already at that end
		cb->hoverIndex = cur;
		return true;
	}
	// The widget the focus is in, else the topmost visible one that has
	// anything focusable at all.
	Instance* w = find(m_focusWidget);
	if (!w || !w->visible || !takesInput(w->id))
	{
		// With a layer up there is exactly one candidate, and it is the layer.
		// This is the focus TRAP: without it, Tab and the arrows walk out of an
		// open dialog into the page it is covering.
		if (!m_grabs.empty()) { w = find(m_grabs.back().widget); if (!w) return false; }
		else
		{
			std::vector<Instance*> sorted;
			for (auto& inst : m_instances) if (inst.visible) sorted.push_back(&inst);
			std::stable_sort(sorted.begin(), sorted.end(),
				[](const Instance* a, const Instance* b){ return a->zOrder > b->zOrder; });
			w = sorted.empty() ? nullptr : sorted.front();
			if (!w) return false;
		}
	}
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);

	// A focused list takes up/down as a step through its ITEMS. Falling through
	// at either end is the point: Down on the last row hands the focus to
	// whatever is below the list, so a list is not a place the keyboard gets
	// stuck in.
	if (w->focusedElem != 0 && (dir == NavDir::Up || dir == NavDir::Down))
		if (auto* lv = dynamic_cast<HE::UIListView*>(w->tree.find(w->focusedElem));
		    lv && lv->itemCount > 0 && lv->selectionMode != 0)
		{
			const int cur  = lv->firstSelected();
			const int next = cur < 0
				? (dir == NavDir::Down ? 0 : lv->itemCount - 1)
				: cur + (dir == NavDir::Down ? 1 : -1);
			if (next >= 0 && next < lv->itemCount)
			{
				lv->clearSelection();
				lv->setSelected(next, true);
				const ScriptTarget t = scriptTargetFor(*w, lv->id);
				rt().fireOnSelectionChanged(t.scriptId, t.elem, next);
				// The row it just picked has to be on screen, or stepping past
				// the bottom edge would move a highlight nobody can see.
				lv->scrollToItem(next);
				syncLists(*w);
				return true;
			}
		}

	// A focused slider takes left/right as a value step instead of handing the
	// focus on — that is what those keys mean while a slider has the focus.
	if (w->focusedElem != 0 && (dir == NavDir::Left || dir == NavDir::Right))
		if (auto* s = dynamic_cast<HE::UISlider*>(w->tree.find(w->focusedElem)))
		{
			const float span = s->maxValue - s->minValue;
			if (span > 0.0f)
			{
				const float step = span * 0.05f;
				const float nv = std::clamp(s->value + (dir == NavDir::Right ? step : -step),
				                            s->minValue, s->maxValue);
				if (nv != s->value)
				{
					s->value = nv;
					const ScriptTarget t = scriptTargetFor(*w, w->focusedElem);
					rt().fireOnValueChanged(t.scriptId, t.elem, nv);
					return true;
				}
			}
			return false;
		}

	auto centre = [&](const HE::UIElement& e, float& cx, float& cy)
	{
		const HE::UIWidgetRect r = HE::uiElementRect(w->tree, e, &canvas);
		cx = r.x + r.w * 0.5f; cy = r.y + r.h * 0.5f;
	};

	const HE::UIElement* from = w->focusedElem != 0 ? w->tree.find(w->focusedElem) : nullptr;
	float fx = 0.0f, fy = 0.0f;
	if (from) centre(*from, fx, fy);

	int   best = 0;
	float bestCost = 0.0f;
	for (const auto& ep : w->tree.elements)
	{
		const HE::UIElement& e = *ep;
		if (e.id == w->focusedElem) continue;
		if (!isFocusable(*w, e, canvas)) continue;

		float cx = 0.0f, cy = 0.0f;
		centre(e, cx, cy);
		float cost = 0.0f;
		if (!from)
		{
			// Nothing focused yet: take the top-most, then left-most candidate,
			// which is where a menu expects to start.
			cost = cy * 10000.0f + cx;
		}
		else
		{
			const float dx = cx - fx, dy = cy - fy;
			// Along the direction, and how far off to the side. The lateral
			// term is weighted so a candidate straight ahead beats a nearer one
			// that is well off to the side — that is what makes a grid feel
			// like a grid.
			float along = 0.0f, lateral = 0.0f;
			switch (dir)
			{
			case NavDir::Up:    along = -dy; lateral = std::fabs(dx); break;
			case NavDir::Down:  along =  dy; lateral = std::fabs(dx); break;
			case NavDir::Left:  along = -dx; lateral = std::fabs(dy); break;
			case NavDir::Right: along =  dx; lateral = std::fabs(dy); break;
			}
			if (along <= 0.5f) continue;   // not in this direction at all
			cost = along + lateral * 2.0f;
		}
		if (best == 0 || cost < bestCost) { best = e.id; bestCost = cost; }
	}
	if (best == 0) return false;
	return setFocus(w->id, best);
}

bool WidgetManager::focusNext(bool backwards, float vpWidth, float vpHeight)
{
	m_visualDirty = true;

	// An open list takes Tab too, and steps through its ENTRIES with it. The
	// layer on top decides what a key means, and while a list hangs there the
	// only things a keyboard can reach are its rows: Tab used to close it and
	// move on, which walked out of a list somebody had just opened. Escape is
	// how you leave without choosing and Enter is how you take one; Tab is
	// simply the down arrow of a form.
	if (hasOpenDropdown())
		return navigate(backwards ? NavDir::Up : NavDir::Down, vpWidth, vpHeight);

	// Whose form is being tabbed through: the layer, else the widget that has
	// the focus, else the topmost visible one. Same three answers navigate()
	// gives, and for the same reason — a Tab must not walk out of a dialog.
	Instance* w = nullptr;
	if (!m_grabs.empty()) w = find(m_grabs.back().widget);
	if (!w) w = find(m_focusWidget);
	if (!w || !w->visible || !takesInput(w->id))
	{
		std::vector<Instance*> sorted;
		for (auto& inst : m_instances) if (inst.visible && takesInput(inst.id))
			sorted.push_back(&inst);
		std::stable_sort(sorted.begin(), sorted.end(),
			[](const Instance* a, const Instance* b){ return a->zOrder > b->zOrder; });
		w = sorted.empty() ? nullptr : sorted.front();
	}
	if (!w) return false;

	// Depth first through the HIERARCHY, parents before their children — the
	// order the designer's tree shows, which is the order an author arranges
	// and the only one they can predict. Not the element vector: that is keyed
	// by id, so it is creation order, and re-parenting a row would leave the
	// tab order where the row used to be.
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);
	std::vector<int> order;
	const std::function<void(int)> walk = [&](int parent)
	{
		for (int child : w->tree.childrenOf(parent))
		{
			if (const HE::UIElement* e = w->tree.find(child))
				if (isFocusable(*w, *e, canvas)) order.push_back(child);
			walk(child);
		}
	};
	walk(0);
	if (order.empty()) return false;

	// Where we are now. An element that is no longer focusable (hidden, or
	// scrolled out of its list) is not in the list, so Tab starts from the
	// beginning rather than from nowhere.
	int at = -1;
	for (int i = 0; i < static_cast<int>(order.size()); ++i)
		if (order[i] == w->focusedElem) { at = i; break; }

	const int n = static_cast<int>(order.size());
	// Wrapping is right here where it was wrong in the dropdown: a form is a
	// ring you cycle through, a list is a range you run along.
	const int next = at < 0 ? (backwards ? n - 1 : 0)
	                        : ((at + (backwards ? -1 : 1)) % n + n) % n;
	return setFocus(w->id, order[next]);
}

bool WidgetManager::processWheel(float vpWidth, float vpHeight,
                                 float mouseX, float mouseY, float wheel)
{
	if (wheel == 0.0f) return false;
	syncLists();            // a list's rows decide how far it can scroll at all
	m_visualDirty = true;   // a scrolled box moves everything inside it
	// One notch moves this many canvas units — the same order as a text line,
	// so a list of buttons steps rather than jumps.
	constexpr float kUnitsPerNotch = 48.0f;

	// Topmost widget first, and within it the DEEPEST scroll box under the
	// cursor: a list inside a list scrolls the one the pointer is in.
	std::vector<Instance*> sorted;
	for (auto& w : m_instances) if (w.visible) sorted.push_back(&w);
	std::stable_sort(sorted.begin(), sorted.end(),
		[](const Instance* a, const Instance* b){ return a->zOrder > b->zOrder; });

	for (Instance* wp : sorted)
	{
		Instance& w = *wp;
		// Same question the pointer asks: while a dialog is up, the list behind
		// it does not scroll either.
		if (!takesInput(w.id)) continue;
		const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
		HE::uiUpdateScrollExtents(w.tree);

		int   best = 0;
		int   bestDepth = -1;
		for (auto& ep : w.tree.elements)
		{
			HE::UIElement& e = *ep;
			// Whatever says it scrolls — a scroll box or a list view. Naming the
			// types here is how the wheel and the layout drift apart.
			if (!e.scrollOffsetPtr()) continue;
			if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
			if (!HE::uiElementEffectiveEnabled(w.tree, e)) continue;
			const HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
			const float x0 = r.x * canvas.scaleX, y0 = r.y * canvas.scaleY;
			const float x1 = (r.x + r.w) * canvas.scaleX, y1 = (r.y + r.h) * canvas.scaleY;
			if (mouseX < x0 || mouseX > x1 || mouseY < y0 || mouseY > y1) continue;
			// Depth = how far down the tree, so the innermost box wins.
			int depth = 0, guard = 0;
			for (const HE::UIElement* c = &e;
			     c->parentId != 0 && guard++ < static_cast<int>(w.tree.elements.size()) + 1;)
			{
				const HE::UIElement* p = w.tree.find(c->parentId);
				if (!p) break;
				++depth; c = p;
			}
			if (depth > bestDepth) { bestDepth = depth; best = e.id; }
		}
		if (best != 0 && HE::uiScrollBy(w.tree, best, -wheel * kUnitsPerNotch))
			return true;
	}
	return false;
}

void WidgetManager::extract(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out)
{
	m_lastViewportW = vpWidth; m_lastViewportH = vpHeight;
	// Every list's rows, for the size the view has NOW: a window that just grew
	// shows more rows in the frame it grew in, not in the one after it.
	syncLists();

	// Widgets sorted by zOrder (stable: creation order breaks ties).
	std::vector<Instance*> sorted;
	sorted.reserve(m_instances.size());
	for (auto& w : m_instances)
		if (w.visible) sorted.push_back(&w);
	std::stable_sort(sorted.begin(), sorted.end(),
		[](const Instance* a, const Instance* b){ return a->zOrder < b->zOrder; });

	// Which widget the scrim goes under: the LOWEST modal, so a dialog over a
	// dialog dims the world once rather than twice as dark. It is emitted by
	// this class and not by any element, because it belongs to no widget — it is
	// what everything else is behind. Which is also why pointerOverUI has to
	// answer for it by hand: there is no element there to be hit.
	int lowestModal = 0;
	for (const Grab& g : m_grabs)
		if (g.kind == Grab::Kind::Modal) { lowestModal = g.widget; break; }

	for (Instance* wp : sorted)
	{
		// Appended immediately before the widget that asked for it, so whatever
		// was drawn earlier is behind the dim and the dialog is in front of it.
		if (lowestModal != 0 && wp->id == lowestModal)
		{
			UIRenderObject dim;
			dim.position = { 0.0f, 0.0f };
			dim.size     = { vpWidth, vpHeight };
			dim.color    = kModalScrim;
			out.push_back(dim);
		}
		Instance& w = *wp;
		// The widget's scale mode decides how the authored canvas meets this
		// viewport — and how many canvas units the screen is worth. Everything
		// below (layout, auto-size wrap column, the pixel conversion) has to go
		// through the SAME resolution, or the picture and the hit test drift.
		const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
		const float sx = canvas.scaleX;
		const float sy = canvas.scaleY;

		// Auto-sizing elements fit themselves BEFORE the rects are resolved, so a
		// text/font change made this frame (script, HorizonCode Set Property) is
		// already reflected in the layout below.
		HE::uiApplyAutoSize(w.tree, &canvas);
		// Scroll boxes measure their content after auto-size (a text that grew
		// changes it) and before any rect is asked for.
		HE::uiUpdateScrollExtents(w.tree);

		// Draw elements of this widget, painter-ordered by (layer, depth).
		struct Item { const HE::UIElement* e; int key; HE::UIWidgetRect r; };
		std::vector<Item> items;
		for (const auto& ep : w.tree.elements)
		{
			const HE::UIElement& e = *ep;
			if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
			items.push_back({ &e, elementSortKey(w.tree, e),
			                  HE::uiElementRect(w.tree, e, &canvas) });
		}
		std::stable_sort(items.begin(), items.end(),
			[](const Item& a, const Item& b){ return a.key < b.key; });

		for (const Item& it : items)
		{
			const HE::UIElement& e = *it.e;

			// Rect in pixels.
			HE::UIWidgetRect px;
			px.x = it.r.x * sx; px.y = it.r.y * sy;
			px.w = it.r.w * sx; px.h = it.r.h * sy;

			// Transient interaction state.
			HE::UIElementRenderState st;
			st.hovered = (e.id == w.hoveredElem);
			st.pressed = (e.id == w.pressedElem && m_wasDown);
			st.focused = (e.id == w.focusedElem);
			st.editing = st.focused && m_focusEditing && m_focusWidget == w.id;
			st.dropTarget = (e.id == m_dropElem && m_dropWidget == w.id);

			const auto matIt = w.materials.find(e.id);
			const HE::UUID matId = matIt != w.materials.end() ? matIt->second : HE::UUID{};

			// Cut off by a clipping ancestor? An element with nothing left of it
			// is dropped here rather than handed to the backend to scissor
			// away — that covers both an empty clip rect (clippers that do not
			// overlap each other) and a rect that simply misses this element,
			// which is every row of a list that is scrolled out of view.
			HE::UIWidgetRect clip{};
			const bool clipped = HE::uiElementClipRect(w.tree, e, clip, &canvas);
			if (clipped)
			{
				if (clip.w <= 0.0f || clip.h <= 0.0f) continue;
				if (it.r.x + it.r.w <= clip.x || it.r.x >= clip.x + clip.w ||
				    it.r.y + it.r.h <= clip.y || it.r.y >= clip.y + clip.h) continue;
			}

			// The element draws itself (quads + glyphs) into `out`. The scale
			// handed over turns one of THIS element's units into a pixel, so an
			// embedded widget's factor belongs in it: its rect is already
			// scaled, and a font size that is not would come out 1/factor too
			// big — the one part of an element that is not a rectangle.
			float eus = 1.0f, evs = 1.0f;
			HE::uiElementUnitScale(w.tree, e, eus, evs, &canvas);
			// ── The drop shadow, emitted BEFORE the element ──────────────────
			// It is the element's own shape once more, in one colour, offset and
			// softened — a quad like any other, which is why "Schicht 0" needs
			// no blur pass and works in every backend.
			//
			// Before render() so it lands UNDER this element's own quads and
			// over everything drawn earlier, which is where a shadow belongs;
			// the vector is appended to, never inserted into.
			//
			// Grown by the blur on every side: the falloff reaches that far past
			// the shape, and a quad cut off at the shape's edge would show the
			// shadow as a hard line. The shader measures the shape against a box
			// inset by exactly that much (see UIRenderObject::blur).
			const size_t emitStart = out.size();
			if (e.shadow && e.hasSurfaceStyle() && e.shadowColor.a > 0.001f)
			{
				const float blurPx = std::max(0.0f, e.shadowBlur * sy * evs);
				UIRenderObject sh;
				// The offset is a length on each axis and takes that axis's
				// factor; the blur is one number and follows the radius.
				sh.position = { px.x + e.shadowOffsetX * sx * eus - blurPx,
				                px.y + e.shadowOffsetY * sy * evs - blurPx };
				sh.size     = { px.w + 2.0f * blurPx, px.h + 2.0f * blurPx };
				sh.color    = e.shadowColor;
				sh.type     = 0;
				sh.cornerRadius = e.cornerRadius * (sy * evs);
				sh.blur     = blurPx;
				out.push_back(std::move(sh));
			}
			const size_t firstQuad = out.size();
			e.render(px, st, matId, sy * evs, out);

			// ── The border, stamped onto the element's SURFACE ────────────────
			// Only the FIRST quad an element emits, and only when it covers the
			// element's whole rect. That is the background — the surface the
			// border belongs to. Stamping every quad would outline a progress
			// bar's fill as well as its track, and a slider's handle as well as
			// its groove; testing the rect is what tells a background apart from
			// a part drawn on top of one, without any widget type knowing that
			// borders exist.
			if ((e.maxCornerRadius() > 0.0f || e.borderWidth > 0.0f || e.gradient ||
			     e.innerShadow) && out.size() > firstQuad)
			{
				UIRenderObject& first = out[firstQuad];
				const bool coversRect =
					first.type == 0 &&
					std::abs(first.position.x - px.x) < 0.5f &&
					std::abs(first.position.y - px.y) < 0.5f &&
					std::abs(first.size.x - px.w)     < 0.5f &&
					std::abs(first.size.y - px.h)     < 0.5f;
				if (coversRect)
				{
					// A length, so it scales with the canvas — and clamped the
					// way the shaders clamp it, so a radius larger than the box
					// is a capsule rather than a mistake.
					// All four take the SAME factor: a radius is a length along
					// the shorter way round its corner, and giving x and y their
					// own scale would turn a circle into an ellipse the shaders
					// cannot draw.
					if (e.maxCornerRadius() > 0.0f)
						first.cornerRadius = e.cornerRadius * (sy * evs);
					if (e.borderWidth > 0.0f)
					{
						// In pixels like every other length here, so a scaled
						// canvas scales the line with the box it outlines.
						first.borderWidth = e.borderWidth * sy * evs;
						first.borderColor = e.borderColor;
					}
					if (e.gradient)
					{
						// The angle is not a length, so it does NOT scale.
						first.gradient         = true;
						first.gradientColor    = e.gradientColor;
						first.gradientAngleDeg = e.gradientAngle;
						first.gradientShape    = e.gradientShape;
					}
					// The inner shadow rides on the surface itself, because it
					// has to be cut off by the surface's own shape — a second
					// quad could not be.
					if (e.innerShadow && e.innerShadowColor.a > 0.001f)
					{
						first.innerShadowBlur  = std::max(0.0f, e.innerShadowBlur)
						                       * sy * evs;
						first.innerShadowColor = e.innerShadowColor;
					}
				}
			}

			// Inherited opacity and the disabled dim, applied to whatever the
			// element emitted — same reason as the clip below: a widget type
			// with five quads gets both right without knowing they exist.
			// Multiplied, never assigned: an element's own colours keep their
			// alpha, they are only faded further.
			const float alpha = HE::uiElementEffectiveOpacity(w.tree, e);
			const bool  usable = HE::uiElementEffectiveEnabled(w.tree, e);
			if (alpha < 1.0f || !usable)
			{
				const float dim = usable ? 1.0f : HE::kUIDisabledDim;
				for (size_t i = emitStart; i < out.size(); ++i)
				{
					out[i].color.r *= dim;
					out[i].color.g *= dim;
					out[i].color.b *= dim;
					out[i].color.a *= alpha;
				}
			}

			// Every quad the element just emitted inherits the clip. Stamped
			// here rather than passed into render(), so no widget type has to
			// know clipping exists — a type that emits five quads gets it right
			// by construction.
			if (clipped)
			{
				const glm::vec4 r(clip.x * sx, clip.y * sy,
				                  std::max(clip.w * sx, 0.0f), std::max(clip.h * sy, 0.0f));
				for (size_t i = emitStart; i < out.size(); ++i) out[i].clipRect = r;
			}

			// Rotation, folded down the chain: the quads are shifted so the
			// element's own pivot lands where its ancestors' rotations carried
			// it, and then everything turns about that point. Stamped like the
			// clip above, so no widget type has to know rotation exists.
			{
				HE::UIRotation rot;
				if (HE::uiElementRotation(w.tree, e, rot, &canvas))
				{
					const float shiftX = (rot.dstX - rot.srcX) * sx;
					const float shiftY = (rot.dstY - rot.srcY) * sy;
					const float rad = rot.degrees * 3.14159265358979323846f / 180.0f;
					const glm::vec2 pivot(rot.dstX * sx, rot.dstY * sy);
					for (size_t i = emitStart; i < out.size(); ++i)
					{
						out[i].position.x += shiftX;
						out[i].position.y += shiftY;
						out[i].rotation      = rad;
						out[i].rotationPivot = pivot;
					}
				}
			}

			// Focus ring: ONE outlined rectangle around the element the keyboard
			// or gamepad is on. Drawn here rather than by the widget types
			// because every type needs it and none of them should have to know.
			//
			// It follows the element's OWN corners. It used to be four
			// hairlines, and four rectangles can only draw a square: around a
			// rounded search field that put a square ring with its corners
			// sticking out past the curve, which reads as a drawing mistake
			// rather than as focus. No fill and a border — the shaders blend the
			// border over the fill, so a transparent fill leaves the outline.
			//
			// Two things ask for one, so the ring itself is written once: the
			// keyboard is on this element, or something is being dragged over
			// it. They differ in WHICH element they hang on and in colour, and
			// in nothing else.
			auto emitRing = [&](const HE::UIElement& ringOn, const glm::vec4& colour)
			{
				constexpr float kRing = 2.0f;   // pixels
				const size_t ringFirst = out.size();
				HE::UIWidgetRect rpx{ px.x, px.y, px.w, px.h };
				float rus = eus, rvs = evs;
				if (&ringOn != &e)
				{
					const HE::UIWidgetRect rr = HE::uiElementRect(w.tree, ringOn, &canvas);
					rpx = { rr.x * sx, rr.y * sy, rr.w * sx, rr.h * sy };
					HE::uiElementUnitScale(w.tree, ringOn, rus, rvs, &canvas);
				}
				// ON the element's edge, not around it. Outside would be the
				// prettier ring and it is the one that disappears: a text field
				// clips its own glyphs to its rect (uiElementClipRect), a list
				// row is clipped by its box, and a ring drawn two pixels beyond
				// the edge is two pixels of nothing but clipped-away band. It
				// also cannot bleed into whatever sits next to it.
				UIRenderObject ro;
				ro.position = { rpx.x, rpx.y };
				ro.size     = { rpx.w, rpx.h };
				ro.color    = glm::vec4(0.0f);
				ro.cornerRadius = ringOn.maxCornerRadius() > 0.0f
					? ringOn.cornerRadius * (sy * rvs) : glm::vec4(0.0f);
				ro.borderWidth  = kRing;
				ro.borderColor  = colour;
				out.push_back(std::move(ro));
				// Clipped by whatever clips the element the ring is drawn ON —
				// which for a focus frame is NOT the focused element. The field
				// clips itself; the frame around it does not, and stamping the
				// field's clip onto a ring that belongs to the frame is how a
				// ring ends up cut away to nothing.
				HE::UIWidgetRect rclip{};
				if (HE::uiElementClipRect(w.tree, ringOn, rclip, &canvas))
				{
					const glm::vec4 r(rclip.x * sx, rclip.y * sy,
					                  std::max(rclip.w * sx, 0.0f),
					                  std::max(rclip.h * sy, 0.0f));
					for (size_t i = ringFirst; i < out.size(); ++i) out[i].clipRect = r;
				}
			};
			if (st.focused && m_focusWidget == w.id)
			{
				// Around the element, or around the FRAME that says it is the
				// control (UIElement::focusFrame). A search field is a rounded
				// panel with an icon and an inset text field inside it: the
				// field takes the keyboard, but the thing a person sees is the
				// panel, and a ring around the field is a small rectangle
				// floating inside a pill.
				const HE::UIElement* ringOn = &e;
				for (int p = e.parentId; p != 0; )
				{
					const HE::UIElement* anc = w.tree.find(p);
					if (!anc) break;
					if (anc->focusFrame) { ringOn = anc; break; }
					p = anc->parentId;
				}
				emitRing(*ringOn, glm::vec4(1.0f, 0.78f, 0.25f, 0.95f));
			}
			// The drop zone, in a colour of its own — "this is where it lands"
			// and "this is where the keys go" are two different promises, and a
			// person dragging a file has to be able to tell them apart at a
			// glance. On the ACCEPTING element itself: it is the one that said
			// it takes drops, so it is the one the drop belongs to.
			if (st.dropTarget)
				emitRing(e, glm::vec4(0.35f, 0.80f, 1.0f, 0.95f));
		}
	}

	// ── The two things that are drawn OVER everything ────────────────────────
	// An open dropdown and a tooltip both hang outside the element they belong
	// to, and both have to be on top of every widget rather than of their own.
	// That makes them the manager's to draw, exactly like the scrim and the
	// focus ring — appended after the loop, so they land last.
	drawOpenDropdown(vpWidth, vpHeight, out);
	drawTooltip(vpWidth, vpHeight, out);
}

// The list a ComboBox drops down. Its rows come from `comboListRect`, the same
// arithmetic the pointer uses to decide which one it is over — one source, or
// the picture and the click disagree by exactly one row and nobody can say why.
void WidgetManager::drawOpenDropdown(float vpWidth, float vpHeight,
                                     std::vector<UIRenderObject>& out)
{
	if (m_grabs.empty() || m_grabs.back().kind != Grab::Kind::Dropdown) return;
	const Grab g = m_grabs.back();
	Instance* w = find(g.widget);
	auto* cb = w ? dynamic_cast<HE::UIComboBox*>(w->tree.find(g.elem)) : nullptr;
	if (!w || !cb || cb->options.empty()) return;

	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);
	const HE::UIWidgetRect r = comboListRect(w->tree, *cb, &canvas);
	const float sx = canvas.scaleX, sy = canvas.scaleY;
	float eus = 1.0f, evs = 1.0f;
	HE::uiElementUnitScale(w->tree, *cb, eus, evs, &canvas);

	const float x = r.x * sx, y = r.y * sy, wid = r.w * sx, hei = r.h * sy;
	const float rowH = hei / static_cast<float>(cb->options.size());

	// How round the card may be — NOT simply as round as the box. See
	// UIComboBox::listRadius: a corner deeper than half a row is a corner eating
	// a row, and the first and last entries lose their outer half to it.
	const float rad = HE::UIComboBox::listRadius(
		cb->maxCornerRadius() * (sy * evs), rowH, hei);
	// …and the inset follows that rounding, so no label crosses the curve.
	const float pad = HE::UIComboBox::contentInset(rad);

	UIRenderObject bg;
	bg.position = { x, y };
	bg.size     = { wid, hei };
	bg.color    = cb->backColor;
	bg.cornerRadius = glm::vec4(rad);
	bg.borderWidth  = std::max(1.0f, cb->borderWidth * sy * evs);
	bg.borderColor  = cb->borderWidth > 0.0f ? cb->borderColor
	                                         : glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
	out.push_back(bg);

	const std::size_t last = cb->options.size() - 1;
	for (std::size_t i = 0; i < cb->options.size(); ++i)
	{
		const float ry = y + rowH * static_cast<float>(i);
		const bool  hot = static_cast<int>(i) == cb->hoverIndex;
		const bool  sel = static_cast<int>(i) == cb->selectedIndex;
		if (hot || sel)
		{
			UIRenderObject row;
			row.position = { x, ry };
			row.size     = { wid, rowH };
			// The first and last rows take the CARD'S corners, each on its own
			// side — that is what the four-radius vocabulary is for. A row
			// rounded uniformly (or not at all) either pokes out of the card at
			// the top and bottom or leaves a square shoulder inside a round one.
			row.cornerRadius = glm::vec4(i == 0    ? rad : 0.0f,
			                             i == 0    ? rad : 0.0f,
			                             i == last ? rad : 0.0f,
			                             i == last ? rad : 0.0f);
			// The one under the pointer is the brighter of the two: it is where
			// the click would go, and that is the more useful thing to see.
			row.color    = hot ? cb->highlightColor
			                   : glm::vec4(glm::vec3(cb->highlightColor), 0.45f);
			out.push_back(row);
		}
		HE::UITextLayout opts;
		opts.alignV = 1;   // centred in its row
		HE::emitUITextGlyphs(HE::sharedUIFont(), 0, cb->options[i],
		                     { x + pad, ry }, { wid - 2.0f * pad, rowH },
		                     cb->fontSize * sy * evs, cb->textColor, 0, opts, out);
	}
}

// What the element under the pointer says about itself, after the wait. Placed
// beside the pointer and pushed back inside the screen — a hint that runs off
// the edge is the one case where it was needed most.
void WidgetManager::drawTooltip(float vpWidth, float vpHeight,
                                std::vector<UIRenderObject>& out)
{
	if (!m_tooltipUp || m_tooltipElem == 0) return;
	const Instance* w = find(m_tooltipWidget);
	const HE::UIElement* e = w ? w->tree.find(m_tooltipElem) : nullptr;
	if (!e || e->tooltip.empty()) return;
	// A tooltip belongs to something you can still see and still use.
	if (!HE::uiElementEffectiveVisible(w->tree, *e)) return;

	constexpr float kFontPx = 14.0f;
	constexpr float kPad    = 6.0f;
	const glm::vec2 text = HE::measureUIText(HE::sharedUIFont(), e->tooltip, kFontPx,
	                                         0.0f, HE::UITextLayout{});
	const float bw = text.x + 2.0f * kPad;
	const float bh = text.y + 2.0f * kPad;
	// Below and to the right of the pointer, which is where it does not sit
	// under the cursor itself; flipped when that would leave the screen.
	float bx = m_pointerX + 14.0f;
	float by = m_pointerY + 18.0f;
	if (bx + bw > vpWidth)  bx = std::max(0.0f, m_pointerX - bw - 6.0f);
	if (by + bh > vpHeight) by = std::max(0.0f, m_pointerY - bh - 6.0f);

	UIRenderObject bg;
	bg.position = { bx, by };
	bg.size     = { bw, bh };
	bg.color    = { 0.08f, 0.08f, 0.10f, 0.96f };
	bg.cornerRadius = glm::vec4(4.0f);
	bg.borderWidth  = 1.0f;
	bg.borderColor  = { 0.45f, 0.45f, 0.52f, 0.9f };
	out.push_back(bg);

	HE::UITextLayout opts;
	opts.alignV = 1;
	HE::emitUITextGlyphs(HE::sharedUIFont(), 0, e->tooltip,
	                     { bx + kPad, by }, { bw - 2.0f * kPad, bh },
	                     kFontPx, glm::vec4(0.95f, 0.95f, 0.97f, 1.0f), 0, opts, out);
}
