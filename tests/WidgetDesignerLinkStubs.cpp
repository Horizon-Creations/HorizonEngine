// ── Two symbols the widget designer links against, and nothing more ─────────
// test_widget_designer_ui.cpp drives the real UIEditorPanel headless. Of the
// sources it reaches for, two would each drag a whole panel into he_tests:
//
//   • HcGraphHost reads the variable-list look from EditorSettingsPanel, which
//     is the entire Editor Preferences window (toolchains, git, MCP setup …).
//   • UIEditorPanel hands a graph rename to HcRenameDialog, which reaches into
//     the animator editor and EditorApplication.
//
// Neither is on the path a Designer-view screenshot takes, so they are answered
// here with the defaults a fresh editor has: the Detailed variable rows, and a
// rename that finds nothing elsewhere. If he_tests ever compiles the real
// EditorSettingsPanel.cpp or HcRenameDialog.cpp, delete the matching stub —
// the linker will say so with a duplicate symbol.

#include "EditorSettingsPanel.h"
#include "HcRenameDialog.h"

namespace EditorSettingsPanel
{
	HcVariableStyle hcVariableStyle() { return HcVariableStyle::Detailed; }
}

namespace HcRenameDialog
{
	void requestAfterRename(AppContext&, const HcRename::Target&,
	                        const std::vector<std::string>&) {}
}
