// macOS native menu bar (see MacMenuBar.h). ObjC++ — compiled on APPLE only.
#include "MacMenuBar.h"
#include "HorizonVersion.h"   // HE_VERSION_STRING / HE_VERSION_CODENAME
#include "OutlinerPanel.h"    // entityPresetTable — the Entity ▸ Create rows
#include "ViewportPanel.h"    // showFlagFields — the View ▸ Show rows
#include "EditorCamera.h"     // ViewPreset + presetName — the View ▸ Camera rows
#include <Renderer/IRenderer.h> // HE::ViewMode + viewModeName — the View ▸ View Mode rows

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>         // SDL_GetBasePath — locate the bundled logo
#ifdef HE_HAVE_LIBSSH2
#include <ContentManager/ContentManager.h> // ContentManager::isEngineContentDevMode
#endif
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
	// A command with the argument its row carries (0 for most rows; a view
	// mode, a show flag, a recent-project index for the ones that need it).
	struct Pending { MacMenuBar::Cmd cmd; int arg; };
	std::deque<Pending>         s_queue;      // menu actions run on the main thread
	int                         s_lastArg = 0;
	std::vector<NSMenuItem*>    s_projectItems; // enabled only with a project loaded
	// Items that show an on/off tick (the Window-menu panels, the View
	// switches, the tutorial). Small enough that a linear lookup beats a map,
	// and it keeps the item registration in one place: heAddItem records,
	// setToggleState finds.
	struct ToggleItem { MacMenuBar::Cmd cmd; int arg; NSMenuItem* item; };
	std::vector<ToggleItem>     s_toggleItems;
	// Rows that only mean something in a GAME: scene operations, the ground grid,
	// the level script, the whole Entity menu. Hidden wholesale in an application
	// project — see MacMenuBar::setAppProject.
	std::vector<NSMenuItem*>    s_gameOnlyItems;
	NSMenu*                     s_recentMenu = nil;
	std::vector<std::string>    s_recentPaths;
	bool s_installed     = false;
	bool s_projectLoaded = false;
	bool s_appProject    = false;
}

// Menu target: every custom item routes here; the Cmd rides in the item's tag,
// the argument (when the row has one) in its representedObject.
@interface HEMenuTarget : NSObject
- (void)fire:(id)sender;
@end
@implementation HEMenuTarget
- (void)fire:(id)sender
{
	NSMenuItem* item = (NSMenuItem*)sender;
	const int arg = [item.representedObject isKindOfClass:[NSNumber class]]
	              ? [(NSNumber*)item.representedObject intValue] : 0;
	s_queue.push_back({ static_cast<MacMenuBar::Cmd>(item.tag), arg });
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
                             NSString* key, NSEventModifierFlags mods, bool needsProject,
                             int arg = 0)
{
	NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title
	                                            action:@selector(fire:)
	                                     keyEquivalent:key ? key : @""];
	if (mods) it.keyEquivalentModifierMask = mods;
	it.target = s_target;
	it.tag = static_cast<NSInteger>(cmd);
	if (arg) it.representedObject = @(arg);
	[menu addItem:it];
	if (needsProject) s_projectItems.push_back(it);
	return it;
}

