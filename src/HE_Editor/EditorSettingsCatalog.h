#pragma once

// ─── The editor's settings, as a table instead of as a screen ────────────────
// Preferences draws its settings as `row("key", "Category", widget)` calls in
// EditorSettingsPanel.cpp, and that is the only place the editor has ever
// written down which knobs exist. It works for a human — the widget IS the
// documentation — and it is useless to anything that is not drawing: a caller
// that wants to READ what can be set, or set one by name, has nothing to read.
//
// This is that list, written once, with the types and the ranges the widgets
// enforce by construction. It exists because `settings_get`/`settings_set`
// needed a catalogue and the honest alternatives were both worse: hand a model
// a free-form config key (nothing would tell it "AntiAliasing" takes 0..4, and
// a 7 would be written and silently clamped at push time) or teach it to parse
// a panel that cannot be compiled without a window.
//
// ── Three things a row can be, and only the first is a plain field ───────────
//   • a field of EditorConfig — read and written here, by member pointer;
//   • a field of EditorConfig that ADDITIONALLY has to travel somewhere
//     (`apply` names which): MaxFps reaches the frame pacer through
//     AppContext::setMaxFps, and a write that only set the struct would be a
//     number in a file that changes nothing until the next restart;
//   • not a field of EditorConfig at all (`storage == External`): VSync lives on
//     the Application, the backend name on the AppContext. Those are carried by
//     the caller's own hooks — this table only says they exist and what shape
//     they have.
//
// ── The category strings are the panel's own ─────────────────────────────────
// "Display", "Post-Processing", "Global Illumination", "Effects",
// "Collaboration", "Remote Control", "Viewport", "Input", "Appearance",
// "Content Browser" — copied from the `row(...)` calls so a caller that reads
// this table and a human reading Preferences are talking about the same page.
// `row` is the panel's own group key, which is also what
// EditorConfig::QuickSettingsFavorites pins by.
//
// Deliberately free of ImGui, of SDL and of EditorApplication: a settings table
// whose claims cannot be checked without a window is a settings table whose
// claims are not checked.

#include "EditorConfig.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace HE::Ed
{

enum class SettingType
{
	Bool,
	Int,
	Float,
	String,
	Enum,     // an int with names: both the number and the name are accepted
};

enum class SettingStorage
{
	Config,    // a field of EditorConfig, reachable through the member pointers
	External,  // lives elsewhere (Application, AppContext) — the caller's hooks
};

struct SettingDesc
{
	std::string    key;        // stable address, e.g. "postprocess.bloomIntensity"
	std::string    label;      // what Preferences calls it
	std::string    category;   // the Preferences category, verbatim
	std::string    row;        // the panel's group key ("bloom"), = a pin name
	SettingType    type     = SettingType::Bool;
	SettingStorage storage  = SettingStorage::Config;
	std::string    help;
	// False = readable, never writable. Two reasons occur here and they are
	// different: a value that is not ours to change (the renderer backend is
	// chosen at startup) and a value this interface must not touch because it
	// is the interface's own switch (see kRemoteControlCategory).
	bool           writable = true;
	std::string    readOnlyReason;
	// What else a write has to reach, "" for nothing. Named rather than a
	// callback so this table stays free of the editor: the caller maps the name
	// onto the AppContext member.
	std::string    apply;      // "vsync", "maxfps"

	bool   hasRange = false;
	double minValue = 0.0;
	double maxValue = 0.0;

	std::vector<std::string> options;   // Enum: index → name

	// Exactly one of these is set for a Config row (the Enum type uses `pi`),
	// and none of them for an External one.
	bool        EditorConfig::* pb = nullptr;
	int         EditorConfig::* pi = nullptr;
	float       EditorConfig::* pf = nullptr;
	std::string EditorConfig::* ps = nullptr;
};

// The category whose rows are refused a write, always and by name: it is the
// page on which the MCP bridge itself is switched on and its port chosen. A
// tool that can turn its own listener off is useless in the best case and
// inexplicable in the worst — the human who opened that door is the one who
// closes it.
inline constexpr const char* kRemoteControlCategory = "Remote Control";

// The whole table, in the order Preferences walks its pages.
const std::vector<SettingDesc>& editorSettingCatalog();

// Null for a key that is not in the table.
const SettingDesc* findEditorSetting(const std::string& key);

// The current value as JSON — bool / int / float / string, and for an Enum the
// NAME (with the index alongside it in the tool's own result). False for an
// External row, which this file cannot read.
bool readEditorSetting(const EditorConfig& cfg, const SettingDesc& d,
                       nlohmann::json& out);

// Apply `in` to `cfg`. False + `outError` for a wrong type, an out-of-range
// number, an unknown enum name or an External row. `outChanged` reports whether
// the value actually moved — a set to what it already was is a success that
// changed nothing, and the difference matters to a caller deciding whether to
// persist.
bool writeEditorSetting(EditorConfig& cfg, const SettingDesc& d,
                        const nlohmann::json& in,
                        std::string& outError, bool& outChanged);

} // namespace HE::Ed
