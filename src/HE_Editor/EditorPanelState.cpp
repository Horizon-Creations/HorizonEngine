#include "EditorPanelState.h"
#include "EditorApplication.h"      // AppContext
#include <ContentManager/ContentManager.h>
#include <filesystem>

HE::UUID openPanelAsset(AppContext& ctx, const std::string& absPath,
                        std::string& nameOut, std::string& relPathOut)
{
	// No ContentManager yet (project still loading): leave the tab unloaded so the
	// next frame retries — the panels gate their lazy open on that.
	if (!ctx.contentManager) return {};

	nameOut = std::filesystem::path(absPath).filename().string();
	const std::string rel = ctx.contentManager->toContentRelativePath(absPath);
	relPathOut = rel.empty() ? absPath : rel;
	return ctx.contentManager->loadAsset(relPathOut);
}