// A row with a tick: registered for setToggleState.
static NSMenuItem* heAddToggle(NSMenu* menu, NSString* title, MacMenuBar::Cmd cmd,
                               bool needsProject, int arg = 0)
{
	NSMenuItem* it = heAddItem(menu, title, cmd, nil, 0, needsProject, arg);
	s_toggleItems.push_back({ cmd, arg, it });
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
		// Filled by setRecentProjects; empty until the editor has read its
		// config, and rebuilt whenever the list changes.
		{
			NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"Recent Projects"
			                                                action:nil keyEquivalent:@""];
			s_recentMenu = [[NSMenu alloc] initWithTitle:@"Recent Projects"];
			s_recentMenu.autoenablesItems = NO;
			holder.submenu = s_recentMenu;
			[file addItem:holder];
		}
		heAddItem(file, @"Close Project", C::CloseProject, @"w", NSEventModifierFlagCommand, true);
		[file addItem:[NSMenuItem separatorItem]];
		s_gameOnlyItems.push_back(
			heAddItem(file, @"New Scene",            C::NewScene,        nil, 0, true));
		s_gameOnlyItems.push_back(
			heAddItem(file, @"Open Scene…",          C::OpenScene,       nil, 0, true));
		s_gameOnlyItems.push_back(
			heAddItem(file, @"Add Scene Additive…",  C::AddSceneAdditive, nil, 0, true));
		[file addItem:[NSMenuItem separatorItem]];
		// ⌘S saves the tab in front, ⇧⌘S saves everything — Save As has to move
		// off ⇧⌘S, or the menu would swallow that keystroke before Save All ever
		// sees it (a key equivalent wins over anything the app does with the key).
		heAddItem(file, @"Save",                 C::Save,        @"s", NSEventModifierFlagCommand, true);
		heAddItem(file, @"Save All",             C::SaveAll,     @"s",
		          NSEventModifierFlagCommand | NSEventModifierFlagShift, true);
		s_gameOnlyItems.push_back(
			heAddItem(file, @"Save Scene As…",       C::SaveSceneAs, @"s",
			          NSEventModifierFlagCommand | NSEventModifierFlagOption, true));
		[file addItem:[NSMenuItem separatorItem]];
		// Import lives with the assets (Assets ▸ Import Asset…) and is offered
		// here too: File is where somebody who has never seen this editor
		// looks for "get a file in".
		heAddItem(file, @"Import Asset…", C::ImportAsset, nil, 0, true);
	}

	// ── Edit ───────────────────────────────────────────────────────────────
	// NO KEY EQUIVALENTS here, and that is the whole design of this block. A
	// native ⌘Z wins over anything the app does with the key (the same rule that
	// forced Save As off ⇧⌘S above), so it would reach this menu INSTEAD of the
	// editor — and every panel with its own undo stack (material graph, UI
	// editor, HorizonCode canvas) plus every text field would lose the key to a
	// command that only knows about the scene. ⌘Z keeps going to the app, which
	// routes it per context; these items are the visible door onto the same
	// scene stack the footer buttons drive. The same holds for ⌘X/⌘C/⌘V/⌘D and
	// Delete: the rows act on the selected ENTITY, the keys stay with the app.
	{
		NSMenu* edit = heAddSubmenu(main, @"Edit");
		heAddItem(edit, @"Undo", C::Undo, nil, 0, true);
		heAddItem(edit, @"Redo", C::Redo, nil, 0, true);
		[edit addItem:[NSMenuItem separatorItem]];
		s_gameOnlyItems.push_back(heAddItem(edit, @"Cut",       C::Cut,       nil, 0, true));
		s_gameOnlyItems.push_back(heAddItem(edit, @"Copy",      C::Copy,      nil, 0, true));
		s_gameOnlyItems.push_back(heAddItem(edit, @"Paste",     C::Paste,     nil, 0, true));
		s_gameOnlyItems.push_back(heAddItem(edit, @"Duplicate", C::Duplicate, nil, 0, true));
		s_gameOnlyItems.push_back(heAddItem(edit, @"Delete",    C::Delete,    nil, 0, true));
		[edit addItem:[NSMenuItem separatorItem]];
		// The project's own settings, an editor tab like Preferences (which sits
		// in the app menu, where macOS keeps an application's preferences). No
		// key equivalent, for the reason the whole block has none.
		heAddItem(edit, @"Project Settings…", C::ProjectSettings, nil, 0, true);
	}

	// ── Entity (game projects only — hidden whole in an application) ──────
	// The verbs the viewport's and the Outliner's right-click menus carry, in
	// the bar so they have a fixed address. Built from the Outliner's preset
	// table, so the Create list here IS the Outliner's.
	{
		NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"Entity" action:nil keyEquivalent:@""];
		NSMenu* entity = [[NSMenu alloc] initWithTitle:@"Entity"];
		entity.autoenablesItems = NO;
		holder.submenu = entity;
		[main addItem:holder];
		s_gameOnlyItems.push_back(holder);

		NSMenuItem* createHolder = [[NSMenuItem alloc] initWithTitle:@"Create" action:nil keyEquivalent:@""];
		NSMenu* create = [[NSMenu alloc] initWithTitle:@"Create"];
		create.autoenablesItems = NO;
		createHolder.submenu = create;
		[entity addItem:createHolder];
		s_projectItems.push_back(createHolder);
		{
			int n = 0;
			const OutlinerPanel::EntityPresetRow* rows = OutlinerPanel::entityPresetTable(n);
			NSMenu* group = nil;
			const char* groupName = "";
			for (int i = 0; i < n; ++i)
			{
				// A row with a group goes into that group's submenu, opened on
				// the first row that names it; a top-level row after a group
				// closes it (the table is grouped contiguously).
				if (std::string(rows[i].group) != groupName)
				{
					// A separator where the list changes from loose rows to
					// groups or back — the same two the ImGui list draws.
					if (!*groupName || !*rows[i].group)
						[create addItem:[NSMenuItem separatorItem]];
					groupName = rows[i].group;
					group = nil;
					if (*groupName)
					{
						NSMenuItem* gh = [[NSMenuItem alloc]
							initWithTitle:[NSString stringWithUTF8String:groupName]
							       action:nil keyEquivalent:@""];
						group = [[NSMenu alloc] initWithTitle:gh.title];
						group.autoenablesItems = NO;
						gh.submenu = group;
						[create addItem:gh];
					}
				}
				// arg is 1-based on the wire (0 = "no argument"); the dispatch
				// takes one off again.
				heAddItem(group ? group : create, [NSString stringWithUTF8String:rows[i].label],
				          C::CreateEntity, nil, 0, false, i + 1);
			}
		}
		[entity addItem:[NSMenuItem separatorItem]];
		heAddItem(entity, @"Focus Selected",   C::FocusSelected,   nil, 0, true);
		heAddItem(entity, @"Snap to Ground",   C::SnapToGround,    nil, 0, true);
		[entity addItem:[NSMenuItem separatorItem]];
		heAddItem(entity, @"Hide Selected",    C::HideSelected,    nil, 0, true);
		heAddItem(entity, @"Isolate Selected", C::IsolateSelected, nil, 0, true);
		heAddItem(entity, @"Show All",         C::ShowAll,         nil, 0, true);
		[entity addItem:[NSMenuItem separatorItem]];
		heAddItem(entity, @"Group",            C::Group,           nil, 0, true);
		heAddItem(entity, @"Ungroup",          C::Ungroup,         nil, 0, true);
		[entity addItem:[NSMenuItem separatorItem]];
		// Retitled "Unlock" by setItemTitle while the primary is locked.
		heAddItem(entity, @"Lock",             C::ToggleLock,      nil, 0, true);
		[entity addItem:[NSMenuItem separatorItem]];
		heAddItem(entity, @"Save as Prefab",   C::SaveAsPrefab,    nil, 0, true);
	}

	// ── Assets ─────────────────────────────────────────────────────────────
	{
		NSMenu* assets = heAddSubmenu(main, @"Assets");
		heAddItem(assets, @"Create Asset…",  C::CreateAsset,   nil, 0, true);
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
	}

	// ── Play ───────────────────────────────────────────────────────────────
	// The toolbar's transport, with a fixed place in the bar. No key
	// equivalents (⌘P and friends are read by the editor through SDL and
	// rebindable in Preferences ▸ Shortcuts); the titles follow the state.
	{
		NSMenu* play = heAddSubmenu(main, @"Play");
		s_toggleItems.push_back({ C::PlayToggle, 0,
			heAddItem(play, @"Play",       C::PlayToggle,  nil, 0, true) });
		s_toggleItems.push_back({ C::PauseToggle, 0,
			heAddItem(play, @"Pause",      C::PauseToggle, nil, 0, true) });
		heAddItem(play, @"Step Frame", C::StepFrame,   nil, 0, true);
		heAddItem(play, @"Step Node",  C::StepNode,    nil, 0, true);
	}

	// ── Build ──────────────────────────────────────────────────────────────
	{
		NSMenu* build = heAddSubmenu(main, @"Build");
		// Mirrors EditorUI.cpp's ImGui Build menu. Project-scoped only: whether
		// this project HAS a native module is answered by the action, because
		// the menu is built once and the project changes under it.
		heAddItem(build, @"Build and Reload Game Logic", C::BuildGameLogic, nil, 0, true);
		[build addItem:[NSMenuItem separatorItem]];
		heAddItem(build, @"Export Project…", C::ExportProject, nil, 0, true);
	}

	// ── View: how the Scene window draws ───────────────────────────────────
	{
		NSMenu* view = heAddSubmenu(main, @"View");
		// View Mode ▸ — one row per HE::ViewMode, ticked for the current one.
		{
			NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"View Mode" action:nil keyEquivalent:@""];
			NSMenu* modes = [[NSMenu alloc] initWithTitle:@"View Mode"];
			modes.autoenablesItems = NO;
			holder.submenu = modes;
			[view addItem:holder];
			s_gameOnlyItems.push_back(holder);
			for (int m = 0; m < HE::kViewModeCount; ++m)
			{
				const auto mode = static_cast<HE::ViewMode>(m);
				if (mode == HE::ViewMode::GBufferBaseColor) [modes addItem:[NSMenuItem separatorItem]];
				heAddToggle(modes, [NSString stringWithUTF8String:HE::viewModeName(mode)],
				            C::SetViewMode, true, m + 1);
			}
		}
		// Show ▸ — the overlay switches, from the same table the toolbar's
		// popup and the config round-trip read.
		{
			NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"Show" action:nil keyEquivalent:@""];
			NSMenu* show = [[NSMenu alloc] initWithTitle:@"Show"];
			show.autoenablesItems = NO;
			holder.submenu = show;
			[view addItem:holder];
			s_gameOnlyItems.push_back(holder);
			int n = 0;
			const ViewportPanel::ShowFlagField* fields = ViewportPanel::showFlagFields(n);
			for (int i = 0; i < n; ++i)
				heAddToggle(show, [NSString stringWithUTF8String:fields[i].label],
				            C::ToggleShowFlag, true, i + 1);
			[show addItem:[NSMenuItem separatorItem]];
			heAddItem(show, @"Show All Overlays", C::ShowAllOverlays, nil, 0, true);
			heAddItem(show, @"Hide All Overlays", C::HideAllOverlays, nil, 0, true);
		}
		// Camera ▸ — the axis presets and the lens, over the Scene window's
		// camera (the keypad and the toolbar's View cell do the same).
		{
			NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"Camera" action:nil keyEquivalent:@""];
			NSMenu* cam = [[NSMenu alloc] initWithTitle:@"Camera"];
			cam.autoenablesItems = NO;
			holder.submenu = cam;
			[view addItem:holder];
			s_gameOnlyItems.push_back(holder);
			using VP = EditorCamera::ViewPreset;
			const VP presets[] = { VP::Perspective, VP::Top, VP::Bottom, VP::Front,
			                       VP::Back, VP::Right, VP::Left };
			for (VP p : presets)
			{
				heAddToggle(cam, [NSString stringWithUTF8String:EditorCamera::presetName(p)],
				            C::SetViewPreset, true, static_cast<int>(p) + 1);
				if (p == VP::Perspective) [cam addItem:[NSMenuItem separatorItem]];
			}
			[cam addItem:[NSMenuItem separatorItem]];
			heAddToggle(cam, @"Orthographic", C::ToggleOrthographic, true);
		}
		[view addItem:[NSMenuItem separatorItem]];
		NSMenuItem* fs = [[NSMenuItem alloc]
			initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
		fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
		[view addItem:fs];   // responder chain → the key window
	}

	// ── Window: everything that opens a panel or a tab ─────────────────────
	// The standard minimize/zoom first (registered so macOS lists windows),
	// then the editor's panels, ticked while open (setToggleState).
	{
		NSMenu* window = heAddSubmenu(main, @"Window");
		NSMenuItem* mini = [[NSMenuItem alloc]
			initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
		[window addItem:mini];
		NSMenuItem* zoom = [[NSMenuItem alloc]
			initWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
		[window addItem:zoom];
		NSApp.windowsMenu = window;
		[window addItem:[NSMenuItem separatorItem]];
		heAddToggle(window, @"Console",               C::ToggleConsole,       false);
		heAddToggle(window, @"Performance Profiler",  C::ToggleProfiler,      false);
		heAddToggle(window, @"Environment",           C::ToggleEnvironment,   false);
		heAddToggle(window, @"Collaboration",         C::ToggleCollab,        false);
		heAddToggle(window, @"Source Control",        C::ToggleSourceControl, true);
		heAddToggle(window, @"Audio Mixer",           C::ToggleAudioMixer,    false);
		heAddToggle(window, @"Undo History",          C::ToggleUndoHistory,   false);
		heAddToggle(window, @"Watch",                 C::ToggleWatch,         false);
		// The secondary scene viewports, ticked while open like the panels above.
		{
			[window addItem:[NSMenuItem separatorItem]];
			const struct { C cmd; NSString* title; } panes[] = {
				{ C::ToggleScene2, @"Scene 2" },
				{ C::ToggleScene3, @"Scene 3" },
				{ C::ToggleScene4, @"Scene 4" },
			};
			for (const auto& p : panes)
				s_gameOnlyItems.push_back(heAddToggle(window, p.title, p.cmd, true));
		}
		[window addItem:[NSMenuItem separatorItem]];
		s_gameOnlyItems.push_back(
			heAddItem(window, @"Level Script",   C::OpenLevelScript,  nil, 0, true));
		// The Game Instance stays: it is the one script an application really does
		// own — the thing whose OnInit builds its interface.
		heAddItem(window, @"Game Instance",  C::OpenGameInstance, nil, 0, true);
		[window addItem:[NSMenuItem separatorItem]];
		s_gameOnlyItems.push_back(heAddToggle(window, @"Landscape Tools", C::ToggleLandscapeTools, true));
		[window addItem:[NSMenuItem separatorItem]];
		heAddItem(window, @"Reset Layout", C::ResetLayout, nil, 0, false);
	}

	// ── Help (last, as macOS expects) ──────────────────────────────────────
	// Deliberately NOT project-scoped: the guided tour is exactly what someone
	// with no project open is most likely to reach for.
	{
		NSMenu* help = heAddSubmenu(main, @"Help");
		// The offline manual first — the item most of this menu exists for. No
		// key equivalent: F1 is wired inside the editor (EditorUI), and a native
		// one would swallow the keystroke before the panel that is meant to
		// answer it in context ever sees it, exactly like the Edit menu's ⌘Z.
		heAddItem(help, @"Documentation",             C::Documentation,       nil, 0, false);
		heAddItem(help, @"Search the Documentation…", C::SearchDocumentation, nil, 0, false);
		heAddItem(help, @"Documentation (Website)",   C::DocumentationOnline, nil, 0, false);
		[help addItem:[NSMenuItem separatorItem]];
		heAddToggle(help, @"Interactive Tutorial", C::OpenTutorial, false);
		[help addItem:[NSMenuItem separatorItem]];
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
	s_projectLoaded = loaded;   // a few dozen items; cheap enough to set every frame
	for (NSMenuItem* it : s_projectItems) it.enabled = loaded ? YES : NO;
}

void setAppProject(bool isApp)
{
	// Called every frame like setProjectLoaded, so it only touches the items when
	// the answer actually changed: assigning `hidden` marks the menu for redisplay.
	if (isApp == s_appProject) return;
	s_appProject = isApp;
	for (NSMenuItem* it : s_gameOnlyItems) it.hidden = isApp ? YES : NO;
}

void setToggleState(Cmd cmd, int arg, bool on)
{
	for (const ToggleItem& t : s_toggleItems)
	{
		if (t.cmd != cmd || t.arg != arg) continue;
		const NSControlStateValue want = on ? NSControlStateValueOn : NSControlStateValueOff;
		// Only on a change: assigning re-marks the menu as needing display, and
		// this is called every frame for every toggle.
		if (t.item.state != want) t.item.state = want;
		return;
	}
}

void setToggleState(Cmd cmd, bool on) { setToggleState(cmd, 0, on); }

void setItemTitle(Cmd cmd, const char* title)
{
	if (!title) return;
	NSMenu* main = NSApp.mainMenu;
	if (!main) return;
	// The retitled rows are all top-level rows of one menu (Play, Entity), so
	// one level of submenus is as deep as this has to look.
	for (NSMenuItem* top in main.itemArray)
	{
		for (NSMenuItem* it in top.submenu.itemArray)
		{
			if (it.tag != static_cast<NSInteger>(cmd) || it.representedObject) continue;
			NSString* want = [NSString stringWithUTF8String:title];
			if (![it.title isEqualToString:want]) it.title = want;
			return;
		}
	}
}

void setRecentProjects(const std::vector<std::string>& paths)
{
	if (!s_recentMenu || paths == s_recentPaths) return;
	s_recentPaths = paths;
	[s_recentMenu removeAllItems];
	if (paths.empty())
	{
		NSMenuItem* none = [[NSMenuItem alloc] initWithTitle:@"No Recent Projects"
		                                              action:nil keyEquivalent:@""];
		none.enabled = NO;
		[s_recentMenu addItem:none];
		return;
	}
	for (std::size_t i = 0; i < paths.size(); ++i)
	{
		std::string name = std::filesystem::path(paths[i]).stem().string();
		if (name.empty()) name = paths[i];
		// The row is the project's name; the full path is the tooltip, since
		// two projects called "Demo" in different folders are an ordinary thing.
		NSMenuItem* it = heAddItem(s_recentMenu, [NSString stringWithUTF8String:name.c_str()],
		                           Cmd::OpenRecentProject, nil, 0, false, static_cast<int>(i) + 1);
		it.toolTip = [NSString stringWithUTF8String:paths[i].c_str()];
		std::error_code ec;
		if (!std::filesystem::exists(paths[i], ec)) it.enabled = NO;
	}
}

Cmd take()
{
	if (s_queue.empty()) return Cmd::None;
	const Pending p = s_queue.front();
	s_queue.pop_front();
	s_lastArg = p.arg;
	return p.cmd;
}

int arg() { return s_lastArg; }

} // namespace MacMenuBar
