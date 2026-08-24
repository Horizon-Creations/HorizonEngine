#include <HorizonScene/WidgetManager.h>
#include <HorizonScene/UISystem.h>   // sortKey — one painter-order rule for both UI paths
#include <HorizonCode/HcCompiledLoader.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Renderer/UIFont.h>
#include <Diagnostics/Logger.h>
#include <algorithm>

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

// Host bindings shared by every widget: the central runtime owns the graph +
// variable state and hands back the InstanceId, so one binding set serves all
// widgets. Property access + show/hide resolve the widget from the id and act on
// its live tree / visibility. Variables are handled by the runtime, not here.
HorizonCode::HostBindings WidgetManager::makeBindings()
{
	HorizonCode::HostBindings b;
	b.getProperty = [this](HorizonCode::InstanceId id, int elem, const std::string& prop) -> HorizonCode::Value
	{
		Instance* w = findByScript(id);
		const HE::UIElement* e = w ? w->tree.find(elem) : nullptr;
		// getPropAny/setPropAny: base properties (Visible, Hit Testable,
		// Position, Size, Layer, Hover Cursor, Material, Font) plus the
		// type-specific ones — every property is both gettable and settable.
		return e ? HE::uiPropToHcValue(e->getPropAny(prop)) : HorizonCode::Value{};
	};
	b.setProperty = [this](HorizonCode::InstanceId id, int elem, const std::string& prop, const HorizonCode::Value& v)
	{
		Instance* w = findByScript(id);
		HE::UIElement* e = w ? w->tree.find(elem) : nullptr;
		if (!e) return;
		e->setPropAny(prop, HE::uiHcValueToProp(v, e->getPropAny(prop).type));
		// Asset-path properties change what the element draws with — re-resolve
		// immediately so the set is visible this frame, not on the next reload.
		if (prop == "Material" || prop == "Font")
			refreshElementAssets(*w, *e);
	};
	b.showSelf = [this](HorizonCode::InstanceId id){ if (Instance* w = findByScript(id)) w->visible = true; };
	b.hideSelf = [this](HorizonCode::InstanceId id){ if (Instance* w = findByScript(id)) w->visible = false; };
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
	return stored.id;
}

void WidgetManager::destroyWidget(int id)
{
	if (m_focusWidget == id) m_focusWidget = 0;
	if (Instance* w = find(id))
	{
		HE_LOG_DEBUG(Widget, "Destroying widget id %d", id);
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
	// Fire each widget's "Destruct" and unregister it from the shared runtime
	// (which may also host the level script / GameInstance — so tear down
	// per-instance, don't wipe). Snapshot the ids first: a Destruct handler may
	// itself destroy widgets, mutating m_instances mid-iteration.
	std::vector<HorizonCode::InstanceId> ids;
	ids.reserve(m_instances.size());
	for (const auto& w : m_instances) ids.push_back(w.scriptId);
	if (!ids.empty()) HE_LOG_DEBUG(Widget, "Clearing %zu live widget(s)", ids.size());
	for (const HorizonCode::InstanceId sid : ids) rt().destroy(sid);
	m_instances.clear();
	m_focusWidget = 0;
}

void WidgetManager::showWidget(int id)  { if (Instance* w = find(id)) w->visible = true; }
void WidgetManager::hideWidget(int id)  { if (Instance* w = find(id)) w->visible = false; }
void WidgetManager::setZOrder(int id, int z) { if (Instance* w = find(id)) w->zOrder = z; }

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
	}
}

