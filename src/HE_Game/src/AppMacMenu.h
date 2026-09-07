#pragma once
#include <UIWidget/AppMenu.h>
#include <string>
#include <vector>

// ── The application's menu bar, in the SYSTEM bar (plan A6, macOS half) ───────
// The same std::vector<HE::AppMenu> the widget layer draws as a strip on Windows
// and Linux becomes NSMenus next to the Apple symbol here. One data structure,
// two ways of showing it, and the application still says what its menus are
// exactly once.
//
// This lives in HE_Game and NOT in HE_Core on purpose: HorizonCore is linked by
// he_tests, hc_codegen, widget_gen and the Windows CI, and a Cocoa dependency
// down there would drag AppKit into all of them for the sake of one platform's
// menu bar. The editor keeps its own native bar (HE_Editor/MacMenuBar) for the
// same reason and by the same shape.
//
// Compiled only on APPLE (AppMacMenu.mm), so every call site is guarded by
// #ifdef __APPLE__ exactly like the editor's — there is no off-macOS stub to
// link against, which is the point: nothing else has to know this exists.
namespace HE::AppMacMenu {

// True once there is an NSApp with a menu bar to insert into. False before SDL
// has created the application object.
bool available();

// Replace whatever this application put in the bar with these menus. Empty
// takes ours back out again.
//
// It INSERTS rather than replaces: SDL builds its own main menu (the app menu
// with About/Services/Hide/Quit, and the Window menu with Close/Minimize/Zoom/
// Full Screen), and assigning NSApp.mainMenu would throw all of that away — for
// every shipped GAME too, not only for an application that asked for a bar. So
// ours go between the app menu and Window, which is where a Mac application's
// own menus go, and a program that never calls Add Menu keeps exactly the bar
// SDL gave it.
void set(const std::vector<HE::AppMenu>& menus);

// Take one chosen entry's id (false when there is nothing waiting). A click
// arrives on the main thread from AppKit's own run loop, in the middle of
// whatever the frame was doing, so the id waits here and the frame loop
// delivers it — the same rule the tray's clicks follow.
bool take(std::string& id);

} // namespace HE::AppMacMenu
