#pragma once

// ─── HorizonNet — internal logging helpers ───────────────────────────────────
// Everything in HorizonNet logs to HE::Log::Cat::Net, so one HE_LOG=Net=Trace
// run tells the whole story of a session: sockets, framing, handshake, session
// protocol, directory calls and router mapping, in order, on one timeline.
//
// ⚠ WHAT MUST NEVER REACH THE LOG
// A network log is the one place where a debugging aid turns into a
// credential leak, because logs get pasted into bug reports and attached to
// crash dumps. HorizonNet handles four kinds of material that must not appear
// verbatim, no matter the verbosity:
//
//   1. The join secret / join code. Anyone holding it can enter the session.
//      Not even a digest is logged — the code is short enough to type, so a
//      digest narrows an offline search far more than it does for a full key.
//      Only its LENGTH is ever recorded, which is what you actually need to
//      debug "the two sides disagree about the code".
//   2. Key material: X25519 private scalars, the ECDH shared secret and the
//      derived session key. For correlating two peers there is already a
//      purpose-built one-way value — SecureTransport::sessionFingerprint() —
//      and that is the only thing logged.
//   3. The directory management token. It authorises heartbeat and
//      unregister for someone else's session.
//   4. Payload bytes. Frames carry scene data, asset blobs and chat-adjacent
//      presence — logging contents would quietly turn the log into a copy of
//      the user's project. Only sizes and message ids are recorded.
//
// Session IDs sit one step below that: they are meant to be shared, but they
// resolve to the host's public address in the directory, so they are logged
// abbreviated (see logSessionId) rather than in full.
//
// The helpers below exist so those rules are followed by construction instead
// of by remembering them at each of the ~200 call sites.

#include <Diagnostics/Log.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace HE::Net::detail {

// A session id, shortened for logging: enough to correlate lines within a run,
// not enough to be a working invitation if the log is shared. Short ids are
// passed through — abbreviating "abc" to "ab…" would be theatre, and an id that
// short is not one of ours anyway.
inline std::string logSessionId(const std::string& id)
{
	if (id.size() <= 6) return id.empty() ? std::string("<none>") : id;
	return id.substr(0, 6) + "…";
}

// Describes a secret WITHOUT revealing anything about its value. Used for the
// join code, where "the lengths differ" is the diagnosis 90% of the time and
// the value itself is never worth the risk.
inline std::string logSecretShape(const std::string& secret)
{
	if (secret.empty()) return "<empty>";
	return "<" + std::to_string(secret.size()) + " chars>";
}

// Human-readable byte counts. Frame and snapshot sizes span six orders of
// magnitude here (a 12-byte NAT-PMP datagram, a 200 MB scene snapshot), and
// raw byte counts at that range are genuinely hard to read at a glance.
inline std::string logBytes(std::size_t n)
{
    char buf[48];
    if (n < 1024)
        std::snprintf(buf, sizeof(buf), "%zu B", n);
    else if (n < 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(n) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return buf;
}

} // namespace HE::Net::detail
