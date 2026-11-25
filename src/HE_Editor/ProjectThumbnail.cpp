#include "ProjectThumbnail.h"

#include <Application/AppIcon.h>   // hePngWrite — the engine's one PNG encoder

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

namespace ProjectThumbnail
{

fs::path fileFor(const std::string& heprojPath)
{
	if (heprojPath.empty()) return {};
	return fs::path(heprojPath).parent_path() / "Saved" / "ProjectThumbnail.png";
}

bool fit(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h,
         std::vector<std::uint8_t>& out)
{
	if (w == 0 || h == 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return false;

	// The largest kWidth:kHeight window in the frame, centred: a viewport's
	// edges are the part of the view you can lose (same reasoning as the scene
	// tile's square crop in AssetThumbnailCache::captureToTile).
	std::uint32_t cw = w, ch = static_cast<std::uint32_t>(static_cast<uint64_t>(w) * kHeight / kWidth);
	if (ch > h) { ch = h; cw = static_cast<std::uint32_t>(static_cast<uint64_t>(h) * kWidth / kHeight); }
	cw = std::max(cw, 1u); ch = std::max(ch, 1u);
	const std::uint32_t x0 = (w - cw) / 2, y0 = (h - ch) / 2;

	out.assign(static_cast<size_t>(kWidth) * kHeight * 4, 255);
	// Box filter capped at 4×4 taps: a 5K viewport averaged in full would be
	// tens of millions of reads on the way out of the editor.
	constexpr std::uint32_t kMaxTaps = 4;
	for (int y = 0; y < kHeight; ++y)
	{
		const std::uint32_t sy0 = y0 + static_cast<std::uint32_t>(static_cast<uint64_t>(y) * ch / kHeight);
		const std::uint32_t sy1 = std::max(sy0 + 1, y0 + static_cast<std::uint32_t>(static_cast<uint64_t>(y + 1) * ch / kHeight));
		const std::uint32_t stepY = std::max(1u, (sy1 - sy0) / kMaxTaps);
		for (int x = 0; x < kWidth; ++x)
		{
			const std::uint32_t sx0 = x0 + static_cast<std::uint32_t>(static_cast<uint64_t>(x) * cw / kWidth);
			const std::uint32_t sx1 = std::max(sx0 + 1, x0 + static_cast<std::uint32_t>(static_cast<uint64_t>(x + 1) * cw / kWidth));
			const std::uint32_t stepX = std::max(1u, (sx1 - sx0) / kMaxTaps);
			std::uint32_t acc[3] = { 0, 0, 0 }, taps = 0;
			for (std::uint32_t sy = sy0; sy < sy1 && sy < h; sy += stepY)
				for (std::uint32_t sx = sx0; sx < sx1 && sx < w; sx += stepX)
				{
					const std::uint8_t* p = &rgba[(static_cast<size_t>(sy) * w + sx) * 4];
					acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
					++taps;
				}
			std::uint8_t* d = &out[(static_cast<size_t>(y) * kWidth + x) * 4];
			if (taps) { d[0] = std::uint8_t(acc[0] / taps); d[1] = std::uint8_t(acc[1] / taps); d[2] = std::uint8_t(acc[2] / taps); }
		}
	}
	return true;
}

bool isFlat(const std::vector<std::uint8_t>& rgba)
{
	for (size_t i = 4; i + 3 < rgba.size(); i += 4)
		if (std::memcmp(&rgba[i], &rgba[0], 3) != 0) return false;
	return true;
}

bool save(const std::string& heprojPath, const std::vector<std::uint8_t>& rgba,
          std::uint32_t w, std::uint32_t h)
{
	const fs::path file = fileFor(heprojPath);
	std::vector<std::uint8_t> px;
	if (file.empty() || !fit(rgba, w, h, px) || isFlat(px)) return false;
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	return HE::hePngWrite(file, px.data(), kWidth, kHeight);
}

} // namespace ProjectThumbnail
