#pragma once

// ─── HorizonSourceControl — module export macro ──────────────────────────────
// Same discipline as HorizonNet's HE_NET_API and HorizonCore's HE_API: explicit
// dllexport while building, dllimport for consumers. Not
// WINDOWS_EXPORT_ALL_SYMBOLS — that is HorizonScene's arrangement, and mixing
// the two is a documented build break (writing HE_API inside a module that
// exports everything produces MSVC C4273).
//
// The module is editor-only. It is deliberately NOT copied into the packaged
// game: a player's binary has no business carrying a git client and three
// provider REST clients.
#ifdef _WIN32
  #ifdef HE_SC_BUILD_DLL
    #define HE_SC_API __declspec(dllexport)
  #else
    #define HE_SC_API __declspec(dllimport)
  #endif
#else
  #define HE_SC_API __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <string>

namespace HE::Sc {

// Which hosting service a remote belongs to. Only three things actually differ
// between them — how a token is presented, where the user creates one, and what
// size limits they enforce — so this enum is deliberately small.
enum class ProviderKind : std::uint8_t {
	Generic = 0,   // any plain git remote, and the fallback for unknown hosts
	GitHub,
	GitLab,
	AzureDevOps,
};

HE_SC_API const char* providerName(ProviderKind kind);

} // namespace HE::Sc
