// HcExtractUi::bindFrom — kept apart from HcExtractUi.cpp for HcPullUiBind's
// reason: the drawing itself does not need EditorApplication.h and every panel
// header behind it, and he_tests compiles the drawing without them.
#include "HcExtractUi.h"
#include "CollabController.h"
#include "EditorApplication.h"   // AppContext

namespace HcExtractUi
{
void bindFrom(AppContext& ctx)
{
	Targets t;
	t.content = ctx.contentManager;
	// What the content browser's Create menu and the MCP asset tools do after
	// writing a new asset: refresh the browser, publish it to a session.
	bool* refresh = &ctx.contentRefreshPending;
	CollabController* collab = ctx.collab;
	t.onAssetCreated = [refresh, collab](const std::string& rel, const std::string& abs)
	{
		*refresh = true;
		if (collab && collab->inSession()) collab->publishAssetCreate(rel, abs);
	};
	setTargets(std::move(t));
}
}
