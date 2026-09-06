// The application's menu bar in the macOS system bar (see AppMacMenu.h).
// ObjC++ — compiled on APPLE only.
#include "AppMacMenu.h"

#import <Cocoa/Cocoa.h>
#include <UIWidget/UIShortcut.h>
#include <cstdlib>   // std::atoi — the function row's number out of "F7"
#include <deque>
#include <string>
#include <vector>

namespace
{
	// Chosen entries wait here for the frame loop; see AppMacMenu::take.
	std::deque<std::string> s_clicks;
	// The top-level items WE put into SDL's main menu, so the next set() can take
	// exactly those back out again and leave SDL's own two alone. ARC keeps them
	// alive in here, though the menu they sit in already does.
	std::vector<NSMenuItem*> s_ours;

	// An id or a label out of a project file is whatever somebody typed. AppKit
	// wants valid UTF-8 and answers nil when it does not get it, and a nil title
	// is a menu item that cannot be read or reached — an empty one at least still
	// draws a row somebody can report.
	NSString* heStr(const std::string& s)
	{
		NSString* n = [NSString stringWithUTF8String:s.c_str()];
		return n ? n : @"";
	}

	// ── A chord, as AppKit spells one (plan B10) ────────────────────────────
	// The engine's shortcut text becomes a key equivalent, and from then on the
	// SYSTEM owns that keystroke: it draws it beside the entry, it dims it with
	// the menu, and it fires the entry through `fire:` above. The widget layer
	// knows and stays out of the way — WidgetManager::fireMenuShortcut answers
	// "yes, a menu has this key" and fires nothing while the native bar is up.
	// Both halves are needed: without the equivalent the shortcut is invisible
	// on the only platform whose bar the user reads, and without the gate over
	// there every one of them fires twice.
	//
	// The letter goes in LOWERCASE with Shift as an explicit flag. An uppercase
	// key equivalent means shift to AppKit all by itself, so "S" plus no flag
	// and "s" plus the flag are the same chord written two ways — and the first
	// spelling makes a plain Cmd+S impossible to express.
	//
	// Empty means "this chord cannot be a key equivalent", and then the row gets
	// none rather than a wrong one.
	NSString* heKeyEquivalent(const HE::UIShortcut& sc)
	{
		const std::string& k = sc.key;
		if (k.empty()) return @"";
		if (k.size() == 1)
		{
			const char c = k[0];
			if (c >= 'A' && c <= 'Z') return [NSString stringWithFormat:@"%c", c - 'A' + 'a'];
			if (c >= '0' && c <= '9') return [NSString stringWithFormat:@"%c", c];
			return @"";
		}
		if (k[0] == 'F' && k.size() <= 3)
		{
			const int n = std::atoi(k.c_str() + 1);
			if (n >= 1 && n <= 12)
				return [NSString stringWithFormat:@"%C",
				                 (unichar)(NSF1FunctionKey + (n - 1))];
		}
		struct Named { const char* key; unichar ch; };
		// NSDeleteCharacter is the backspace key on a Mac keyboard (⌫) and
		// NSDeleteFunctionKey the forward delete (⌦) — the names read backwards
		// and getting them the wrong way round puts the wrong glyph in the menu.
		static const Named kNamed[] = {
			{ "Return",    (unichar)'\r'                   },
			{ "Escape",    (unichar)27                     },
			{ "Backspace", (unichar)NSDeleteCharacter      },
			{ "Delete",    (unichar)NSDeleteFunctionKey    },
			{ "Tab",       (unichar)'\t'                   },
			{ "Space",     (unichar)' '                    },
			{ "Left",      (unichar)NSLeftArrowFunctionKey },
			{ "Right",     (unichar)NSRightArrowFunctionKey},
			{ "Up",        (unichar)NSUpArrowFunctionKey   },
			{ "Down",      (unichar)NSDownArrowFunctionKey },
			{ "Home",      (unichar)NSHomeFunctionKey      },
			{ "End",       (unichar)NSEndFunctionKey       },
			{ "Page Up",   (unichar)NSPageUpFunctionKey    },
			{ "Page Down", (unichar)NSPageDownFunctionKey  },
		};
		for (const Named& n : kNamed)
			if (k == n.key) return [NSString stringWithFormat:@"%C", n.ch];
		return @"";
	}

