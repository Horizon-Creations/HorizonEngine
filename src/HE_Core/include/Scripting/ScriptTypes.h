#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class ScriptPropType { Float, Int, Bool, String };

namespace HE
{
	// Gameplay scripting language of a ScriptAsset. Lua is 0 so that language-
	// tagged instance ids (language in the high byte) stay bit-identical to the
	// pre-tagging Lua-only ids.
	//
	// The VALUES are persisted — as the 1-byte CHUNK_SLNG in .hasset/.hpak and as
	// the high byte of a script InstanceId. Never renumber. The NAME is not
	// persisted anywhere, which is why moving it into HE was safe: it changes no
	// byte on disk. It used to sit at global scope, with a note warning that
	// adding an HE::ScriptLanguage alongside it would silently shadow it for code
	// inside namespace HE — that hazard only existed while both spellings did, and
	// moving it rather than duplicating it is what removed it for good.
	enum class ScriptLanguage : uint8_t { Lua = 0, Python = 1 };

	// Derive the language from an asset/file path by extension (".py" → Python,
	// everything else → Lua, the engine's default scripting language).
	inline ScriptLanguage scriptLanguageFromPath(const std::string& path)
	{
		if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".py") == 0)
			return ScriptLanguage::Python;
		return ScriptLanguage::Lua;
	}

	// ── the script log tag ───────────────────────────────────────────────────
	// One process-wide prefix naming the language the PROJECT is authored in
	// ("[HC] ", "[Lua] ", "[Python] ", "[C++] "), prepended to every log line a
	// script emits. A project has exactly one gameplay language — the editor
	// enforces that (ProjectScriptLanguage in ProjectManager.h) — so one process
	// variable is the whole model.
	//
	// It lives HERE, in HE_Core, because the two sides that must agree are both
	// below the tool layer: the HorizonCode interpreter and its generated C++
	// (HE_Core) and ScriptApi::log, the Lua/Python/registry path (HE_Scene).
	// Neither may include ProjectManager.h — that would point the dependency
	// upward — so the applications, which DO know the project, push the tag down.
	//
	// NOT a header-only inline getter over a function-local static: on Windows an
	// inline function is emitted per module, so HorizonCore.dll, HorizonScene.dll
	// and the editor executable would each get their OWN static and the editor
	// would set a tag the interpreter never reads. Declared HE_API and defined in
	// exactly one translation unit (ScriptEngine.cpp), the whole process shares
	// one variable on every platform.
	//
	// Default: EMPTY, i.e. no prefix at all. An application that never sets the
	// tag (the packaged game today, the tests, the tools) logs byte-identically
	// to before this existed, and a line that cannot name its language says
	// nothing rather than guessing — a WRONG language label is worse than none.
	HE_API void               setScriptLogTag(const std::string& tag);
	HE_API const std::string& scriptLogTag();

	// The ONE place the tag is applied. All three script log paths call this so
	// they cannot drift apart again: the interpreter's Print, the same Print in
	// generated C++, and ScriptApi::log once printed three different prefixes.
	HE_API std::string scriptLogLine(const std::string& message);
}

// UI pointer events dispatched to a UI element's behavior script. Handler
// names: Lua onClick/onHoverEnter/onHoverExit, Python on_click/on_hover_enter/
// on_hover_exit — a missing handler is a silent no-op, like the collision pair.
enum class UIScriptEvent : uint8_t { Click = 0, HoverEnter = 1, HoverExit = 2 };

// A single typed value used both as a default (in ScriptPropDef) and as a
// per-instance override stored in ScriptComponent::properties.
struct ScriptPropValue {
    ScriptPropType type = ScriptPropType::Float;
    float          f    = 0.0f;
    int            i    = 0;
    bool           b    = false;
    std::string    s;
};

// One declared property: name + default value inferred from M.properties table.
struct ScriptPropDef {
    std::string    name;
    ScriptPropValue defaultVal;
};
