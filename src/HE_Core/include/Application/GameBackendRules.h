#pragma once
// The one place that answers "which graphics backend does a shipped build run
// on?" — for both halves of the question, which used to live in two files that
// never saw each other:
//
//   * the export dialog, deciding what to WRITE into config.json's "GameBackend"
//   * GameApplication, deciding what to DO with the name it READS back
//
// They disagreed once, and the disagreement shipped: an application export
// forced "Software" into the dialog's remembered game backend, that value is
// persisted in the editor's settings under the very key the game reads, and so
// every LATER game export carried it. The game then booted the UI-only CPU
// rasterizer — a window filled edge to edge with the near-black it clears to,
// no scene in it, nothing in the log that looks like an error.
//
// Header-only on purpose: the editor, the game runtime and the tests all need
// these rules, and no shared library links all three.
#include <Types/Enums.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace HE::BackendRules
{

// The platform an export really lands on ("Host" resolved against this build).
inline std::string targetPlatformName(std::string_view platform)
{
    if (platform != "Host") return std::string(platform);
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

// The graphics backends a game built for `platform` can create. The TARGET
// decides, not the editor: offering DirectX for a Linux build would only hand
// the player a name their runtime falls back from at startup. Names are the
// getRHIName spelling — that is what the game parses.
//
// Software is in NO list. It is not a GPU backend a player picks; it is what an
// application with Advanced Shader Effects off ships with, and a game shipped
// with it shows nothing.
inline std::vector<const char*> choicesFor(std::string_view platform)
{
    const std::string target = targetPlatformName(platform);
    if (target == "Windows") return { "OpenGL", "Vulkan", "D3D11", "D3D12" };
    if (target == "macOS")   return { "Metal", "OpenGL" };
    return { "OpenGL", "Vulkan" };
}

// Whether the dialog would offer this name for SOME target — the test a
// remembered value has to pass before it is believed. A settings file written
// by an editor that still forced "Software" into that key carries the word
// forever otherwise, and the next game export ships it.
inline bool isOffered(const std::string& name)
{
    for (const char* platform : { "Windows", "macOS", "Linux" })
        for (const char* b : choicesFor(platform))
            if (name == b) return true;
    return false;
}

// Which backend THIS export ships, as opposed to which one the dialog
// remembers. A game ships the combo's value. An APPLICATION does not choose one
// at all: it takes the platform default, or — with Advanced Shader Effects
// switched off — the software renderer, which is what that switch has meant all
// along (docs/he-apps-plan.md Block G).
//
// `remembered` is handed in and handed back, never assigned: that value is the
// GAME's choice and outlives the dialog. Answering the app's question by
// overwriting it is precisely the bug this function exists to make impossible.
inline std::string forExport(bool appProject, bool advancedShaderEffects,
                             const std::string& remembered)
{
    if (!appProject) return remembered;
    return advancedShaderEffects ? std::string() : std::string("Software");
}

// The backend a game built for `platform` starts on when config.json names none
// (RendererFactory::Default of the shipped runtime: Metal where it is linked, else
// OpenGL on the desktop runtimes).
inline const char* defaultBackendNameFor(std::string_view platform)
{
    return targetPlatformName(platform) == "macOS" ? "Metal" : "OpenGL";
}

// Which precompiled material shaders (CHUNK_PSHD) an export has to carry, as the
// bitmask the packer takes (1u << RendererBackend). A shipped game runs ONE backend,
// the one `shipped` names (forExport's answer) or, with no name, the platform
// default; the variants for any other backend are dead weight in the pak. A game
// that falls back to another backend (a config naming one its runtime lacks) just
// cross-compiles at load, which is what an unprecompiled pak does anyway.
// 0 for the software renderer: it draws no 3D, so there is nothing to compile.
inline uint32_t shaderBackendMaskFor(std::string_view platform, const std::string& shipped);

// Backends by NAME (the editor's getRHIName spelling), because config.json is a
// file a player or a support ticket edits by hand: "Metal" survives a
// renumbering of the enum, a bare 4 does not.
inline bool fromName(const std::string& name, RendererBackend& out)
{
    if (name == "OpenGL")   { out = RendererBackend::OpenGL;   return true; }
    if (name == "Vulkan")   { out = RendererBackend::Vulkan;   return true; }
    if (name == "D3D11")    { out = RendererBackend::D3D11;    return true; }
    if (name == "D3D12")    { out = RendererBackend::D3D12;    return true; }
    if (name == "Metal")    { out = RendererBackend::Metal;    return true; }
    if (name == "Software") { out = RendererBackend::Software; return true; }
    return false;
}

inline uint32_t shaderBackendMaskFor(std::string_view platform, const std::string& shipped)
{
    RendererBackend b{};
    if (!fromName(shipped.empty() ? std::string(defaultBackendNameFor(platform)) : shipped, b))
        b = RendererBackend::OpenGL;   // an unknown name: the game keeps its default, near enough
    if (b == RendererBackend::Software) return 0;
    return 1u << static_cast<uint32_t>(b);
}

// What the runtime does with the name it found in config.json.
enum class Shipped
{
    Use,           // take it
    UnknownName,   // not a backend name at all — keep the default
    NotBuilt,      // not compiled into this runtime flavour — keep the default
    UiOnlyForGame, // the software renderer, in a build that is not an app
};

// `available` is a predicate rather than a compile-time ladder: a runtime
// flavour (docs/he-apps-plan.md A3b) links a subset of the backends, and asking
// RendererFactory is the only way to know which. Passing it in is also what
// makes this decision answerable in a test without a window or a GPU.
template <class AvailableFn>
inline Shipped verdictForShipped(const std::string& name, bool appMode,
                                 RendererBackend& out, AvailableFn available)
{
    RendererBackend wanted{};
    if (!fromName(name, wanted))    return Shipped::UnknownName;
    if (!available(wanted))         return Shipped::NotBuilt;
    // The software renderer draws NOTHING BUT UI. For an application that is
    // the point; for a game it means an empty window and no way to tell from
    // the outside that anything went wrong.
    if (wanted == RendererBackend::Software && !appMode)
        return Shipped::UiOnlyForGame;
    out = wanted;
    return Shipped::Use;
}

} // namespace HE::BackendRules
