#pragma once
#include <cstdint>

// ─── Node-graph texture misses and the content epoch (Thema 210) ─────────────
// Every backend's ResolveGraphTexture caches a texture it could not load as a
// null entry under the slot's key ("hi:lo", or the PATH for a loose material's
// graphTexturePaths, plus "#arr" for a sampler2DArray slot), so an unloadable
// reference does not open its file again every frame. That is right for "never
// loadable" and wrong for "not there YET": an EngineContent texture referenced
// by path starts its download on the first ask (ContentManager::loadAsset ->
// requestRemotePath) and reports UUID{} until it lands. The null stayed for the
// rest of the run, InvalidateTexture (UUID key) never reaches a path key, and a
// sampler2DArray slot bound the white array instead. Under the auto landscape
// material that is the hexagon pattern: the texture bombing rotates the white
// array's constant normal per hex (docs/landscape-hex-reload-ursache-2026-10-18.md).
//
// The fix keeps the negative cache and ties it to ContentManager::contentEpoch(),
// which moves whenever files may have appeared (a save, a content scan, a
// download landing). Call this at the top of ResolveGraphTexture: once per epoch
// it drops the misses, and the next ask loads again. Loaded textures are never
// touched, so a first load renders exactly as before.
//
// `isMiss(value)` says whether a cache value is a miss, and may free what a
// half-built miss holds (Vulkan); it is only called when the epoch moved.
namespace HE
{
template<class Map, class IsMiss>
void forgetGraphTextureMisses(Map& cache, uint64_t& seenEpoch, uint64_t epoch, IsMiss&& isMiss)
{
	if (epoch == seenEpoch) return;
	seenEpoch = epoch;
	for (auto it = cache.begin(); it != cache.end();)
		it = isMiss(it->second) ? cache.erase(it) : ++it;
}
} // namespace HE
