#include "InputMappingModel.h"
#include <Application/InputAssets.h>   // axisSourceName / axisSourceFromName
#include <nlohmann/json.hpp>

namespace HE::Ed
{

void decodeMapping(const std::string& json, std::vector<MapEntry>& out)
{
	out.clear();
	const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
	if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) return;
	for (const auto& e : j["entries"])
	{
		if (!e.is_object()) continue;
		MapEntry me;
		me.actionPath = e.value("action", "");
		if (e.contains("keys") && e["keys"].is_array())
			for (const auto& k : e["keys"])
				if (k.is_string()) me.keys.push_back(k.get<std::string>());
		if (e.contains("gamepadButtons") && e["gamepadButtons"].is_array())
			for (const auto& gb : e["gamepadButtons"])
				if (gb.is_string()) me.gamepadButtons.push_back(gb.get<std::string>());
		if (e.contains("mouseButtons") && e["mouseButtons"].is_array())
			for (const auto& mb : e["mouseButtons"])
				if (mb.is_string()) me.mouseButtons.push_back(mb.get<std::string>());
		auto readAxes = [](const nlohmann::json& arr, std::vector<AxisRow>& into)
		{
			for (const auto& a : arr)
			{
				if (!a.is_object()) continue;
				AxisRow r;
				r.positive       = a.value("positive", "");
				r.negative       = a.value("negative", "");
				r.positiveButton = a.value("positiveButton", "");
				r.negativeButton = a.value("negativeButton", "");
				r.scale          = a.value("scale", 1.0f);
				r.source         = HE::axisSourceFromName(a.value("source", "Key"));
				r.uiPadRow       = !r.positiveButton.empty() || !r.negativeButton.empty();
				into.push_back(std::move(r));
			}
		};
		if (e.contains("axes")  && e["axes"].is_array())  readAxes(e["axes"],  me.axes);
		if (e.contains("axesX") && e["axesX"].is_array()) readAxes(e["axesX"], me.axes);
		if (e.contains("axesY") && e["axesY"].is_array()) readAxes(e["axesY"], me.axesY);
		out.push_back(std::move(me));
	}
}

std::string encodeMapping(const std::vector<MapEntry>& entries)
{
	nlohmann::json j; j["entries"] = nlohmann::json::array();
	for (const auto& e : entries)
	{
		nlohmann::json je; je["action"] = e.actionPath;
		if (!e.keys.empty()) je["keys"] = e.keys;
		if (!e.gamepadButtons.empty()) je["gamepadButtons"] = e.gamepadButtons;
		if (!e.mouseButtons.empty())   je["mouseButtons"]   = e.mouseButtons;
		auto writeAxes = [](const std::vector<AxisRow>& rows)
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& a : rows)
			{
				nlohmann::json ja = { {"positive", a.positive}, {"negative", a.negative},
				                      {"scale", a.scale},
				                      {"source", HE::axisSourceName(a.source)} };
				// Written only when set — old contexts round-trip byte-stable.
				if (!a.positiveButton.empty()) ja["positiveButton"] = a.positiveButton;
				if (!a.negativeButton.empty()) ja["negativeButton"] = a.negativeButton;
				arr.push_back(std::move(ja));
			}
			return arr;
		};
		// An entry is 2D when its ACTION declares Axis2D — or, for an entry
		// whose action could not be resolved, when a Y list exists (the old
		// shape rule, kept as the fallback). Writing axesX for a 2D entry even
		// while Y is still empty matters: "axes" would register a 1D mapping
		// and axis2DValue() would answer 0,0 for it at runtime.
		const bool as2D = e.valueType == 2 || (e.valueType < 0 && !e.axesY.empty());
		if (as2D)
		{
			if (!e.axes.empty())  je["axesX"] = writeAxes(e.axes);
			if (!e.axesY.empty()) je["axesY"] = writeAxes(e.axesY);
		}
		else if (!e.axes.empty())
			je["axes"] = writeAxes(e.axes);
		j["entries"].push_back(std::move(je));
	}
	return j.dump();
}

// ── Binding by pressing the thing ────────────────────────────────────────────

bool bindSlotAccepts(BindSlot slot, const DetectedBinding& d)
{
	switch (slot)
	{
	case BindSlot::EntryAny:    return d.any();
	case BindSlot::Key:
	case BindSlot::AxisKeyPos:
	case BindSlot::AxisKeyNeg:  return !d.key.empty();
	case BindSlot::PadButton:
	case BindSlot::AxisPadPos:
	case BindSlot::AxisPadNeg:  return !d.gamepadButton.empty();
	case BindSlot::MouseButton: return !d.mouseButton.empty();
	case BindSlot::AxisSource:  return d.axisSource >= 0;
	case BindSlot::None:        break;
	}
	return false;
}

