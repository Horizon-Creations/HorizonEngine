// The system's reduce-motion setting everywhere but macOS (see
// EditorSystemMotion.h; the macOS one is EditorSystemMotion.mm).
#include "EditorSystemMotion.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace HE::Ed
{
bool systemReducesMotionQuery()
{
#ifdef _WIN32
	// "Animation effects" off in Settings clears this. A failed call keeps the
	// full motion: reduced is a choice the user made, not a fallback.
	BOOL animate = TRUE;
	if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0)) return false;
	return animate == FALSE;
#else
	return false;
#endif
}
}
