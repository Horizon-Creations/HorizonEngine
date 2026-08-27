#include <HorizonScene/WidgetManager.h>
#include <HorizonScene/UISystem.h>   // sortKey — one painter-order rule for both UI paths
#include <HorizonCode/HcCompiledLoader.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Renderer/UIFont.h>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <cmath>      // std::abs — the border stamp compares rects

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
	// "Self" for an EMBEDDED widget is that widget, not the whole page it sits
	// on: showing itself shows its WidgetRef element, and nothing around it.
	auto setSelfVisible = [this](HorizonCode::InstanceId id, bool vis)
	{
		int off = 0;
		Instance* w = resolveScriptOwner(id, off);
		if (!w) return;
		m_visualDirty = true;
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
                                    std::vector<std::string>& chain, int depth)
{
	// A widget that embeds itself, directly or around a circle, would expand
	// forever. Both guards are cheap and both are needed: the chain catches the
	// circle, the depth catches an honest but absurd nesting.
	constexpr int kMaxDepth = 8;
	if (depth > kMaxDepth) return;

	// Snapshot the ids to visit: the loop appends to tree.elements, and a ref
	// grafted in this pass is expanded by the recursive call, not by this loop.
	std::vector<int> refIds;
	for (const auto& ep : w.tree.elements)
		if (ep && ep->type() == HE::UIWidgetType::WidgetRef) refIds.push_back(ep->id);

	for (const int refId : refIds)
	{
		auto* ref = dynamic_cast<HE::UIWidgetRef*>(w.tree.find(refId));
		if (!ref || ref->embedded || ref->widgetPath.empty()) continue;

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

		w.embeds.push_back(em);
		ref->embedded = true;
		// What its elements measure in, and by which rule that meets this slot.
		// Without it a widget authored for 1920x1080 dropped into a 400x300 slot
		// would keep its absolute offsets and hang out of its own frame.
		ref->contentW    = sub.canvasWidth;
		ref->contentH    = sub.canvasHeight;
		ref->contentMode = sub.scaleMode;

		// …and the widget it just brought in may embed further widgets.
		chain.push_back(ref->widgetPath);
		embedWidgetRefs(w, content, chain, depth + 1);
		chain.pop_back();
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
		std::vector<std::string> chain{ assetPath };
		embedWidgetRefs(w, content, chain, 0);
	}

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
	Instance& stored = m_instances.back();
	HE_LOG_INFO(Widget, "Created widget '%s' (id %d, %zu element(s), %s logic)",
	            assetPath.c_str(), stored.id, stored.tree.elements.size(),
	            graph.nodes.empty() ? "compiled/no" : "interpreted");
	rt().fireConstruct(stored.scriptId);
	// Embedded widgets construct too, innermost last — an embed may only be
	// spoken to once the widget holding it has run its own Construct.
	for (const Instance::Embed& em : stored.embeds)
		rt().fireConstruct(em.scriptId);
	m_visualDirty = true;
	return stored.id;
}

