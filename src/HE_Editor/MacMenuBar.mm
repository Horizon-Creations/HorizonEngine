// macOS native menu bar (see MacMenuBar.h). ObjC++ — compiled on APPLE only.
#include "MacMenuBar.h"
#include "HorizonVersion.h"   // HE_VERSION_STRING / HE_VERSION_CODENAME

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>         // SDL_GetBasePath — locate the bundled logo
#ifdef HE_HAVE_LIBSSH2
#include <ContentManager/ContentManager.h> // ContentManager::isEngineContentDevMode
#endif
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace
{
	std::deque<MacMenuBar::Cmd> s_queue;      // menu actions run on the main thread
	std::vector<NSMenuItem*>    s_projectItems; // enabled only with a project loaded
	// Items that show an on/off tick (the View-menu panels + the tutorial).
	// Small enough that a linear lookup beats a map, and it keeps the item
	// registration in one place: heAddItem records, setToggleState finds.
	std::vector<std::pair<MacMenuBar::Cmd, NSMenuItem*>> s_toggleItems;
	bool s_installed     = false;
	bool s_projectLoaded = false;
}

// Menu target: every custom item routes here; the Cmd rides in the item's tag.
@interface HEMenuTarget : NSObject
- (void)fire:(id)sender;
@end
@implementation HEMenuTarget
- (void)fire:(id)sender
{
	s_queue.push_back(static_cast<MacMenuBar::Cmd>([(NSMenuItem*)sender tag]));
}
// About panel driven by the compile-time version macros so it shows the right
// release regardless of whether the editor runs as a bare exe (no Info.plist) or
// the packaged .app. Renders as: "Horizon Editor  /  Version 0.3.0 (Aurora)".
- (void)showAbout:(id)sender
{
	NSMutableDictionary* opts = [@{
		NSAboutPanelOptionApplicationName    : @"Horizon Editor",
		NSAboutPanelOptionApplicationVersion : @HE_VERSION_STRING,   // → "Version 0.3.0"
		NSAboutPanelOptionVersion            : @HE_VERSION_CODENAME, // → "(Aurora)"
	} mutableCopy];

	// Run as a bare exe (no .app bundle) the panel would fall back to the generic
	// application icon (a blank document/folder). Show the editor's own logo — the
	// same HC_Logo.png the Project Hub loads — when we can find and decode it.
	if (const char* base = SDL_GetBasePath())
	{
		NSString* logoPath = [NSString stringWithUTF8String:base];
		logoPath = [logoPath stringByAppendingString:@"Images/HC_Logo.png"];
		if (NSImage* logo = [[NSImage alloc] initWithContentsOfFile:logoPath])
			opts[NSAboutPanelOptionApplicationIcon] = logo;
	}

	[NSApp orderFrontStandardAboutPanelWithOptions:opts];
}
@end

static HEMenuTarget* s_target = nil;

static NSMenuItem* heAddItem(NSMenu* menu, NSString* title, MacMenuBar::Cmd cmd,
                             NSString* key, NSEventModifierFlags mods, bool needsProject)
{
	NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title
	                                            action:@selector(fire:)
	                                     keyEquivalent:key ? key : @""];
	if (mods) it.keyEquivalentModifierMask = mods;
	it.target = s_target;
	it.tag = static_cast<NSInteger>(cmd);
	[menu addItem:it];
	if (needsProject) s_projectItems.push_back(it);
	return it;
}

static NSMenu* heAddSubmenu(NSMenu* mainMenu, NSString* title)
{
	NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
	NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
	menu.autoenablesItems = NO;   // we drive enabled-state via setProjectLoaded
	holder.submenu = menu;
	[mainMenu addItem:holder];
	return menu;
}

