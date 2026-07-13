// macOS native menu bar (see MacMenuBar.h). ObjC++ — compiled on APPLE only.
#include "MacMenuBar.h"
#include "HorizonVersion.h"   // HE_VERSION_STRING / HE_VERSION_CODENAME

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>         // SDL_GetBasePath — locate the bundled logo
#include <deque>
#include <string>
#include <vector>

namespace
{
	std::deque<MacMenuBar::Cmd> s_queue;      // menu actions run on the main thread
	std::vector<NSMenuItem*>    s_projectItems; // enabled only with a project loaded
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
// the packaged .app. Renders as: "Horizon Editor  /  Version 0.2.0 (Sunrise)".
- (void)showAbout:(id)sender
{
	NSMutableDictionary* opts = [@{
		NSAboutPanelOptionApplicationName    : @"Horizon Editor",
		NSAboutPanelOptionApplicationVersion : @HE_VERSION_STRING,   // → "Version 0.2.0"
		NSAboutPanelOptionVersion            : @HE_VERSION_CODENAME, // → "(Sunrise)"
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
		heAddItem(app, @"Preferences…", C::Preferences, @",", NSEventModifierFlagCommand, false);
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
		// not bypass it.
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
		heAddItem(file, @"Save Scene",           C::SaveScene,   @"s", NSEventModifierFlagCommand, true);
		heAddItem(file, @"Save Scene As…",       C::SaveSceneAs, @"s",
		          NSEventModifierFlagCommand | NSEventModifierFlagShift, true);
	}

	// ── View ───────────────────────────────────────────────────────────────
	{
		NSMenu* view = heAddSubmenu(main, @"View");
		NSMenuItem* fs = [[NSMenuItem alloc]
			initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
		fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
		[view addItem:fs];   // responder chain → the key window
		heAddItem(view, @"Reset Layout",          C::ResetLayout,    nil, 0, false);
		heAddItem(view, @"Performance Profiler",  C::ToggleProfiler, nil, 0, false);
		heAddItem(view, @"Environment",           C::ToggleEnvironment, nil, 0, false);
		[view addItem:[NSMenuItem separatorItem]];
		heAddItem(view, @"Level Script",   C::OpenLevelScript,  nil, 0, true);
		heAddItem(view, @"Game Instance",  C::OpenGameInstance, nil, 0, true);
	}

	// ── Assets / Build ─────────────────────────────────────────────────────
	{
		NSMenu* assets = heAddSubmenu(main, @"Assets");
		heAddItem(assets, @"Import Asset…", C::ImportAsset, nil, 0, true);
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

Cmd take()
{
	if (s_queue.empty()) return Cmd::None;
	const Cmd c = s_queue.front();
	s_queue.pop_front();
	return c;
}

} // namespace MacMenuBar
