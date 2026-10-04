// Export-time baking of one material's precompiled shader variant (CHUNK_PSHD).
//
// The single recipe for "what goes into a MaterialShaderVariant", shared by the
// exporter (ExportDialogPanel's CompileMaterialShaderVariants) and the editor's
// HE_DUMP_MATPRECOMPILE witness — the witness only proves something if it bakes
// exactly what a shipped pak carries.
#pragma once

#include <Types/Enums.h> // HE::RendererBackend
#include <string>
#include <vector>

struct MaterialShaderVariant;

namespace HE
{
class MaterialShaderLibrary;

// Cross-compile `fragGlsl` (+ the shared standard vertex, or the graph's WPO
// `vertBody`) for `backend` into `out`:
//   vertex / fragment   — the plain pair (8-light window). Required: false when
//                         either fails, `error` says why, `out` is untouched.
//   uiVertex            — the screen-space UI quad vertex for the same fragment.
//   fragmentClustered   — the clustered-lighting twin (Thema 117); on GL with a
//   (+ vertexClustered)   GLSL 4.30 vertex, because that program is 4.30 on both
//                         stages. Elsewhere it pairs with `vertex`.
// The last two are optional: a failure leaves the field empty (the runtime then
// cross-compiles the UI vertex / draws the plain fragment) and appends a line to
// `warnings`. Vulkan fields carry the SPIR-V words as raw bytes.
bool bakeMaterialShaderVariant(MaterialShaderLibrary& lib, RendererBackend backend,
                               const std::string& fragGlsl, const std::string& vertBody,
                               MaterialShaderVariant& out, std::string& error,
                               std::vector<std::string>& warnings);
} // namespace HE
