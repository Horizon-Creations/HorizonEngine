#include "material/MaterialShaderBake.h"
#include "material/MaterialShaderLibrary.h"

#include <ContentManager/Assets.h>
#include <cstring>
#include <functional>

namespace HE
{
namespace
{
using LB = MaterialShaderLibrary::Backend;

// RendererBackend → cross-compiler target of the PLAIN pair. D3D11/D3D12 share HLSL.
bool plainTarget(RendererBackend rb, LB& out)
{
	switch (rb)
	{
		case RendererBackend::OpenGL: out = LB::GLSL410; return true;
		case RendererBackend::Vulkan: out = LB::SpirV;   return true;
		case RendererBackend::D3D11:
		case RendererBackend::D3D12:  out = LB::HLSL;    return true;
		case RendererBackend::Metal:  out = LB::Metal;   return true;
	}
	return false;
}

// The variant's string fields hold backend-native text or, for Vulkan, the raw
// SPIR-V words as bytes (MaterialShaderLibrary::spirvFromBytes undoes this).
std::string payload(const MaterialShaderLibrary::Compiled& c, LB lb)
{
	if (lb != LB::SpirV) return c.source;
	std::string s(c.spirv.size() * sizeof(uint32_t), '\0');
	if (!c.spirv.empty()) std::memcpy(s.data(), c.spirv.data(), s.size());
	return s;
}
} // namespace

bool bakeMaterialShaderVariant(MaterialShaderLibrary& lib, RendererBackend backend,
                               const std::string& fragGlsl, const std::string& vertBody,
                               MaterialShaderVariant& out, std::string& error,
                               std::vector<std::string>& warnings,
                               const std::string& gbufGlsl)
{
	LB lb;
	if (fragGlsl.empty() || !plainTarget(backend, lb))
	{
		error = fragGlsl.empty() ? "empty fragment source" : "unknown backend";
		return false;
	}
	const uint64_t fragHash = std::hash<std::string>{}(fragGlsl);
	const uint64_t vertHash = std::hash<std::string>{}(vertBody);
	// WPO materials bake their graph-generated vertex; everything else the shared one.
	auto vertexFor = [&](LB target) -> const MaterialShaderLibrary::Compiled& {
		return vertBody.empty() ? lib.standardVertex(target)
		                        : lib.customVertex(vertHash, vertBody, target);
	};

	const auto& vert = vertexFor(lb);
	const auto& frag = lib.fragment(fragHash, fragGlsl, lb);
	if (!vert.ok || !frag.ok)
	{
		error = vert.log + " " + frag.log;
		return false;
	}

	MaterialShaderVariant var;
	var.backend  = static_cast<uint8_t>(backend);
	var.vertex   = payload(vert, lb);
	var.fragment = payload(frag, lb);

	// The same fragment on the screen-space UI quad vertex (A3b): a widget with
	// this material must not need glslang at load either. Baked per material even
	// though the UI vertex is material-INDEPENDENT: it is one to two kilobytes, and
	// the alternative (one blob per pak, or a checked-in generated copy) buys that
	// back at the price of a second place where the UI vertex lives and can drift
	// from kUIVertex. Pairs with the PLAIN fragment only — on GL the clustered one
	// is GLSL 4.30 and the UI vertex 4.10.
	const auto& uiVert = lib.uiVertex(lb);
	if (uiVert.ok)
		var.uiVertex = payload(uiVert, lb);
	else
		warnings.push_back("UI vertex precompile failed (materials on widgets will "
		                   "cross-compile at load): " + uiVert.log);

	// Clustered twin (Thema 117). GL clusters only from GLSL 4.30 — both stages,
	// one version per program — so it needs its own vertex; fragmentClustered
	// refuses GLSL410 outright. Everywhere else the plain vertex pairs as is.
	const LB clTarget = (lb == LB::GLSL410) ? LB::GLSL430 : lb;
	const auto& clFrag = lib.fragmentClustered(fragHash, fragGlsl, clTarget);
	const auto* clVert = (clTarget != lb) ? &vertexFor(clTarget) : nullptr;
	if (clFrag.ok && (!clVert || clVert->ok))
	{
		var.fragmentClustered = payload(clFrag, clTarget);
		if (clVert) var.vertexClustered = payload(*clVert, clTarget);
	}
	else
		warnings.push_back("clustered variant precompile failed (the shipped game draws "
		                   "this material with the 8-light window): " + clFrag.log
		                   + (clVert ? " " + clVert->log : std::string()));

	// The deferred twin and the instanced vertex twins. Only where the renderer reads a
	// baked one (OpenGL, Metal); baking them elsewhere would add bytes nothing consumes.
	// The G-buffer pipeline draws the plain vertex stage, so only the fragment is new.
	const bool bakesTwins = backend == RendererBackend::OpenGL || backend == RendererBackend::Metal;
	if (bakesTwins)
	{
		auto instancedFor = [&](LB target) -> const MaterialShaderLibrary::Compiled& {
			return vertBody.empty() ? lib.standardVertexInstanced(target)
			                        : lib.customVertexInstanced(vertHash, vertBody, target);
		};
		const auto& vi = instancedFor(lb);
		if (vi.ok)
			var.vertexInstanced = payload(vi, lb);
		else
			warnings.push_back("instanced vertex precompile failed (batches of this material "
			                   "draw per instance or cross-compile the twin at load): " + vi.log);
		// GL's clustered pair is GLSL 4.30 on both stages, so its instanced twin is too.
		if (!var.vertexClustered.empty())
		{
			const auto& vic = instancedFor(clTarget);
			if (vic.ok) var.vertexInstancedClustered = payload(vic, clTarget);
		}
	}
	if (!gbufGlsl.empty() && bakesTwins)
	{
		const auto& gb = lib.fragment(std::hash<std::string>{}(gbufGlsl), gbufGlsl, lb);
		if (gb.ok)
			var.fragmentGBuffer = payload(gb, lb);
		else
			warnings.push_back("G-buffer variant precompile failed (the deferred path will "
			                   "cross-compile this material at load): " + gb.log);
	}

	out = std::move(var);
	return true;
}
} // namespace HE
