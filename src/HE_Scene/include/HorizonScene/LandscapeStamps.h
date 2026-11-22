#pragma once
#include "HorizonScene/LandscapeEdit.h"

#include <Types/UUID.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class HorizonWorld;
class ContentManager;

namespace HE
{
class LandscapeRuntime;

// The scene's stamp entities (LandscapeStampComponent) as LandscapeStamps: each one's texture read once (its red channel), its place
// from the entity's world transform and the floating origin. `images` keeps the decoded pictures between calls, by texture id.
struct LandscapeStampImage { std::shared_ptr<const std::vector<float>> values; int w = 0, h = 0; };   // null values: unreadable, not tried again
using LandscapeStampImages = std::unordered_map<HE::UUID, LandscapeStampImage>;
std::vector<LandscapeStamp> landscapeStampsOfWorld(HorizonWorld& world, ContentManager& content, LandscapeStampImages& images);

// Keeps the runtime's ground in step with the stamp entities. Call once a frame after LandscapeRuntime::update.
//
// What an idle stamp costs: one pass over the stamp entities that folds their transform and settings into a number, and a compare.
// Only when that number changed are the stamps handed over (LandscapeEditSource::setStamps), and then only the tiles under a stamp
// that came, went or changed are read again (on the job pool); a stamp that is being dragged does that at most ten times a second,
// and the far levels (the coarse world, a millisecond a tile on the main thread) follow once it has rested for 0.3 s. Nothing is
// evaluated per frame, per tile or per pixel while nothing moves.
class LandscapeStampSync
{
public:
	// Returns how many tiles were sent to be read again (0 on an idle frame).
	size_t sync(HorizonWorld& world, LandscapeRuntime& runtime, ContentManager& content);
	void   reset() { m_key = 0; m_images.clear(); }

private:
	uint64_t m_key = 0;                    // of the stamps last handed over
	const void* m_overlay = nullptr;       // ... and the overlay they went to, with how many it took
	size_t      m_applied = 0;
	std::vector<LandscapeTileKey> m_pendingCoarse;   // tiles the far levels have not taken yet (they do once the stamps rest)
	LandscapeStampImages m_images;
	std::chrono::steady_clock::time_point m_last{};
};

} // namespace HE
