#pragma once

#include <filesystem>

// ─── Containment checks for paths that came from somewhere else ──────────────
// Anything built from a path a PEER supplied — a collaboration message, an
// imported manifest, a downloaded catalogue — has to be proven to land inside
// the directory it claims to be in. Concatenating a root with a relative path
// does not prove that: "a/../../etc/passwd" concatenates perfectly well.

namespace HE {

// Is `candidate` inside `root` (or root itself)?
//
// Lexical, not filesystem-canonical: the caller is usually about to CREATE the
// file, so it does not exist yet, and this must work the same on a machine
// where it does and one where it does not.
//
// Compared COMPONENT BY COMPONENT rather than as a string prefix, which is the
// part that is easy to get wrong: "/proj/Content" is a string prefix of
// "/proj/Content-backup", and a check that missed that would wave through every
// sibling directory whose name merely starts the same way.
//
// Known limit, accepted deliberately: symlinks are not resolved, so a link
// already inside the root pointing out of it would pass. Creating that link
// requires local file access, which is a strictly higher privilege than sending
// a message — this check is aimed at the traversal, not at an attacker who is
// already on the machine.
inline bool isPathWithin(const std::filesystem::path& root,
                         const std::filesystem::path& candidate)
{
    if (root.empty() || candidate.empty()) return false;

    const std::filesystem::path r = root.lexically_normal();
    const std::filesystem::path c = candidate.lexically_normal();

    auto ri = r.begin();
    auto ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci)
    {
        // A trailing "." is what lexically_normal leaves on a path written with
        // a trailing separator; it names the same directory, so it ends the
        // comparison rather than failing it.
        if (*ri == ".") { ++ri; break; }
        if (ci == c.end()) return false;      // candidate is shorter → above root
        if (*ci != *ri)    return false;      // diverged → a sibling, not a child
    }
    return true;
}

// The same question for a relative path that has not been joined yet: does it
// stay inside once appended? Cheaper than building the full path, and it gives
// the caller something to reject before touching the filesystem at all.
inline bool isRelativePathContained(const std::filesystem::path& relative)
{
    if (relative.empty()) return false;
    if (relative.is_absolute()) return false;   // decides its own root — not ours
#ifdef _WIN32
    // "C:foo" is relative but carries a drive, and "\\server\share" is neither
    // relative nor ours. has_root_name covers both.
    if (relative.has_root_name()) return false;
#endif
    int depth = 0;
    for (const auto& part : relative.lexically_normal())
    {
        if (part == "..") { if (--depth < 0) return false; }
        else if (part != ".") ++depth;
    }
    return true;
}

} // namespace HE