void WidgetManager::destroyWidget(int id)
{
	m_visualDirty = true;
	if (m_focusWidget == id) m_focusWidget = 0;
	if (Instance* w = find(id))
	{
		HE_LOG_DEBUG(Widget, "Destroying widget id %d", id);
		// Embedded widgets are instances of their own and have to go with it,
		// innermost first — otherwise their scripts outlive the tree they act on.
		for (auto it = w->embeds.rbegin(); it != w->embeds.rend(); ++it)
			rt().destroy(it->scriptId);
		rt().destroy(w->scriptId); // fire "Destruct", then drop it
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
void WidgetManager::hideWidget(int id)  { if (Instance* w = find(id)) { w->visible = false; m_visualDirty = true; } }
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

void WidgetManager::tick(float dt)
{
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

bool WidgetManager::processPointer(float vpWidth, float vpHeight,
                                   float mouseX, float mouseY,
                                   bool primaryDown, bool valid)
{
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
	Instance* topW = nullptr;
	int  topElem = 0;
	long topKey  = 0;
	bool topActs = false;      // does the winner actually take events?
	HE::UICursor topCursor = HE::UICursor::Default;
	if (valid)
	{
		for (auto& w : m_instances)
		{
			if (!w.visible) continue;
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
				float mcx = mouseX / sx, mcy = mouseY / sy;
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
				if (!topW || key >= topKey)
				{
					topW = &w; topElem = e.id; topKey = key; topCursor = e.hoverCursor;
					topActs = isInteractive(w, e);
					// A text field asks for the I-beam by BEING a text field.
					// Only when the author picked nothing else: an explicit
					// hoverCursor is a decision and stays one.
					if (topCursor == HE::UICursor::Default &&
					    e.type() == HE::UIWidgetType::TextInput)
						topCursor = HE::UICursor::Text;
				}
			}
		}
	}
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

	const bool pressEdge   = primaryDown && !m_wasDown;
	const bool releaseEdge = !primaryDown && m_wasDown;

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
					setCaretFromPointer(vpWidth, vpHeight, mouseX);
					// From here until the button comes up, pointer movement
					// extends the selection instead of moving the caret.
					w.draggingText = hot;
				}
				else if (w.focusedElem != 0)
				{
					// Pressed something else in this widget → unfocus its field.
					fireP(&HorizonCode::Runtime::fireOnUnfocused, w.focusedElem);
					w.focusedElem = 0;
					if (m_focusWidget == w.id) m_focusWidget = 0;
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

		// ── Text selection drag ──────────────────────────────────────────────
		// Held down after a press inside a field: the anchor stays put and the
		// caret follows the pointer, which is what selecting with the mouse is.
		// No hit test — dragging past the edge of the field must keep extending,
		// exactly like it does everywhere else.
		if (w.draggingText != 0 && primaryDown && m_focusWidget == w.id &&
		    w.focusedElem == w.draggingText)
			dragCaretFromPointer(vpWidth, vpHeight, mouseX);

		// ── Release ──────────────────────────────────────────────────────────
		if (releaseEdge)
		{
			if (w.pressedElem != 0 && w.pressedElem == hot)
				activateElement(w, hot);
			w.pressedElem    = 0;
			w.draggingSlider = 0;
			w.draggingText   = 0;
		}
	}

	m_wasDown = primaryDown;
	// Remembered, not just returned: both apps discard the return value, and a
	// script asking "is the pointer on the UI?" runs long after this call.
	m_pointerOverUI = topW != nullptr;
	return m_pointerOverUI;
}

// The focused text field, or nullptr. Every editing entry point starts here,
// and each one re-clamps: a script can have rewritten the text since the last
// keystroke, leaving the caret pointing past the end of it.
HE::UITextInput* WidgetManager::focusedTextField(Instance*& outWidget)
{
	outWidget = nullptr;
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
	{
		const size_t before = ti->caret;
		// Without shift, a plain arrow COLLAPSES a selection to its near end
		// rather than moving off the caret — what every text field does.
		if (!extend && ti->hasSelection() &&
		    (op == TextEdit::Left || op == TextEdit::Right))
		{
			ti->caret = op == TextEdit::Left ? ti->selMin() : ti->selMax();
			ti->selAnchor = ti->caret;
			return true;
		}
		switch (op)
		{
		case TextEdit::Left:      ti->caret = HE::uiUtf8Prev(ti->text, ti->caret); break;
		case TextEdit::Right:     ti->caret = HE::uiUtf8Next(ti->text, ti->caret); break;
		case TextEdit::Home:      ti->caret = 0; break;
		case TextEdit::End:       ti->caret = ti->text.size(); break;
		case TextEdit::WordLeft:  ti->caret = wordStartBefore(ti->text, ti->caret); break;
		case TextEdit::WordRight: ti->caret = wordEndAfter(ti->text, ti->caret); break;
		default: break;
		}
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

// Byte offset in the focused field that a pointer at `mouseX` points at. The
// one place the canvas/rect/padding arithmetic lives, so click, drag and
// double-click cannot drift apart the way image and hit-area would if extract
// and processPointer used different canvases.
bool WidgetManager::caretOffsetAtPointer(float vpWidth, float vpHeight, float mouseX,
                                         size_t& outOffset)
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
	outOffset = ti->caretAtX(localX, canvas.scaleY * vs);
	return true;
}

bool WidgetManager::setCaretFromPointer(float vpWidth, float vpHeight, float mouseX)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !caretOffsetAtPointer(vpWidth, vpHeight, mouseX, at)) return false;
	ti->caret = ti->selAnchor = at;
	return true;
}

bool WidgetManager::dragCaretFromPointer(float vpWidth, float vpHeight, float mouseX)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !ti->selectable) return false;
	if (!caretOffsetAtPointer(vpWidth, vpHeight, mouseX, at)) return false;
	if (at == ti->caret) return false;
	// The ANCHOR stays where the press put it — that is what makes this a drag
	// rather than a second click.
	ti->caret = at;
	return true;
}

