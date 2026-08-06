#include "doctest.h"
#include "EditorInput.h"

// The resolve is deliberately a pure header inline (the SDL hardware probe lives
// behind it in EditorInput.cpp) so the config x detection matrix is testable
// without a display or an SDL init.

TEST_CASE("EditorInput: Auto follows the detection result")
{
    CHECK(EditorInput::resolveTrackpad(EditorInput::kPointerAuto, true));
    CHECK(!EditorInput::resolveTrackpad(EditorInput::kPointerAuto, false));
}

TEST_CASE("EditorInput: explicit choices override the detection")
{
    // A docked laptop (battery detected, mouse attached) must be able to force
    // Mouse; a desktop with an unseen trackpad must be able to force Trackpad.
    CHECK(!EditorInput::resolveTrackpad(EditorInput::kPointerMouse, true));
    CHECK(!EditorInput::resolveTrackpad(EditorInput::kPointerMouse, false));
    CHECK(EditorInput::resolveTrackpad(EditorInput::kPointerTrackpad, true));
    CHECK(EditorInput::resolveTrackpad(EditorInput::kPointerTrackpad, false));
}
