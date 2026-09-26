#include "Application/InputRebind.h"
#include "Application/InputAssets.h"
#include "Diagnostics/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

namespace HE
{

bool bindingDeviceFromName(const std::string& name, BindingDevice& out)
{
	std::string n = name;
	std::transform(n.begin(), n.end(), n.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (n == "keyboard" || n == "mouse" || n == "keyboardmouse")
	{ out = BindingDevice::KeyboardMouse; return true; }
	if (n == "gamepad" || n == "pad")
	{ out = BindingDevice::Gamepad; return true; }
	return false;
}

bool bindingBelongsTo(const ActionBinding& b, BindingDevice d)
{
	if (d == BindingDevice::Gamepad) return b.gamepadButton != SDL_GAMEPAD_BUTTON_INVALID;
	return b.key != SDL_SCANCODE_UNKNOWN || b.mouseButton >= 0;
}

bool sameBinding(const ActionBinding& a, const ActionBinding& b)
{
	return a.key == b.key && a.gamepadButton == b.gamepadButton &&
	       a.mouseButton == b.mouseButton;
}

std::string bindingDisplayName(const ActionBinding& b)
{
	if (b.key != SDL_SCANCODE_UNKNOWN)
	{
		// The key's name on the layout in use: the scancode is a POSITION, and
		// telling a French player to press "W" for the key that says Z on it is
		// how a settings screen loses them. Without a keymap (headless) SDL
		// answers from its default one; the scancode's own name is the fallback
		// for keys that map to nothing printable.
		const SDL_Keycode kc = SDL_GetKeyFromScancode(b.key, SDL_KMOD_NONE, false);
		const char* kn = kc != SDLK_UNKNOWN ? SDL_GetKeyName(kc) : nullptr;
		if (kn && kn[0]) return kn;
		const char* sn = SDL_GetScancodeName(b.key);
		return sn && sn[0] ? sn : "?";
	}
	if (b.gamepadButton != SDL_GAMEPAD_BUTTON_INVALID) return gamepadButtonDisplayName(b.gamepadButton);
	if (b.mouseButton >= 0) return mouseButtonDisplayName(b.mouseButton);
	return "";
}

std::vector<std::string> bindingConflicts(const InputMapping& mapping, const std::string& action,
                                          const ActionBinding& b)
{
	std::vector<std::string> out;
	for (const std::string& name : mapping.actionNames())
	{
		if (name == action) continue;
		if (const auto* rows = mapping.actionBindings(name))
			if (std::any_of(rows->begin(), rows->end(),
			                [&](const ActionBinding& r) { return sameBinding(r, b); }))
				out.push_back(name);
	}
	// An axis is only hit through its key-source rows: a mouse or stick source
	// has no button of its own that a captured press could share.
	auto axisUses = [&](const std::vector<AxisBinding>& rows)
	{
		for (const AxisBinding& r : rows)
		{
			if (r.source != AxisSource::Key) continue;
			if (b.key != SDL_SCANCODE_UNKNOWN &&
			    (r.positiveKey == b.key || r.negativeKey == b.key)) return true;
			if (b.gamepadButton != SDL_GAMEPAD_BUTTON_INVALID &&
			    (r.positiveButton == b.gamepadButton || r.negativeButton == b.gamepadButton))
				return true;
		}
		return false;
	};
	for (const std::string& name : mapping.axisNames())
	{
		if (name == action) continue;
		const auto* x = mapping.axisBindings(name);
		const auto* y = mapping.axisYBindings(name);
		if ((x && axisUses(*x)) || (y && axisUses(*y))) out.push_back(name);
	}
	// Both lists come out sorted; together they need it once more, and a name
	// that is an action AND an axis (a shape conflict between contexts) once.
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// ── BindingOverrides ─────────────────────────────────────────────────────────

void BindingOverrides::set(const std::string& action, BindingDevice device,
                           std::vector<ActionBinding> bindings)
{
	Entry& e = m_entries[action];
	// Only rows of the class being replaced: a desk rebind that somehow carried
	// a pad row would otherwise leak it into the wrong half.
	std::erase_if(bindings, [&](const ActionBinding& b) { return !bindingBelongsTo(b, device); });
	if (device == BindingDevice::Gamepad) { e.hasPad = true;  e.pad  = std::move(bindings); }
	else                                  { e.hasDesk = true; e.desk = std::move(bindings); }
}

std::string BindingOverrides::toJson() const
{
	nlohmann::json entries = nlohmann::json::array();
	for (const auto& [name, e] : m_entries)   // std::map: sorted by action
	{
		nlohmann::json j;
		// The action NAME where a context has a path: the loader keys on the
		// path's stem, and a bare name is its own stem.
		j["action"] = name;
		if (e.hasDesk)
		{
			nlohmann::json keys = nlohmann::json::array(), mouse = nlohmann::json::array();
			for (const ActionBinding& b : e.desk)
			{
				if (b.key != SDL_SCANCODE_UNKNOWN) keys.push_back(SDL_GetScancodeName(b.key));
				if (b.mouseButton >= 0)            mouse.push_back(mouseButtonName(b.mouseButton));
			}
			j["keys"] = std::move(keys);
			j["mouseButtons"] = std::move(mouse);
		}
		if (e.hasPad)
		{
			nlohmann::json pad = nlohmann::json::array();
			for (const ActionBinding& b : e.pad)
				if (const char* n = SDL_GetGamepadStringForButton(b.gamepadButton)) pad.push_back(n);
			j["gamepadButtons"] = std::move(pad);
		}
		entries.push_back(std::move(j));
	}
	nlohmann::json doc;
	doc["entries"] = std::move(entries);
	return doc.dump();
}

bool BindingOverrides::fromJson(const std::string& json)
{
	m_entries.clear();
	if (json.find_first_not_of(" \t\r\n") == std::string::npos) return true;
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array())
	{
		HE_LOG_WARN(Input, "%s", "Saved input bindings are unreadable - using the project's defaults");
		return false;
	}
	// Through the context loader, into a scratch mapping: the rows it makes are
	// exactly the rows a context with the same text would make.
	InputMapping scratch;
	applyInputMappingContext(scratch, json, MappingMerge::Replace);
	for (const std::string& name : scratch.actionNames())
	{
		const auto* rows = scratch.actionBindings(name);
		if (!rows) continue;
		std::vector<ActionBinding> desk, pad;
		for (const ActionBinding& b : *rows)
		{
			if (bindingBelongsTo(b, BindingDevice::Gamepad)) pad.push_back(b);
			if (bindingBelongsTo(b, BindingDevice::KeyboardMouse)) desk.push_back(b);
		}
		if (!desk.empty()) set(name, BindingDevice::KeyboardMouse, std::move(desk));
		if (!pad.empty())  set(name, BindingDevice::Gamepad, std::move(pad));
	}
	return true;
}

void BindingOverrides::applyTo(InputMapping& mapping) const
{
	for (const auto& [name, e] : m_entries)
	{
		std::vector<ActionBinding> rows;
		if (const auto* base = mapping.actionBindings(name)) rows = *base;
		std::erase_if(rows, [&](const ActionBinding& b)
		{
			return (e.hasDesk && bindingBelongsTo(b, BindingDevice::KeyboardMouse)) ||
			       (e.hasPad  && bindingBelongsTo(b, BindingDevice::Gamepad));
		});
		if (e.hasDesk) rows.insert(rows.end(), e.desk.begin(), e.desk.end());
		if (e.hasPad)  rows.insert(rows.end(), e.pad.begin(),  e.pad.end());
		// Replace, not add: the layer is the last word on this action.
		mapping.mapAction(name, std::move(rows));
	}
}

// ── BindingCapture ───────────────────────────────────────────────────────────

void BindingCapture::arm(BindingDevice device)
{
	m_device   = device;
	m_phase    = Phase::Arming;
	m_pending  = Result::None;
	m_captured = ActionBinding{};
}

void BindingCapture::cancel()
{
	m_phase   = Phase::Idle;
	m_pending = Result::None;
}

void BindingCapture::snapshot(const Input& input, const MouseFrame& mouse)
{
	for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc)
		m_keys[sc] = input.IsKeyDown(static_cast<SDL_Scancode>(sc));
	for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
		m_pad[b] = input.isGamepadButtonDown(static_cast<SDL_GamepadButton>(b));
	m_mouse = mouse.buttons;
}

bool BindingCapture::heldNow(const Input& input, const MouseFrame& mouse) const
{
	if (m_captured.key != SDL_SCANCODE_UNKNOWN) return input.IsKeyDown(m_captured.key);
	if (m_captured.gamepadButton != SDL_GAMEPAD_BUTTON_INVALID)
		return input.isGamepadButtonDown(m_captured.gamepadButton);
	if (m_captured.mouseButton >= 0) return (mouse.buttons >> m_captured.mouseButton) & 1u;
	return false;
}

BindingCapture::Result BindingCapture::update(const Input& input, const MouseFrame& mouse)
{
	switch (m_phase)
	{
	case Phase::Idle:
		return Result::None;

	case Phase::Arming:
		// Only look this frame: whatever is down now was down before the
		// capture began and is not the player's answer.
		snapshot(input, mouse);
		m_phase = Phase::Listening;
		return Result::None;

	case Phase::Listening:
	{
		auto keyEdge = [&](int sc) { return input.IsKeyDown(static_cast<SDL_Scancode>(sc)) && !m_keys[sc]; };
		auto padEdge = [&](int b)
		{ return input.isGamepadButtonDown(static_cast<SDL_GamepadButton>(b)) && !m_pad[b]; };

		ActionBinding hit;
		Result        what = Result::None;
		// The two ways out, on either device whatever is being bound: a player
		// holding the pad must not need the keyboard to back out, and the
		// other way round.
		if (keyEdge(SDL_SCANCODE_ESCAPE))
		{ hit.key = SDL_SCANCODE_ESCAPE; what = Result::Cancelled; }
		else if (padEdge(SDL_GAMEPAD_BUTTON_START))
		{ hit.gamepadButton = SDL_GAMEPAD_BUTTON_START; what = Result::Cancelled; }
		else if (m_device == BindingDevice::KeyboardMouse)
		{
			for (int sc = 1; sc < SDL_SCANCODE_COUNT && what == Result::None; ++sc)
				if (sc != SDL_SCANCODE_ESCAPE && keyEdge(sc))
				{ hit.key = static_cast<SDL_Scancode>(sc); what = Result::Captured; }
			for (int mb = 0; mb < kMouseButtonCount && what == Result::None; ++mb)
				if (((mouse.buttons >> mb) & 1u) && !((m_mouse >> mb) & 1u))
				{ hit.mouseButton = mb; what = Result::Captured; }
		}
		else
		{
			for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && what == Result::None; ++b)
				if (b != SDL_GAMEPAD_BUTTON_START && padEdge(b))
				{ hit.gamepadButton = static_cast<SDL_GamepadButton>(b); what = Result::Captured; }
		}
		snapshot(input, mouse);
		if (what != Result::None)
		{
			m_captured = hit;
			m_pending  = what;
			m_phase    = Phase::Draining;
		}
		return Result::None;
	}

	case Phase::Draining:
		if (heldNow(input, mouse)) return Result::None;
		{
			const Result r = m_pending;
			m_pending = Result::None;
			m_phase   = Phase::Idle;
			return r;
		}
	}
	return Result::None;
}

} // namespace HE
