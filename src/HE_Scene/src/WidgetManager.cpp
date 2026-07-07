#include <HorizonScene/WidgetManager.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <algorithm>

namespace
{
	// Sort key inside one widget: layer (major) + nesting depth (minor), the
	// same rule the entity UI path uses — children draw over their parents.
	int elementSortKey(const HE::UIWidgetTree& tree, const HE::UIElement& e)
	{
		int depth = 0;
		for (const HE::UIElement* c = &e; c->parentId != 0 && depth < 255; ++depth)
		{
			const HE::UIElement* p = tree.find(c->parentId);
			if (!p) break;
			c = p;
		}
		return e.layer * 256 + depth;
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

// Context wires the interpreter to ONE instance. Capture by pointer (never a
// reference to a vector element that can move) and only use the Runner locally,
// so a concurrent m_instances reallocation can't dangle the bindings.
HorizonCode::Context WidgetManager::makeContext(Instance& inst)
{
	Instance* i = &inst;
	HorizonCode::Context ctx;
	ctx.getProperty = [i](int elem, const std::string& prop) -> HorizonCode::Value
	{
		const HE::UIElement* e = i->tree.find(elem);
		return e ? HE::uiPropToHcValue(e->getProp(prop)) : HorizonCode::Value{};
	};
	ctx.setProperty = [i](int elem, const std::string& prop, const HorizonCode::Value& v)
	{
		HE::UIElement* e = i->tree.find(elem);
		if (e) e->setProp(prop, HE::uiHcValueToProp(v, e->getProp(prop).type));
	};
	ctx.showSelf = [i] { i->visible = true; };
	ctx.hideSelf = [i] { i->visible = false; };
	return ctx;
}

int WidgetManager::createWidget(ContentManager& content, const std::string& assetPath)
{
	const HE::UUID assetId = content.loadAsset(assetPath);
	const UIWidgetAsset* asset = content.getWidget(assetId);
	if (!asset)
	{
		Logger::Log(Logger::LogLevel::Warning,
			("WidgetManager: widget asset not found: " + assetPath).c_str());
		return 0;
	}

	Instance w;
	if (!HE::uiWidgetTreeFromJson(asset->treeJson, w.tree))
	{
		Logger::Log(Logger::LogLevel::Error,
			("WidgetManager: invalid widget tree JSON in " + assetPath).c_str());
		return 0;
	}
	if (!asset->graphJson.empty())
		HorizonCode::fromJson(asset->graphJson, w.graph); // absent/broken → no logic

	// Resolve per-element material references once (paths → UUIDs).
	for (const auto& e : w.tree.elements)
		if (!e->material.empty())
		{
			const HE::UUID mid = content.loadAsset(e->material);
			if (mid != HE::UUID{}) w.materials[e->id] = mid;
		}

	w.id = m_nextId++;
	m_instances.push_back(std::move(w));

	Instance& stored = m_instances.back();
	HorizonCode::Runner runner(stored.graph, makeContext(stored));
	runner.fireEvent("Construct", 0);
	return stored.id;
}

void WidgetManager::destroyWidget(int id)
{
	if (m_focusWidget == id) m_focusWidget = 0;
	m_instances.erase(std::remove_if(m_instances.begin(), m_instances.end(),
		[&](const Instance& w){ return w.id == id; }), m_instances.end());
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
	if (!w) return false;
	HorizonCode::Runner runner(w->graph, makeContext(*w));
	return runner.callFunction(name, /*requirePublic=*/true);
}

void WidgetManager::tick(float dt)
{
	for (auto& w : m_instances)
	{
		if (!w.visible) continue;
		HorizonCode::Runner runner(w.graph, makeContext(w));
		runner.fireEvent("Tick", 0, HorizonCode::Value::ofFloat(dt));
	}
}

bool WidgetManager::isInteractive(const Instance& w, const HE::UIElement& e)
{
	if (e.interactive()) return true;
	// Bound by a pointer-event node? (Event node with elem 0 = any element.)
	for (const auto& gn : w.graph.nodes)
		if (gn.type == HorizonCode::NodeType::Event && (gn.elem == 0 || gn.elem == e.id))
		{
			const std::string& n = gn.s;
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
	if (valid)
	{
		for (auto& w : m_instances)
		{
			if (!w.visible) continue;
			const float sx = vpWidth  / w.tree.canvasWidth;
			const float sy = vpHeight / w.tree.canvasHeight;
			for (const auto& ep : w.tree.elements)
			{
				const HE::UIElement& e = *ep;
				if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
				if (!isInteractive(w, e)) continue;
				const HE::UIWidgetRect r = HE::uiElementRect(w.tree, e);
				const float x0 = r.x * sx, y0 = r.y * sy;
				const float x1 = (r.x + r.w) * sx, y1 = (r.y + r.h) * sy;
				if (mouseX < x0 || mouseX > x1 || mouseY < y0 || mouseY > y1)
					continue;
				const long key = (long)w.zOrder * 1000000 + elementSortKey(w.tree, e);
				if (!topW || key >= topKey)
				{
					topW = &w; topElem = e.id; topKey = key;
				}
			}
		}
	}

	const bool pressEdge   = primaryDown && !m_wasDown;
	const bool releaseEdge = !primaryDown && m_wasDown;

	for (auto& w : m_instances)
	{
		const bool isTop = topW == &w;
		const int  hot   = isTop ? topElem : 0;
		HorizonCode::Runner runner(w.graph, makeContext(w));

		// ── Hover transitions ────────────────────────────────────────────────
		// Event names differ per type; fire BOTH candidate names — the Runner
		// only matches Event nodes that actually exist.
		if (w.hoveredElem != hot)
		{
			if (w.hoveredElem != 0)
			{
				runner.fireEvent("OnUnhovered", w.hoveredElem);
				runner.fireEvent("OnMouseLeave", w.hoveredElem);
			}
			if (hot != 0)
			{
				runner.fireEvent("OnHovered", hot);
				runner.fireEvent("OnMouseEnter", hot);
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
					runner.fireEvent("OnPressed", hot);
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
						runner.fireEvent("OnFocused", hot);
					}
				}
				else if (w.focusedElem != 0)
				{
					// Pressed something else in this widget → unfocus its field.
					runner.fireEvent("OnUnfocused", w.focusedElem);
					w.focusedElem = 0;
					if (m_focusWidget == w.id) m_focusWidget = 0;
				}
			}
			else if (w.focusedElem != 0)
			{
				// Pressed empty space → unfocus.
				runner.fireEvent("OnUnfocused", w.focusedElem);
				w.focusedElem = 0;
				if (m_focusWidget == w.id) m_focusWidget = 0;
			}
		}

		// ── Slider drag ──────────────────────────────────────────────────────
		if (w.draggingSlider != 0 && primaryDown)
		{
			if (auto* s = dynamic_cast<HE::UISlider*>(w.tree.find(w.draggingSlider)))
			{
				const float sx = vpWidth / w.tree.canvasWidth;
				const HE::UIWidgetRect r = HE::uiElementRect(w.tree, *s);
				const float mouseCanvasX = mouseX / sx;
				float t = r.w > 0.0f ? (mouseCanvasX - r.x) / r.w : 0.0f;
				t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
				const float nv = s->minValue + t * (s->maxValue - s->minValue);
				if (nv != s->value)
				{
					s->value = nv;
					runner.fireEvent("OnValueChanged", w.draggingSlider,
					                 HorizonCode::Value::ofFloat(nv));
				}
			}
		}

		// ── Release ──────────────────────────────────────────────────────────
		if (releaseEdge)
		{
			const bool sameHit = w.pressedElem != 0 && w.pressedElem == hot;
			if (sameHit)
			{
				HE::UIElement* e = w.tree.find(hot);
				switch (e ? e->type() : HE::UIWidgetType::COUNT)
				{
				case HE::UIWidgetType::Button:
					runner.fireEvent("OnClicked", hot);
					runner.fireEvent("OnReleased", hot);
					break;
				case HE::UIWidgetType::Panel:
				case HE::UIWidgetType::Image:
					runner.fireEvent("OnClicked", hot);
					break;
				case HE::UIWidgetType::CheckBox:
					if (auto* cb = dynamic_cast<HE::UICheckBox*>(e))
					{
						cb->checked = !cb->checked;
						runner.fireEvent("OnCheckChanged", hot,
						                 HorizonCode::Value::ofBool(cb->checked));
					}
					break;
				case HE::UIWidgetType::ComboBox:
					if (auto* combo = dynamic_cast<HE::UIComboBox*>(e))
						if (!combo->options.empty())
						{
							combo->selectedIndex =
								(combo->selectedIndex + 1) % (int)combo->options.size();
							runner.fireEvent("OnSelectionChanged", hot,
							                 HorizonCode::Value::ofInt(combo->selectedIndex));
						}
					break;
				default:
					break;
				}
			}
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
	HorizonCode::Runner runner(w->graph, makeContext(*w));
	runner.fireEvent("OnTextChanged", w->focusedElem, HorizonCode::Value::ofString(ti->text));
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
	HorizonCode::Runner runner(w->graph, makeContext(*w));
	runner.fireEvent("OnTextChanged", w->focusedElem, HorizonCode::Value::ofString(ti->text));
}

void WidgetManager::inputSubmit()
{
	if (m_focusWidget == 0) return;
	Instance* w = find(m_focusWidget);
	if (!w || w->focusedElem == 0) return;
	auto* ti = dynamic_cast<HE::UITextInput*>(w->tree.find(w->focusedElem));
	if (!ti) return;
	HorizonCode::Runner runner(w->graph, makeContext(*w));
	runner.fireEvent("OnTextCommitted", w->focusedElem, HorizonCode::Value::ofString(ti->text));
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
		const float sx = vpWidth  / w.tree.canvasWidth;
		const float sy = vpHeight / w.tree.canvasHeight;

		// Draw elements of this widget, painter-ordered by (layer, depth).
		struct Item { const HE::UIElement* e; int key; HE::UIWidgetRect r; };
		std::vector<Item> items;
		for (const auto& ep : w.tree.elements)
		{
			const HE::UIElement& e = *ep;
			if (!HE::uiElementEffectiveVisible(w.tree, e)) continue;
			items.push_back({ &e, elementSortKey(w.tree, e), HE::uiElementRect(w.tree, e) });
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

			// The element draws itself (quads + glyphs) into `out`.
			e.render(px, st, matId, sy, out);
		}
	}
}
