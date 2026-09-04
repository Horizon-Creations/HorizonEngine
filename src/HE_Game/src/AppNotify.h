#pragma once
#include <string>

// ── System notifications, the macOS half (plan C: notify) ────────────────────
// UserNotifications.framework, which is the only supported way since
// NSUserNotification was removed. Two things follow from that and both are
// visible in the API below:
//
//  1. **It needs a bundle identifier.** The framework asks the running
//     application who it is, and a bare executable has no answer — requesting
//     authorisation without one raises, it does not fail politely. So
//     `available()` asks first, and everything else is a no-op without it. A
//     packaged .app has one (the exporter writes it); HorizonGame started from
//     a build directory does not, and there notifications are simply off.
//
//  2. **Permission is the system's to grant, not ours to check.** The first
//     request pops the OS dialog, the answer arrives later, and a notification
//     posted before it lands is queued by the framework rather than lost. So
//     `show` returns "handed over", never "somebody saw it" — the honest answer,
//     and the same one the row above it gives a graph.
//
// ObjC++ (AppNotify.mm), compiled on APPLE only, in HE_Game and not HE_Core:
// the same placement rule AppMacMenu documents, for the same reason.
namespace HE::AppNotify {

// Is there an application identity to post as? False for a bare executable.
bool available();

// Post one. False when there is nothing to post as, or the framework refused it.
bool show(const std::string& title, const std::string& body);

} // namespace HE::AppNotify
