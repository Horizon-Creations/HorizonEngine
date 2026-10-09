// The trackpad's pinch gesture on macOS (see EditorInput.h, "The pinch gesture").
// ObjC++ — compiled on APPLE only; EditorInput.cpp carries the empty version for
// everyone else.
#include "EditorInput.h"

#import <Cocoa/Cocoa.h>

namespace EditorInput
{
void pinchPlatformInstall()
{
	// A local monitor sees the app's own events before they are dispatched and
	// hands them on untouched, so nothing that already handles a magnify event
	// (nothing does today) is starved. SDL 3.2 ignores the event; AppKit keeps
	// the monitor alive for the life of the process.
	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
	                                      handler:^NSEvent* _Nullable(NSEvent* event)
	{
		detail::g_pinchPending += static_cast<float>(event.magnification);
		return event;
	}];
}
}
