// The system's reduce-motion setting on macOS (see EditorSystemMotion.h).
// ObjC++ — compiled on APPLE only; EditorSystemMotion.cpp is everyone else.
#include "EditorSystemMotion.h"

#import <Cocoa/Cocoa.h>

namespace HE::Ed
{
bool systemReducesMotionQuery()
{
	@autoreleasepool
	{
		return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion] == YES;
	}
}
}
