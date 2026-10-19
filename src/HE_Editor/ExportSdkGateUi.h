#pragma once
// The export dialog's line about the game compile (topic 222 step 3): every
// export compiles its game from the engine SDK, so this is not a checkbox any
// more but what the export will do, or what stops it. Its own file so the
// headless UI test can draw it with a made-up toolchain probe; the export panel
// itself is not linked into he_tests.
#include "ExportSdkBuild.h"
#include "ToolSetupDialog.h"

struct AppContext;

namespace ExportSdkGateUi
{
// The sentence Tool Setup shows above its rows when an export opened it.
inline constexpr const char* kToolSetupPurpose =
    "An export is waiting: the game is compiled from the engine SDK, which needs "
    "cmake and a C++ compiler. It continues by itself once both are installed; "
    "closing this dialog without them cancels it.";

// Which Tool Setup row a NeedTools gate is about (cmake first: the compiler
// probe runs through cmake).
ToolSetupDialog::Tool toolFor(const HE::ExportSdk::ExportGate& gate);

// Draws the status inside the current window (the dialog's column width is the
// caller's WrapText). NeedTools carries Tool Setup's "Set Up…" banner.
void drawGameCompileStatus(AppContext& ctx, const HE::ExportSdk::ExportGate& gate);
} // namespace ExportSdkGateUi