bool WidgetManager::selectWordAtPointer(float vpWidth, float vpHeight, float mouseX)
{
	Instance* w = nullptr;
	HE::UITextInput* ti = focusedTextField(w);
	size_t at = 0;
	if (!ti || !ti->selectable || ti->text.empty()) return false;
	if (!caretOffsetAtPointer(vpWidth, vpHeight, mouseX, at)) return false;
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
		if (auto* combo = dynamic_cast<HE::UIComboBox*>(e))
			if (!combo->options.empty())
			{
				combo->selectedIndex =
					(combo->selectedIndex + 1) % (int)combo->options.size();
				rt().fireOnSelectionChanged(target.scriptId, target.elem, combo->selectedIndex);
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

bool WidgetManager::activateFocused()
{
	m_visualDirty = true;   // a press changes the element's drawn state
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return false;
	// A focused element that has since been disabled or hidden does nothing —
	// the same rule the pointer obeys.
	const HE::UIElement* e = w->tree.find(w->focusedElem);
	if (!e || !HE::uiElementEffectiveEnabled(w->tree, *e) ||
	    !HE::uiElementEffectiveVisible(w->tree, *e)) return false;
	activateElement(*w, w->focusedElem);
	return true;
}

bool WidgetManager::navigate(NavDir dir, float vpWidth, float vpHeight)
{
	m_visualDirty = true;   // the focus ring moves
	// The widget the focus is in, else the topmost visible one that has
	// anything focusable at all.
	Instance* w = find(m_focusWidget);
	if (!w || !w->visible)
	{
		std::vector<Instance*> sorted;
		for (auto& inst : m_instances) if (inst.visible) sorted.push_back(&inst);
		std::stable_sort(sorted.begin(), sorted.end(),
			[](const Instance* a, const Instance* b){ return a->zOrder > b->zOrder; });
		w = sorted.empty() ? nullptr : sorted.front();
		if (!w) return false;
	}
	const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w->tree, vpWidth, vpHeight);

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

bool WidgetManager::processWheel(float vpWidth, float vpHeight,
                                 float mouseX, float mouseY, float wheel)
{
	if (wheel == 0.0f) return false;
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
		const HE::UIWidgetCanvas canvas = HE::uiResolveCanvas(w.tree, vpWidth, vpHeight);
		HE::uiUpdateScrollExtents(w.tree);

		int   best = 0;
		int   bestDepth = -1;
		for (const auto& ep : w.tree.elements)
		{
			const HE::UIElement& e = *ep;
			if (e.type() != HE::UIWidgetType::ScrollBox) continue;
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
	// Widgets sorted by zOrder (stable: creation order breaks ties).
	std::vector<Instance*> sorted;
	sorted.reserve(m_instances.size());
	for (auto& w : m_instances)
		if (w.visible) sorted.push_back(&w);
	std::stable_sort(sorted.begin(), sorted.end(),
		[](const Instance* a, const Instance* b){ return a->zOrder < b->zOrder; });

	for (Instance* wp : sorted)
	{
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

			// Focus ring: four hairlines around the element the keyboard or
			// gamepad is on. Drawn here rather than by the widget types because
			// every type needs it and none of them should have to know.
			if (st.focused && m_focusWidget == w.id)
			{
				constexpr float kRing = 2.0f;   // pixels
				const glm::vec4 ringCol(1.0f, 0.78f, 0.25f, 0.95f);
				const size_t ringFirst = out.size();
				auto ring = [&](float x, float y, float rw, float rh)
				{
					UIRenderObject ro;
					ro.position = { x, y };
					ro.size     = { rw, rh };
					ro.color    = ringCol;
					out.push_back(ro);
				};
				ring(px.x - kRing, px.y - kRing, px.w + 2 * kRing, kRing);              // top
				ring(px.x - kRing, px.y + px.h,  px.w + 2 * kRing, kRing);              // bottom
				ring(px.x - kRing, px.y,         kRing,            px.h);               // left
				ring(px.x + px.w,  px.y,         kRing,            px.h);               // right
				if (clipped)
				{
					const glm::vec4 r(clip.x * sx, clip.y * sy,
					                  std::max(clip.w * sx, 0.0f), std::max(clip.h * sy, 0.0f));
					for (size_t i = ringFirst; i < out.size(); ++i) out[i].clipRect = r;
				}
			}
		}
	}
}
