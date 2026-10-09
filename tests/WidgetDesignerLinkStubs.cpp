// ── Two symbols the widget designer links against, and nothing more ─────────
// test_widget_designer_ui.cpp drives the real UIEditorPanel headless. Of the
// sources it reaches for, two would each drag a whole panel into he_tests:
//
//   • HcGraphHost reads the variable-list look from EditorSettingsPanel, which
//     is the entire Editor Preferences window (toolchains, git, MCP setup …).
//   • UIEditorPanel hands a graph rename to HcRenameDialog, which reaches into
//     the animator editor and EditorApplication.
//
//   • UIEditorPanel binds the Pull on Construct block's write targets through
//     HcPullUi::bindFrom (HcPullUiBind.cpp), which asks the class panels and
//     EditorApplication whether an asset is open with unsaved changes.
//
// None is on the path a Designer-view screenshot takes, so they are answered
// here with the defaults a fresh editor has: the Detailed variable rows, a
// rename that finds nothing elsewhere, and no pull targets (the block then
// lists nothing and writes nothing). If he_tests ever compiles the real
// EditorSettingsPanel.cpp, HcRenameDialog.cpp or HcPullUiBind.cpp, delete the
// matching stub — the linker will say so with a duplicate symbol.

#include "EditorSettingsPanel.h"
#include "HcRenameDialog.h"
#include "HcPullUi.h"
#include "HcExtractUi.h"
#include "ViewportPanel.h"

// The profiler's Streaming tab (StreamingDebugView.cpp, test_streaming_view.cpp)
// shares the Scene window's Streaming Cells switch; the Scene window itself is
// the whole viewport. A fresh editor's flags stand in for it.
namespace ViewportPanel
{
	ShowFlags& showFlags()
	{
		static ShowFlags s_flags;
		return s_flags;
	}
}

namespace EditorSettingsPanel
{
	HcVariableStyle hcVariableStyle() { return HcVariableStyle::Detailed; }
	// UIEditorPanel asks (since 610fbcd2 moved Pre Construct into Preferences);
	// the real one reads config.json and answers true when nothing is stored.
	bool widgetRunPreConstruct() { return true; }
}

namespace HcRenameDialog
{
	void requestAfterRename(AppContext&, const HcRename::Target&,
	                        const std::vector<std::string>&) {}
}

namespace HcPullUi
{
	void bindFrom(AppContext&) { setTargets({}); }
}

// The same for Extract on Destruct's block (HcExtractUiBind.cpp): no content
// manager, so "New Struct from Variables…" writes nothing.
namespace HcExtractUi
{
	void bindFrom(AppContext&) { setTargets({}); }
}
