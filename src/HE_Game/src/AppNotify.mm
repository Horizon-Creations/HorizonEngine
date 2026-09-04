// System notifications on macOS (see AppNotify.h). ObjC++ — APPLE only.
#include "AppNotify.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <Diagnostics/Logger.h>

namespace
{
	// Asked once. Requesting authorisation is asynchronous and the answer is the
	// user's, so this is not "may we" — it is "have we asked yet". Posting before
	// the answer arrives is allowed; the framework holds the notification.
	bool s_asked = false;

	NSString* heStr(const std::string& s)
	{
		NSString* n = [NSString stringWithUTF8String:s.c_str()];
		return n ? n : @"";
	}

	// The centre, or nil when this process has no bundle identity. Everything
	// here goes through it, because `currentNotificationCenter` on an unbundled
	// process RAISES rather than returning nil.
	UNUserNotificationCenter* centre()
	{
		if (![NSBundle mainBundle].bundleIdentifier) return nil;
		@try { return [UNUserNotificationCenter currentNotificationCenter]; }
		@catch (NSException* e)
		{
			HE_LOG_WARN(Core, "notify: the notification centre refused this process (%s)",
			            e.reason ? e.reason.UTF8String : "no reason given");
			return nil;
		}
	}
}

namespace HE::AppNotify
{

bool available() { return centre() != nil; }

bool show(const std::string& title, const std::string& body)
{
	UNUserNotificationCenter* c = centre();
	if (!c)
	{
		// The one case worth naming: run from a build directory there is no
		// bundle, and "nothing happens" would otherwise look like a bug in the
		// graph rather than a property of how the program was started.
		HE_LOG_WARN(Core, "%s",
			"notify: this process has no bundle identifier (started as a bare "
			"executable?) — macOS posts notifications only for bundled apps");
		return false;
	}

	if (!s_asked)
	{
		s_asked = true;
		// Fire and forget: the answer is the user's and arrives whenever the
		// dialog is dealt with. Nothing here waits for it — a frame loop that
		// blocks on a permission dialog is a frozen window.
		[c requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
		                completionHandler:^(BOOL granted, NSError* error)
		{
			if (error)
				HE_LOG_WARN(Core, "notify: authorisation failed (%s)",
				            error.localizedDescription.UTF8String);
			else if (!granted)
				HE_LOG_INFO(Core, "%s", "notify: the user has not allowed notifications for this app");
		}];
	}

	UNMutableNotificationContent* content = [UNMutableNotificationContent new];
	// A banner with an empty title is drawn differently on every system version,
	// so the text moves up into it when there is no title — one line is still a
	// notification, an empty headline is not.
	if (title.empty()) content.title = heStr(body);
	else { content.title = heStr(title); content.body = heStr(body); }

	// nil trigger = deliver now. The identifier is unique per post: reusing one
	// REPLACES the earlier notification, which is a feature for a progress
	// message and a bug for two things worth saying.
	UNNotificationRequest* req =
		[UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
		                                     content:content
		                                     trigger:nil];
	[c addNotificationRequest:req withCompletionHandler:^(NSError* error)
	{
		if (error)
			HE_LOG_WARN(Core, "notify: %s", error.localizedDescription.UTF8String);
	}];
	// "Handed over", not "shown": whether it appears is the system's decision
	// (Do Not Disturb, the per-app switch, a full-screen game), and it makes that
	// decision later.
	return true;
}

} // namespace HE::AppNotify
