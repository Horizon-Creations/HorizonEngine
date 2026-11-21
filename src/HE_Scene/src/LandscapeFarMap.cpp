#include "HorizonScene/LandscapeFarMap.h"
#include "HorizonScene/LandscapeTile.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace HE
{

namespace
{
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void put64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | static_cast<uint64_t>(get32(p + 4)) << 32; }

const std::array<float, 256>& srgbToLinear()
{
	static const std::array<float, 256> lut = [] {
		std::array<float, 256> t{};
		for (int i = 0; i < 256; ++i)
		{
			const float c = static_cast<float>(i) / 255.0f;
			t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}
		return t;
	}();
	return lut;
}
} // namespace

uint8_t landscapeFarEncodeSrgb(float linear)
{
	const float c = std::clamp(linear, 0.0f, 1.0f);
	const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
	return static_cast<uint8_t>(std::lround(s * 255.0f));
}

float landscapeFarDecodeSrgb(uint8_t srgb) { return srgbToLinear()[srgb]; }

bool LandscapeFarTable::empty() const
{
	if (!layout.valid()) return true;
	for (const LandscapeFarRegion& r : regions)
		if (!r.rgba.empty()) return false;
	return true;
}

std::vector<uint8_t> LandscapeFarTable::encode() const
{
	std::vector<uint8_t> b;
	put32(b, kLandscapeFarMagic);
	put32(b, kLandscapeFarVersion);
	put32(b, static_cast<uint32_t>(layout.tilesX));
	put32(b, static_cast<uint32_t>(layout.tilesZ));
	put32(b, static_cast<uint32_t>(layout.regionTiles));
	put32(b, static_cast<uint32_t>(layout.texelsPerTile));
	const int side = layout.regionSide();
	for (const LandscapeFarRegion& r : regions)
	{
		const bool baked = r.rgba.size() == static_cast<size_t>(side) * static_cast<size_t>(side) * 4;
		const std::vector<uint8_t> blob = baked ? encodeLandscapeWeightGrid(r.rgba, side, side, 4) : std::vector<uint8_t>{};
		put64(b, baked ? r.hash : 0);
		put32(b, static_cast<uint32_t>(blob.size()));
		b.insert(b.end(), blob.begin(), blob.end());
	}
	return b;
}

bool LandscapeFarTable::decode(const std::vector<uint8_t>& bytes, LandscapeFarTable& out)
{
	out = LandscapeFarTable{};
	const auto fail = [&] { out = LandscapeFarTable{}; return false; };
	if (bytes.size() < 24 || get32(bytes.data()) != kLandscapeFarMagic || get32(bytes.data() + 4) != kLandscapeFarVersion) return false;
	// Sizes are bounded before anything is allocated from them: a damaged header must not ask for gigabytes.
	const uint32_t tx = get32(bytes.data() + 8), tz = get32(bytes.data() + 12), rt = get32(bytes.data() + 16), tpt = get32(bytes.data() + 20);
	if (tx == 0 || tz == 0 || tx > 65536 || tz > 65536 || rt == 0 || rt > 1024 || tpt == 0 || tpt > 4096) return false;
	out.layout = { static_cast<int>(tx), static_cast<int>(tz), static_cast<int>(rt), static_cast<int>(tpt) };
	if (!out.layout.valid() || out.layout.regionSide() > 16384) return fail();
	const size_t count = static_cast<size_t>(out.layout.regionsX()) * static_cast<size_t>(out.layout.regionsZ());
	if (count > (bytes.size() - 24) / 12) return fail();   // every region costs at least its 12-byte head
	out.regions.resize(count);
	const int side = out.layout.regionSide();
	size_t at = 24;
	for (LandscapeFarRegion& r : out.regions)
	{
		if (bytes.size() - at < 12) return fail();
		r.hash = get64(bytes.data() + at);
		const uint32_t size = get32(bytes.data() + at + 8);
		at += 12;
		if (size > bytes.size() - at) return fail();
		if (size > 0)
		{
			int w = 0, h = 0, ch = 0;
			if (!decodeLandscapeWeightGrid(bytes.data() + at, size, r.rgba, w, h, ch) || w != side || h != side || ch != 4) return fail();
		}
		at += size;
	}
	return at == bytes.size() ? true : fail();
}

