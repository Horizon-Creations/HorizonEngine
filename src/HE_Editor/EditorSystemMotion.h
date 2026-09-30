#pragma once

// The system's "reduce motion" setting — what EditorRewards' "Follow System"
// follows (EditorRewards.h, "Reduced motion"). A file of its own so the Cocoa
// and Win32 calls stay out of EditorRewards.cpp, which he_tests compiles:
// the editor hands this to Rewards::setSystemMotionQuery at start-up.
//   macOS    NSWorkspace.accessibilityDisplayShouldReduceMotion
//            (System Settings ▸ Accessibility ▸ Display ▸ Reduce motion)
//   Windows  SPI_GETCLIENTAREAANIMATION off
//            (Settings ▸ Accessibility ▸ Visual effects ▸ Animation effects)
//   other    false — no one setting to ask
namespace HE::Ed
{
	bool systemReducesMotionQuery();
}
