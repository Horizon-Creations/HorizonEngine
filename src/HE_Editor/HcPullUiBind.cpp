// HcPullUi::bindFrom — kept apart from HcPullUi.cpp so the drawing itself
// does not need EditorApplication.h and every panel header behind it.
#include "HcPullUi.h"
#include "CollabController.h"
#include "EditorApplication.h"   // AppContext
#include "HorizonCodeClassPanel.h"
#include "UIEditorPanel.h"

namespace HcPullUi
{
void bindFrom(AppContext& ctx)
{
	Targets t;
	t.gameInstance       = ctx.gameInstanceGraph;
	t.commitGameInstance = ctx.commitGameInstance;
	t.content            = ctx.contentManager;
	// The rename dialog's rule (HcRenameDialog.cpp blockedBecause): an open tab
	// holds its own copy of the graph and would save it back over ours.
	CollabController* collab = ctx.collab;
	t.blockedBecause = [collab](const std::string& relPath) -> std::string
	{
		if (collab && collab->assetLockedByOther(relPath))
			return "locked by someone else in this session";
		if (UIEditorPanel::isDirty(relPath) || UIEditorPanel::isDirtyByContentPath(relPath) ||
		    HorizonCodeClassPanel::isDirty(relPath))
			return "open with unsaved changes";
		return {};
	};
	setTargets(std::move(t));
}
}
