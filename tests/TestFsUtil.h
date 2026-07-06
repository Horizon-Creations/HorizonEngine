#pragma once
// Best-effort filesystem cleanup for tests.
//
// Windows cannot delete files another handle still has open (HpakReader keeps a
// persistent stream; mounted paks hold theirs; the profiler keeps its dump file).
// POSIX allows it, so throwing cleanups only ever detonate on the Windows CI
// runner. These never throw: leftovers are uniquely named temp files reclaimed
// with the runner's temp directory.
#include <filesystem>
#include <system_error>

namespace he_test
{
inline void removeQuiet(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove(p, ec);
}
inline void removeAllQuiet(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}
} // namespace he_test