namespace MacMenuBar
{

void install()
{
	if (s_installed || NSApp == nil) return;
	s_target = [HEMenuTarget new];

	NSMenu* main = [[NSMenu alloc] initWithTitle:@"MainMenu"];
	using C = Cmd;

	// ── App menu (bold, next to the Apple symbol) ──────────────────────────
	{
		NSMenu* app = heAddSubmenu(main, @"HorizonEditor");
		NSMenuItem* about = [[NSMenuItem alloc]
			initWithTitle:@"About Horizon Editor"
			       action:@selector(showAbout:) keyEquivalent:@""];
		about.target = s_target;
		[app addItem:about];
		[app addItem:[NSMenuItem separatorItem]];
		// Needs a project: Preferences opens as an editor tab, and the tab strip
		// only exists once a project is loaded (the hub has no tabs).
		heAddItem(app, @"Preferences…", C::Preferences, @",", NSEventModifierFlagCommand, true);
		[app addItem:[NSMenuItem separatorItem]];
		NSMenuItem* hide = [[NSMenuItem alloc]
			initWithTitle:@"Hide HorizonEditor" action:@selector(hide:) keyEquivalent:@"h"];
		hide.target = NSApp;
		[app addItem:hide];
		NSMenuItem* hideOthers = [[NSMenuItem alloc]
			initWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
		hideOthers.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
		hideOthers.target = NSApp;
		[app addItem:hideOthers];
		[app addItem:[NSMenuItem separatorItem]];
		// Quit uses the standard -terminate:, which SDL overrides to post
		// SDL_EVENT_QUIT. That is the ONE path both a menu click AND the ⌘Q
		// key-equivalent reliably trigger — a custom action (via fire:) fires only
		// on click, never on the key-equivalent, which is why ⌘Q did nothing. The
		// editor's unsaved-changes guard keys off SDL_EVENT_QUIT (see
		// EditorApplication::OnEvent), so the save prompt still appears; this does
		// not bypass it. That guard covers a dirty scene AND dirty asset tabs —
		// this menu item is the only quit path on macOS (the guarded in-window
		// File ▸ Exit is not drawn while the native menu is up), so anything the
		// guard does not test is silently unrecoverable here.
		NSMenuItem* quit = [[NSMenuItem alloc]
			initWithTitle:@"Quit HorizonEditor"
			       action:@selector(terminate:) keyEquivalent:@"q"];
		quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
		quit.target = NSApp;
		[app addItem:quit];
	}

	// ── File ───────────────────────────────────────────────────────────────
	{
		NSMenu* file = heAddSubmenu(main, @"File");
		heAddItem(file, @"New Project…",  C::NewProject,  @"n", NSEventModifierFlagCommand, false);
		heAddItem(file, @"Open Project…", C::OpenProject, @"o", NSEventModifierFlagCommand, false);
		heAddItem(file, @"Close Project", C::CloseProject, @"w", NSEventModifierFlagCommand, true);
		[file addItem:[NSMenuItem separatorItem]];
		heAddItem(file, @"New Scene",            C::NewScene,        nil, 0, true);
		heAddItem(file, @"Open Scene…",          C::OpenScene,       nil, 0, true);
		heAddItem(file, @"Add Scene Additive…",  C::AddSceneAdditive, nil, 0, true);
		// ⌘S saves the tab in front, ⇧⌘S saves everything — Save As has to move
		// off ⇧⌘S, or the menu would swallow that keystroke before Save All ever
		// sees it (a key equivalent wins over anything the app does with the key).
		heAddItem(file, @"Save",                 C::Save,        @"s", NSEventModifierFlagCommand, true);
		heAddItem(file, @"Save All",             C::SaveAll,     @"s",
		          NSEventModifierFlagCommand | NSEventModifierFlagShift, true);
		heAddItem(file, @"Save Scene As…",       C::SaveSceneAs, @"s",
		          NSEventModifierFlagCommand | NSEventModifierFlagOption, true);
	}

	// ── Edit ───────────────────────────────────────────────────────────────
	// Until now macOS had no Edit menu at all, so undo was reachable only through
	// ⌘Z or the footer button — and a menu bar with no Edit menu reads as an app
	// that cannot undo.
	//
	// NO KEY EQUIVALENTS here, and that is the whole design of this block. A
	// native ⌘Z wins over anything the app does with the key (the same rule that
	// forced Save As off ⇧⌘S above), so it would reach this menu INSTEAD of the
	// editor — and every panel with its own undo stack (material graph, UI
	// editor, HorizonCode canvas) plus every text field would lose the key to a
	// command that only knows about the scene. ⌘Z keeps going to the app, which
	// routes it per context; these two items are the visible door onto the same
	// scene stack the footer buttons drive.
	{
		NSMenu* edit = heAddSubmenu(main, @"Edit");
		heAddItem(edit, @"Undo", C::Undo, nil, 0, true);
		heAddItem(edit, @"Redo", C::Redo, nil, 0, true);
	}

	// ── View ───────────────────────────────────────────────────────────────
	{
		NSMenu* view = heAddSubmenu(main, @"View");
		NSMenuItem* fs = [[NSMenuItem alloc]
			initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
		fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
		[view addItem:fs];   // responder chain → the key window
		heAddItem(view, @"Reset Layout",          C::ResetLayout,    nil, 0, false);
		// The four panel toggles carry a tick showing whether the panel is open,
		// kept in step every frame by setToggleState.
		s_toggleItems.emplace_back(C::ToggleProfiler,
			heAddItem(view, @"Performance Profiler",  C::ToggleProfiler, nil, 0, false));
		s_toggleItems.emplace_back(C::ToggleEnvironment,
			heAddItem(view, @"Environment",           C::ToggleEnvironment, nil, 0, false));
		s_toggleItems.emplace_back(C::ToggleCollab,
			heAddItem(view, @"Collaboration",         C::ToggleCollab,      nil, 0, false));
		s_toggleItems.emplace_back(C::ToggleSourceControl,
			heAddItem(view, @"Source Control",        C::ToggleSourceControl, nil, 0, true));
		s_toggleItems.emplace_back(C::ToggleConsole,
			heAddItem(view, @"Console",               C::ToggleConsole,     nil, 0, false));
		// The world grid is not a panel, but it is a View toggle the user looks
		// for in this menu — on macOS the viewport toolbar's options popup is
		// otherwise its only route.
		s_toggleItems.emplace_back(C::ToggleGroundGrid,
			heAddItem(view, @"Ground Grid",           C::ToggleGroundGrid,  nil, 0, true));
		[view addItem:[NSMenuItem separatorItem]];
		heAddItem(view, @"Level Script",   C::OpenLevelScript,  nil, 0, true);
		heAddItem(view, @"Game Instance",  C::OpenGameInstance, nil, 0, true);
	}

	// ── Assets / Build ─────────────────────────────────────────────────────
	{
		NSMenu* assets = heAddSubmenu(main, @"Assets");
		heAddItem(assets, @"Import Asset…",  C::ImportAsset,   nil, 0, true);
		heAddItem(assets, @"Refresh Assets", C::RefreshAssets, nil, 0, true);
#ifdef HE_HAVE_LIBSSH2
		// Mirrors EditorUI.cpp's ImGui Assets menu exactly, including the same
		// dev-mode gate — see MacMenuBar.h's header comment for why this can't
		// be skipped just because the ImGui side already has it.
		if (ContentManager::isEngineContentDevMode())
		{
			[assets addItem:[NSMenuItem separatorItem]];
			heAddItem(assets, @"Publish Engine Content to Server…",
			          C::PublishEngineContent, nil, 0, false);
			heAddItem(assets, @"Rebuild Manifest from Server…",
			          C::RebuildManifestFromServer, nil, 0, false);
		}
#endif
		NSMenu* build = heAddSubmenu(main, @"Build");
		heAddItem(build, @"Export Project…", C::ExportProject, nil, 0, true);
	}

	// ── Window (standard minimize/zoom; registered so macOS lists windows) ──
	{
		NSMenu* window = heAddSubmenu(main, @"Window");
		NSMenuItem* mini = [[NSMenuItem alloc]
			initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
		[window addItem:mini];
		NSMenuItem* zoom = [[NSMenuItem alloc]
			initWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
		[window addItem:zoom];
		NSApp.windowsMenu = window;
	}

	// ── Help (last, as macOS expects) ──────────────────────────────────────
	// Deliberately NOT project-scoped: the guided tour is exactly what someone
	// with no project open is most likely to reach for.
	{
		NSMenu* help = heAddSubmenu(main, @"Help");
		s_toggleItems.emplace_back(C::OpenTutorial,
			heAddItem(help, @"Interactive Tutorial", C::OpenTutorial, nil, 0, false));
		[help addItem:[NSMenuItem separatorItem]];
		heAddItem(help, @"Documentation",  C::Documentation, nil, 0, false);
		heAddItem(help, @"Report Issue…", C::ReportIssue, nil, 0, false);
		NSApp.helpMenu = help;
	}

	NSApp.mainMenu = main;
	setProjectLoaded(false);
	s_installed = true;
}

bool available() { return s_installed; }

void setProjectLoaded(bool loaded)
{
	s_projectLoaded = loaded;   // ~10 items; cheap enough to set every frame
	for (NSMenuItem* it : s_projectItems) it.enabled = loaded ? YES : NO;
}

void setToggleState(Cmd cmd, bool on)
{
	for (const auto& [itemCmd, item] : s_toggleItems)
	{
		if (itemCmd != cmd) continue;
		const NSControlStateValue want = on ? NSControlStateValueOn : NSControlStateValueOff;
		// Only on a change: assigning re-marks the menu as needing display, and
		// this is called every frame for every toggle.
		if (item.state != want) item.state = want;
		return;
	}
}

Cmd take()
{
	if (s_queue.empty()) return Cmd::None;
	const Cmd c = s_queue.front();
	s_queue.pop_front();
	return c;
}

} // namespace MacMenuBar
