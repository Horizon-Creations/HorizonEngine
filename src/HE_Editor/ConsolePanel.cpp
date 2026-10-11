#include "ConsolePanel.h"
#include "ConsoleView.h"
#include "EditorApplication.h"

#include "HcExecTrace.h"         // a line's node → "go to node"
#include "LevelScriptPanel.h"    // kTabPath — naming the tab a line leads to
#include "GameInstancePanel.h"   // kTabPath
#include "ScriptEditorPanel.h"   // a line's `script:line:` → "go to line"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/ScriptComponent.h>   // moduleName → the script asset
#include <Scripting/ScriptTypes.h>

#include <string>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

// The buffer, the sink and the drawing live in ConsoleView (ImGui-only, so a
// test can draw the real thing). What stays here is what needs the editor: the
// window, and the lookups that turn a row into somewhere to go.
namespace ConsolePanel
{

void attachToEngineLog()   { ConsoleView::attachToEngineLog(); }
void detachFromEngineLog() { ConsoleView::detachFromEngineLog(); }

#ifdef HE_IMGUI_ENABLED
namespace
{
	// The asset behind a script's module name, as the FULL path the tab bar
	// keys on; empty when nothing in the world runs under that name. A Lua or
	// Python script has no name of its own in the log — it runs under the
	// moduleName of the ScriptComponent that started it
	// (ScriptContext::startEntityScript), so that component is the way back:
	// its scriptAssetId, the ContentManager's script for it, that script's
	// content-relative path, resolved. Two components may share a module name
	// across languages (ScriptContext.h), so the one whose asset is in the
	// language that logged wins; anything else with the name is the fallback.
	// A component holding a HorizonCode class under the same id has no
	// ScriptAsset and is skipped.
	//
	// The getter's pointer is only good until the next loadAsset, so the path
	// is copied out at once.
	std::string scriptAssetPathFor(AppContext& ctx, const std::string& moduleName, bool python)
	{
		if (!ctx.world || !ctx.contentManager || moduleName.empty()) return {};
		const HE::ScriptLanguage want = python ? HE::ScriptLanguage::Python : HE::ScriptLanguage::Lua;
		std::string fallback;
		for (auto [entity, sc] : ctx.world->registry().view<ScriptComponent>().each())
		{
			if (sc.moduleName != moduleName) continue;
			const ScriptAsset* asset = ctx.contentManager->getScript(sc.scriptAssetId);
			if (!asset || asset->path.empty()) continue;
			const std::string rel = asset->path;
			if (asset->language == want) return ctx.contentManager->resolveAbsolutePath(rel);
			if (fallback.empty()) fallback = ctx.contentManager->resolveAbsolutePath(rel);
		}
		return fallback;
	}
}
#endif // HE_IMGUI_ENABLED

void DrawConsoleWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
	if (!open) return;

	// Before Begin, so a window the user rolled up still keeps its history
	// current instead of coming back with a gap in it.
	ConsoleView::pump();

	ImGui::SetNextWindowSize(ImVec2(860.0f, 280.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Console", &open)) { ImGui::End(); return; }

	ConsoleView::Hooks hooks;
	hooks.codeFont = ctx.codeFont;
	// The tab's name rather than the runtime's key: "level:<uuid>" tells
	// nobody anything, "Level Script" does.
	hooks.tabLabelFor = [](const std::string& key) -> std::string
	{
		const std::string where = HcExecTrace::tabKeyFor(key);
		if (where == LevelScriptPanel::kTabPath)  return "the Level Script";
		if (where == GameInstancePanel::kTabPath) return "the Game Instance";
		return where;
	};
	hooks.goToNode = [](const std::string& key, int node) { HcExecTrace::requestReveal(key, node); };
	// The asset is looked up on the click: the world can have changed since the
	// line was written, and a script nobody runs any more simply has nowhere
	// to go.
	hooks.goToLine = [&ctx](const std::string& script, int line, bool python)
	{
		const std::string path = scriptAssetPathFor(ctx, script, python);
		if (!path.empty()) ScriptEditorPanel::requestReveal(path, line);
	};
	ConsoleView::draw(hooks);

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif // HE_IMGUI_ENABLED
}

} // namespace ConsolePanel
