#pragma once
#include "Types/Defines.h"
#include "Application/Input.h"
#include "Application/InputMapping.h"
#include <map>
#include <string>
#include <vector>

// ── Player rebinding: the pieces that do not need a session ──────────────────
// A player's own bindings are a LAYER over the project's merged mapping
// contexts, never an edit of them: the assets stay what the author shipped, and
// "reset to defaults" is dropping the layer. PlayerHost owns one layer and one
// capture per session and wires them to the script rows (input.rebindBegin …);
// everything here is the device- and session-free part, so it is testable with
// a bare Input.
namespace HE
{
	// Which half of an action's bindings a rebind replaces. Keyboard and mouse
	// are ONE class: they are the same player's hands on the same desk, and a
	// "Fire" moved from the left mouse button to F is still the desk binding.
	// The pad half is untouched by a desk rebind and the other way round, so a
	// player who remaps the keyboard does not lose their controller layout.
	enum class BindingDevice { KeyboardMouse, Gamepad };

	// "keyboard" / "mouse" / "keyboardmouse" and "gamepad" / "pad", any case.
	// False for anything else, so a typo is refused rather than guessed.
	HE_API bool bindingDeviceFromName(const std::string& name, BindingDevice& out);

	// Which class a binding row belongs to. A row that sets a key or a mouse
	// button is the desk's; one that sets a pad button is the pad's. (A row
	// setting both does not come out of the loader, which writes one per row.)
	HE_API bool bindingBelongsTo(const ActionBinding& b, BindingDevice d);
	HE_API bool sameBinding(const ActionBinding& a, const ActionBinding& b);

	// What a person sees for one binding: the key's name on the CURRENT
	// keyboard layout ("Z" for the key a US layout calls "Y" on a German
	// keyboard), "A (South)" for a pad button, "Left Mouse Button". "" for an
	// empty row.
	HE_API std::string bindingDisplayName(const ActionBinding& b);

	// Every OTHER action and axis of `mapping` that `b` already triggers: button
	// actions by the same row, axes by a key or pad button of a key-source row
	// (binding Jump to W collides with a WASD Move). Sorted, `action` itself
	// left out. The rebind binds anyway and reports this — whether a double
	// binding is a mistake is the player's call, not the engine's.
	HE_API std::vector<std::string> bindingConflicts(const InputMapping& mapping,
	                                                 const std::string& action,
	                                                 const ActionBinding& b);

	// ── The override layer ───────────────────────────────────────────────────
	// Per action and device class: the bindings the player chose instead of the
	// project's. Applied OVER a merged base: an overridden class of an action
	// loses its base rows and gets the override's, the other class keeps what
	// the contexts gave it. So a base that later gains a pad binding still
	// shows it to a player who only ever remapped the keyboard.
	//
	// Stored in the mapping-context JSON format ({"entries":[{"action":"Jump",
	// "keys":["K"]}]}) and read back THROUGH applyInputMappingContext, so there
	// is one parser for bindings, not two. Only BUTTON actions for now: an
	// axis needs a direction per key, which is a later step.
	class HE_API BindingOverrides
	{
	public:
		void set(const std::string& action, BindingDevice device,
		         std::vector<ActionBinding> bindings);
		void clear() { m_entries.clear(); }
		bool empty() const { return m_entries.empty(); }
		size_t size() const { return m_entries.size(); }

		// Deterministic (actions sorted), so the same layer is the same text.
		std::string toJson() const;
		// Replaces the layer with what `json` holds. An empty string is an
		// empty layer (true); unreadable text leaves the layer empty and says so
		// (false) — a broken prefs entry must not take the defaults with it.
		bool fromJson(const std::string& json);

		// Layer the overrides onto `mapping` (the merged base). An action the
		// base never bound gets the override alone.
		void applyTo(InputMapping& mapping) const;

	private:
		struct Entry
		{
			bool hasDesk = false, hasPad = false;
			std::vector<ActionBinding> desk, pad;
		};
		std::map<std::string, Entry> m_entries;
	};

	// ── Capture: "press the button you want" ─────────────────────────────────
	// Reads Input directly, never the mapping: a menu runs in UI-only mode, where
	// every gameplay action is silenced, and the capture has to hear exactly
	// the keys the mapping is not allowed to.
	//
	// It is EDGE detection against a snapshot, not "wait until nothing is held":
	// arm() takes no input, the first update() after it records what is down
	// (the South press or click that opened the capture, a key stuck by a lost
	// focus) and only a press that starts AFTER that counts. A stuck key can
	// therefore never block a capture forever.
	//
	// Cancel: Escape, or the pad's Start/Menu button. Those two can therefore
	// not be bound through a capture — Start is where a pause menu lives anyway.
	// The apps also cancel on Escape themselves, because both consume that key
	// before Input sees it.
	//
	// After a capture the state stays "busy" until the captured button is
	// released again (Draining): the press that was captured must not also
	// activate the menu button under the focus, and a rebuilt mapping must not
	// see it held as a fresh press.
	class HE_API BindingCapture
	{
	public:
		enum class Phase  { Idle, Arming, Listening, Draining };
		enum class Result { None, Captured, Cancelled };

		void arm(BindingDevice device);
		// Stop at once, whatever the phase; nothing is reported.
		void cancel();

		// Once per frame with this frame's input. Answers Captured/Cancelled on
		// the frame the capture FINISHES — for Captured that is when the
		// captured button comes back up — and None on every other frame.
		Result update(const Input& input, const MouseFrame& mouse);

		Phase         phase()    const { return m_phase; }
		bool          busy()     const { return m_phase != Phase::Idle; }
		BindingDevice device()   const { return m_device; }
		// The binding caught — valid from the press on, Draining included.
		const ActionBinding& captured() const { return m_captured; }

	private:
		void snapshot(const Input& input, const MouseFrame& mouse);
		bool heldNow(const Input& input, const MouseFrame& mouse) const;

		Phase         m_phase  = Phase::Idle;
		BindingDevice m_device = BindingDevice::KeyboardMouse;
		ActionBinding m_captured;
		Result        m_pending = Result::None;
		bool          m_keys[SDL_SCANCODE_COUNT]{};
		bool          m_pad[SDL_GAMEPAD_BUTTON_COUNT]{};
		uint32_t      m_mouse = 0;
	};
}