void LandscapeFarTable::assemble(int& width, int& height, std::vector<std::vector<uint8_t>>& mips,
                                 int originTileX, int originTileZ, int tilesX, int tilesZ) const
{
	width = height = 0;
	mips.clear();
	if (empty()) return;
	// The window in texels of the whole world's image, clipped to it.
	originTileX = std::clamp(originTileX, 0, layout.tilesX - 1);
	originTileZ = std::clamp(originTileZ, 0, layout.tilesZ - 1);
	tilesX = tilesX < 0 ? layout.tilesX - originTileX : std::clamp(tilesX, 1, layout.tilesX - originTileX);
	tilesZ = tilesZ < 0 ? layout.tilesZ - originTileZ : std::clamp(tilesZ, 1, layout.tilesZ - originTileZ);
	const int wx0 = originTileX * layout.texelsPerTile, wz0 = originTileZ * layout.texelsPerTile;
	width = tilesX * layout.texelsPerTile;
	height = tilesZ * layout.texelsPerTile;
	const int side = layout.regionSide();
	std::vector<uint8_t> top(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
	for (int rz = wz0 / side; rz < layout.regionsZ() && rz * side < wz0 + height; ++rz)
		for (int rx = wx0 / side; rx < layout.regionsX() && rx * side < wx0 + width; ++rx)
		{
			const LandscapeFarRegion& r = regions[static_cast<size_t>(rz) * static_cast<size_t>(layout.regionsX()) + static_cast<size_t>(rx)];
			if (r.rgba.empty()) continue;
			// The part of the region inside the window (the last region may also hang over the world's edge).
			const int x0 = std::max(rx * side, wx0), x1 = std::min((rx + 1) * side, wx0 + width);
			const int z0 = std::max(rz * side, wz0), z1 = std::min((rz + 1) * side, wz0 + height);
			for (int z = z0; z < z1; ++z)
				std::copy_n(r.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(z - rz * side) * static_cast<size_t>(side) + static_cast<size_t>(x0 - rx * side)) * 4),
				            static_cast<size_t>(x1 - x0) * 4,
				            top.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(z - wz0) * static_cast<size_t>(width) + static_cast<size_t>(x0 - wx0)) * 4));
		}
	mips.push_back(std::move(top));
	const std::array<float, 256>& lin = srgbToLinear();
	int w = width, h = height;
	while (w > 1 || h > 1)
	{
		const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
		const std::vector<uint8_t>& src = mips.back();
		std::vector<uint8_t> dst(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
		for (int z = 0; z < nh; ++z)
			for (int x = 0; x < nw; ++x)
			{
				// 2 x 2 mean; an odd edge repeats its last texel.
				float rgb[3] = { 0, 0, 0 };
				int   a = 0;
				for (int dz = 0; dz < 2; ++dz)
					for (int dx = 0; dx < 2; ++dx)
					{
						const size_t s = (static_cast<size_t>(std::min(z * 2 + dz, h - 1)) * static_cast<size_t>(w) + static_cast<size_t>(std::min(x * 2 + dx, w - 1))) * 4;
						for (int c = 0; c < 3; ++c) rgb[c] += lin[src[s + static_cast<size_t>(c)]];
						a += src[s + 3];
					}
				const size_t d = (static_cast<size_t>(z) * static_cast<size_t>(nw) + static_cast<size_t>(x)) * 4;
				for (int c = 0; c < 3; ++c) dst[d + static_cast<size_t>(c)] = landscapeFarEncodeSrgb(rgb[c] * 0.25f);
				dst[d + 3] = static_cast<uint8_t>((a + 2) / 4);
			}
		mips.push_back(std::move(dst));
		w = nw;
		h = nh;
	}
}

} // namespace HE
