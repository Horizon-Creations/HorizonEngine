#include "ExportSdkGateUi.h"
#include "EditorTheme.h"

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace ExportSdkGateUi
{
namespace Theme = HE::Ed::Theme;

ToolSetupDialog::Tool toolFor(const HE::ExportSdk::ExportGate& gate)
{
	return gate.needCmake ? ToolSetupDialog::Tool::Cmake : ToolSetupDialog::Tool::Compiler;
}

void drawGameCompileStatus(AppContext& ctx, const HE::ExportSdk::ExportGate& gate)
{
#ifdef HE_IMGUI_ENABLED
	using HE::ExportSdk::GateState;
	switch (gate.state)
	{
	case GateState::Ready:
		ImGui::TextDisabled("Game compiled from the engine SDK (%s): %s", gate.profile.c_str(),
		                    gate.sdk.root.string().c_str());
		break;
	case GateState::Waiting:
		ImGui::TextDisabled("Game compiled from the engine SDK (%s): checking for cmake and a "
		                    "C++ compiler\xe2\x80\xa6", gate.profile.c_str());
		break;
	case GateState::NeedTools:
	{
		// The banner's "Set Up…" is the same way out the Export button takes.
		const std::string msg = "The game is compiled from the engine SDK and this machine has " +
		                        std::string(gate.needCmake && gate.needCompiler ? "no cmake and no C++ compiler"
		                                    : gate.needCmake ? "no cmake" : "no working C++ compiler") +
		                        ". Export opens Tool Setup to install it, then continues.";
		ToolSetupDialog::drawMissingBanner(ctx, toolFor(gate), msg.c_str());
		break;
	}
	case GateState::Blocked:
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
		ImGui::TextWrapped("Cannot export: %s.", gate.why.c_str());
		ImGui::PopStyleColor();
		break;
	}
#else
	(void)ctx; (void)gate;
#endif
}
} // namespace ExportSdkGateUi