bool WidgetManager::isInteractive(const Instance& w, const HE::UIElement& e) const
{
	if (e.interactive()) return true;
	// Bound by a pointer-event node? (elem 0 = any element.) eventBindingsOf
	// serves interpreted (Event nodes) and compiled (static tables) scripts alike.
	for (const auto& b : rt().eventBindingsOf(w.scriptId))
		if (b.elem == 0 || b.elem == e.id)
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
	// Topmost interactive element under the pointer, across all visible
	// widgets: highest (widget zOrder, element sort key) wins.
	Instance* topW = nullptr;
	int  topElem = 0;
	long topKey  = 0;
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
				// hitTestable false = transparent to the pointer. Otherwise an
				// element is a hit candidate if it's interactive OR carries a custom
				// hover cursor (so decorative elements can drive the cursor too).
				if (!e.hitTestable) continue;
				// Disabled is inert, all the way down: a greyed-out button that
				// still hovers and clicks is the classic UI lie.
				if (!HE::uiElementEffectiveEnabled(w.tree, e)) continue;
				// Faded to nothing means gone — a menu at opacity 0 must not
				// keep swallowing the clicks meant for what is behind it.
				if (HE::uiElementEffectiveOpacity(w.tree, e) <= 0.001f) continue;
				if (!isInteractive(w, e) && e.hoverCursor == HE::UICursor::Default) continue;
				HE::UIWidgetRect r = HE::uiElementRect(w.tree, e, &canvas);
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
				if (mouseX < x0 || mouseX > x1 || mouseY < y0 || mouseY > y1)
					continue;
				const long key = (long)w.zOrder * 1000000 + elementSortKey(w.tree, e);
				if (!topW || key >= topKey)
				{
					topW = &w; topElem = e.id; topKey = key; topCursor = e.hoverCursor;
				}
			}
		}
	}
	m_hoverCursor = topCursor; // app maps this to a system cursor

	const bool pressEdge   = primaryDown && !m_wasDown;
	const bool releaseEdge = !primaryDown && m_wasDown;

	for (auto& w : m_instances)
	{
		const bool isTop = topW == &w;
		const int  hot   = isTop ? topElem : 0;
		// Typed entry points: the compiled side takes a method, the interpreted
		// one the same named path as before, and both still reach everyone bound
		// to this widget's script.
		auto fireP = [&](void (HorizonCode::Runtime::*fn)(HorizonCode::InstanceId, int), int elem)
		{ (rt().*fn)(w.scriptId, elem); };

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
				// TextInput: focus it.
				if (e && e->type() == HE::UIWidgetType::TextInput)
				{
					if (w.focusedElem != hot)
					{
						w.focusedElem = hot;
						m_focusWidget = w.id;
						fireP(&HorizonCode::Runtime::fireOnFocused, hot);
					}
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
					rt().fireOnValueChanged(w.scriptId, w.draggingSlider, nv);
				}
			}
		}

		// ── Release ──────────────────────────────────────────────────────────
		if (releaseEdge)
		{
			if (w.pressedElem != 0 && w.pressedElem == hot)
				activateElement(w, hot);
			w.pressedElem    = 0;
			w.draggingSlider = 0;
		}
	}

	m_wasDown = primaryDown;
	return topW != nullptr;
}

void WidgetManager::inputText(const std::string& utf8)
{
	if (m_focusWidget == 0) return;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti) return;
	ti->text += utf8;
	rt().fireOnTextChanged(w->scriptId, w->focusedElem, ti->text);
}

void WidgetManager::inputBackspace()
{
	if (m_focusWidget == 0) return;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti || ti->text.empty()) return;
	// Drop one UTF-8 code point (trailing continuation bytes 10xxxxxx).
	size_t n = ti->text.size();
	do { --n; } while (n > 0 && (static_cast<unsigned char>(ti->text[n]) & 0xC0) == 0x80);
	ti->text.erase(n);
	rt().fireOnTextChanged(w->scriptId, w->focusedElem, ti->text);
}

void WidgetManager::inputSubmit()
{
	if (m_focusWidget == 0) return;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti) return;
	rt().fireOnTextCommitted(w->scriptId, w->focusedElem, ti->text);
}

void WidgetManager::activateElement(Instance& w, int elemId)
{
	HE::UIElement* e = w.tree.find(elemId);
	if (!e) return;
	auto fireP = [&](void (HorizonCode::Runtime::*fn)(HorizonCode::InstanceId, int))
	{ (rt().*fn)(w.scriptId, elemId); };

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
			rt().fireOnCheckChanged(w.scriptId, elemId, cb->checked);
		}
		break;
	case HE::UIWidgetType::ComboBox:
		if (auto* combo = dynamic_cast<HE::UIComboBox*>(e))
			if (!combo->options.empty())
			{
				combo->selectedIndex =
					(combo->selectedIndex + 1) % (int)combo->options.size();
				rt().fireOnSelectionChanged(w.scriptId, elemId, combo->selectedIndex);
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
	Instance* w = find(widgetId);
	if (!w) return false;
	if (elementId == 0)
	{
		if (w->focusedElem != 0)
			rt().fireOnUnfocused(w->scriptId, w->focusedElem);
		w->focusedElem = 0;
		if (m_focusWidget == widgetId) m_focusWidget = 0;
		return true;
	}
	const HE::UIElement* e = w->tree.find(elementId);
	if (!e) return false;
	if (w->focusedElem == elementId) return true;
	if (w->focusedElem != 0) rt().fireOnUnfocused(w->scriptId, w->focusedElem);
	w->focusedElem = elementId;
	m_focusWidget  = widgetId;
	rt().fireOnFocused(w->scriptId, elementId);
	return true;
}

bool WidgetManager::activateFocused()
{
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
					rt().fireOnValueChanged(w->scriptId, w->focusedElem, nv);
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

			// The element draws itself (quads + glyphs) into `out`.
			const size_t firstQuad = out.size();
			e.render(px, st, matId, sy, out);

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
				for (size_t i = firstQuad; i < out.size(); ++i)
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
				for (size_t i = firstQuad; i < out.size(); ++i) out[i].clipRect = r;
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