	// Ctrl in the engine's vocabulary IS Command here — one flag, so an
	// application writes "Ctrl+S" once and gets ⌘S on this platform and Ctrl+S
	// on the others. See UIWidget/UIShortcut.h.
	NSEventModifierFlags heModifierMask(const HE::UIShortcut& sc)
	{
		NSEventModifierFlags m = 0;
		if (sc.ctrl)  m |= NSEventModifierFlagCommand;
		if (sc.shift) m |= NSEventModifierFlagShift;
		if (sc.alt)   m |= NSEventModifierFlagOption;
		return m;
	}
}

// Every entry routes here; which one it was rides on the item as its id, so the
// mapping cannot drift out of step with a menu that was rebuilt in between.
@interface HEAppMenuTarget : NSObject
- (void)fire:(id)sender;
@end
@implementation HEAppMenuTarget
- (void)fire:(id)sender
{
	if (NSString* rid = (NSString*)[(NSMenuItem*)sender representedObject])
		s_clicks.push_back(std::string([rid UTF8String]));
}
@end

static HEAppMenuTarget* s_target = nil;

namespace HE::AppMacMenu
{

bool available()
{
	// SDL builds the main menu when it registers the application (Cocoa_RegisterApp
	// → CreateApplicationMenus). Before that there is nothing to insert into, and
	// building our own would be the wholesale replacement this deliberately avoids.
	return NSApp != nil && [NSApp mainMenu] != nil;
}

void set(const std::vector<HE::AppMenu>& menus)
{
	if (!available()) return;
	@autoreleasepool
	{
		NSMenu* main = [NSApp mainMenu];

		// Out with the previous set. Asked for by identity rather than by index:
		// nothing here owns the bar, and AppKit may have items of its own in it.
		for (NSMenuItem* it : s_ours)
			if ([main indexOfItem:it] >= 0) [main removeItem:it];
		s_ours.clear();
		if (menus.empty()) return;   // back to exactly the bar SDL built

		if (!s_target) s_target = [HEAppMenuTarget new];

		// After the application menu, before Window — where a Mac application's
		// own menus go. Index 0 is the app menu SDL made; if something ever left
		// the bar empty, the front is still the right place.
		NSInteger at = [main numberOfItems] > 0 ? 1 : 0;
		for (const HE::AppMenu& m : menus)
		{
			NSString* label = heStr(m.label);
			NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:label
			                                               action:nil
			                                        keyEquivalent:@""];
			NSMenu* sub = [[NSMenu alloc] initWithTitle:label];
			// The same rule the editor's bar follows: we say what is enabled, not
			// the responder chain. Nothing is disabled yet, but a bar that decides
			// that for itself would answer differently from the drawn one the day
			// `enabled` lands, and one of the two would be wrong.
			sub.autoenablesItems = NO;

			for (const HE::AppMenuItem& item : m.items)
			{
				if (item.separator)
				{
					[sub addItem:[NSMenuItem separatorItem]];
					continue;
				}
				// The entry's own chord, if it has one that AppKit can express.
				// A native key equivalent swallows the keystroke before anything
				// in the window sees it, which is exactly what is wanted HERE
				// and exactly why the widget layer must not match it as well.
				NSString* eq = @"";
				NSEventModifierFlags mask = 0;
				if (!item.shortcut.empty())
				{
					HE::UIShortcut sc;
					if (HE::uiParseShortcut(item.shortcut, sc))
					{
						eq   = heKeyEquivalent(sc);
						mask = heModifierMask(sc);
					}
				}
				NSMenuItem* row = [[NSMenuItem alloc] initWithTitle:heStr(item.label)
				                                            action:@selector(fire:)
				                                     keyEquivalent:eq];
				if (eq.length > 0) row.keyEquivalentModifierMask = mask;
				row.target = s_target;
				// The ID travels ON the item. A side table indexed by position
				// would have to be rebuilt in lockstep with the menu, and the tray
				// already showed what that costs.
				row.representedObject = heStr(item.id);
				row.enabled = YES;
				[sub addItem:row];
			}

			holder.submenu = sub;
			[main insertItem:holder atIndex:at++];
			s_ours.push_back(holder);
		}
	}
}

bool take(std::string& id)
{
	if (s_clicks.empty()) return false;
	id = std::move(s_clicks.front());
	s_clicks.pop_front();
	return true;
}

} // namespace HE::AppMacMenu