namespace
{
	// The axis row a slot addresses, or nullptr when the index no longer exists
	// (a capture can outlive the row it was armed on — the panel cancels those,
	// this is the second line of defence).
	AxisRow* axisRowAt(MapEntry& e, int sub)
	{
		std::vector<AxisRow>& rows = axisRowIsY(sub) ? e.axesY : e.axes;
		const int idx = axisRowIndex(sub);
		if (idx < 0 || idx >= static_cast<int>(rows.size())) return nullptr;
		return &rows[idx];
	}

	template <class T>
	bool replaceAt(std::vector<T>& v, int idx, const T& value)
	{
		if (idx < 0 || idx >= static_cast<int>(v.size())) return false;
		v[idx] = value;
		return true;
	}

	// The card-level Auto Detect: what the ACTION declares decides which list a
	// press lands in. A Button entry takes buttons; an Axis entry turns a key or
	// pad button into the POSITIVE half of a new pair row and a moved stick or
	// pulled trigger into a source row. Unresolved (-1/-2) is treated as a
	// Button entry, the same fallback the Add Binding menu offers.
	bool appendToEntry(MapEntry& e, const DetectedBinding& d)
	{
		const bool offerButtons = e.valueType == 0 || e.valueType < 0;
		const bool offerAxes1D  = e.valueType == 1;
		if (offerButtons)
		{
			if      (!d.key.empty())           { e.keys.push_back(d.key); return true; }
			else if (!d.mouseButton.empty())   { e.mouseButtons.push_back(d.mouseButton); return true; }
			else if (!d.gamepadButton.empty()) { e.gamepadButtons.push_back(d.gamepadButton); return true; }
			return false;
		}
		if (offerAxes1D)
		{
			if (!d.key.empty())
			{
				AxisRow r; r.positive = d.key;
				e.axes.push_back(std::move(r));
				return true;
			}
			if (!d.gamepadButton.empty())
			{
				AxisRow r; r.uiPadRow = true; r.positiveButton = d.gamepadButton;
				e.axes.push_back(std::move(r));
				return true;
			}
			if (d.axisSource >= 0)
			{
				AxisRow r; r.source = static_cast<::AxisSource>(d.axisSource);
				e.axes.push_back(std::move(r));
				return true;
			}
		}
		return false;
	}
} // namespace

bool applyDetected(MapEntry& e, BindSlot slot, int sub, const DetectedBinding& d)
{
	if (!bindSlotAccepts(slot, d)) return false;
	switch (slot)
	{
	case BindSlot::EntryAny:    return appendToEntry(e, d);
	case BindSlot::Key:         return replaceAt(e.keys, sub, d.key);
	case BindSlot::PadButton:   return replaceAt(e.gamepadButtons, sub, d.gamepadButton);
	case BindSlot::MouseButton: return replaceAt(e.mouseButtons, sub, d.mouseButton);
	case BindSlot::AxisKeyPos:
		if (AxisRow* r = axisRowAt(e, sub)) { r->positive = d.key; return true; }
		return false;
	case BindSlot::AxisKeyNeg:
		if (AxisRow* r = axisRowAt(e, sub)) { r->negative = d.key; return true; }
		return false;
	case BindSlot::AxisPadPos:
		if (AxisRow* r = axisRowAt(e, sub))
		{ r->positiveButton = d.gamepadButton; r->uiPadRow = true; r->source = ::AxisSource::Key; return true; }
		return false;
	case BindSlot::AxisPadNeg:
		if (AxisRow* r = axisRowAt(e, sub))
		{ r->negativeButton = d.gamepadButton; r->uiPadRow = true; r->source = ::AxisSource::Key; return true; }
		return false;
	case BindSlot::AxisSource:
		if (AxisRow* r = axisRowAt(e, sub))
		{
			r->source   = static_cast<::AxisSource>(d.axisSource);
			r->uiPadRow = false;
			// The runtime reads a Key row's pairs whatever the source says, so
			// a row that becomes a stick must drop them or it would keep
			// binding invisibly (same rule as the source combo).
			r->positive.clear();       r->negative.clear();
			r->positiveButton.clear(); r->negativeButton.clear();
			return true;
		}
		return false;
	case BindSlot::None: break;
	}
	return false;
}

} // namespace HE::Ed
