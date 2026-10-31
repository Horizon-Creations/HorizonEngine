#include "HorizonScene/LandscapeTile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef HE_HAVE_LZ4
#include <lz4.h>
#endif
#ifdef HE_HAVE_ZSTD
#include <zstd.h>
#endif

namespace HE
{
namespace
{
bool isPow2(uint32_t v) { return v && !(v & (v - 1)); }

// The coarsest level keeps at least this many cells per side (tileM / 128 m: 4 for a 512 m tile).
int floorCells(const LandscapeGeometry& g)
{
	return std::max(1, static_cast<int>(std::lround(g.tileM / LandscapeGeometry::kCoarsestRasterM)));
}

int levelCount(int cells, int floorC)
{
	int n = 0;
	for (int k = 0; (cells >> k) >= floorC && (cells >> k) >= 1; ++k) ++n;
	return n;
}

void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// ── transforms ───────────────────────────────────────────────────────────────
// Plane predictor in wrapping 16 bit; zigzag so small residuals of either sign have small high bytes;
// lo-byte plane then hi-byte plane. First row predicts from the left, first column from above.
void heightForward(const std::vector<uint16_t>& v, int w, int h, std::vector<uint8_t>& out)
{
	const size_t cnt = v.size();
	out.resize(cnt * 2);
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x)
		{
			const size_t i  = static_cast<size_t>(z) * w + x;
			const int    l  = x ? v[i - 1] : 0, u = z ? v[i - w] : 0, ul = (x && z) ? v[i - w - 1] : 0;
			const int    pr = (x && z) ? (l + u - ul) : (x ? l : u);
			const uint16_t d  = static_cast<uint16_t>(v[i] - pr);
			const uint16_t zz = static_cast<uint16_t>((d << 1) ^ (static_cast<int16_t>(d) >> 15));
			out[i]       = static_cast<uint8_t>(zz & 0xFF);
			out[cnt + i] = static_cast<uint8_t>(zz >> 8);
		}
}

void heightInverse(const uint8_t* in, int w, int h, std::vector<uint16_t>& v)
{
	const size_t cnt = static_cast<size_t>(w) * h;
	v.resize(cnt);
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x)
		{
			const size_t i  = static_cast<size_t>(z) * w + x;
			const uint16_t zz = static_cast<uint16_t>(in[i] | (in[cnt + i] << 8));
			const uint16_t d  = static_cast<uint16_t>((zz >> 1) ^ -(zz & 1));
			const int      l  = x ? v[i - 1] : 0, u = z ? v[i - w] : 0, ul = (x && z) ? v[i - w - 1] : 0;
			const int      pr = (x && z) ? (l + u - ul) : (x ? l : u);
			v[i] = static_cast<uint16_t>(d + pr);
		}
}

// Channel planes, horizontal delta (mod 256) inside each plane.
void weightForward(const std::vector<uint8_t>& v, int w, int h, int C, std::vector<uint8_t>& out)
{
	const size_t plane = static_cast<size_t>(w) * h;
	out.resize(v.size());
	for (int ch = 0; ch < C; ++ch)
		for (int z = 0; z < h; ++z)
			for (int x = 0; x < w; ++x)
			{
				const size_t  i   = static_cast<size_t>(z) * w + x;
				const uint8_t cur = v[i * C + ch], prev = x ? v[(i - 1) * C + ch] : 0;
				out[ch * plane + i] = static_cast<uint8_t>(cur - prev);
			}
}

void weightInverse(const uint8_t* in, int w, int h, int C, std::vector<uint8_t>& v)
{
	const size_t plane = static_cast<size_t>(w) * h;
	v.resize(plane * C);
	for (int ch = 0; ch < C; ++ch)
		for (int z = 0; z < h; ++z)
		{
			uint8_t acc = 0;
			for (int x = 0; x < w; ++x)
			{
				const size_t i = static_cast<size_t>(z) * w + x;
				acc = static_cast<uint8_t>(acc + in[ch * plane + i]);
				v[i * C + ch] = acc;
			}
		}
}

// ── backends ─────────────────────────────────────────────────────────────────
std::vector<uint8_t> compress(LandscapeCodec::Backend b, int zstdLevel, const uint8_t* src, size_t n)
{
	std::vector<uint8_t> out;
	switch (b)
	{
#ifdef HE_HAVE_LZ4
	case LandscapeCodec::Backend::Lz4:
		out.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(n))));
		out.resize(static_cast<size_t>(LZ4_compress_default(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(out.data()),
		                                                    static_cast<int>(n), static_cast<int>(out.size()))));
		return out;
#endif
#ifdef HE_HAVE_ZSTD
	case LandscapeCodec::Backend::Zstd:
		out.resize(ZSTD_compressBound(n));
		out.resize(ZSTD_compress(out.data(), out.size(), src, n, zstdLevel));
		return out;
#endif
	default:
		(void)zstdLevel;
		out.assign(src, src + n);
		return out;
	}
}

// True when exactly `rawSize` bytes came out.
bool decompress(LandscapeCodec::Backend b, const uint8_t* src, size_t stored, uint8_t* dst, size_t rawSize)
{
	switch (b)
	{
	case LandscapeCodec::Backend::Store:
		if (stored != rawSize) return false;
		std::memcpy(dst, src, rawSize);
		return true;
#ifdef HE_HAVE_LZ4
	case LandscapeCodec::Backend::Lz4:
		return LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst), static_cast<int>(stored),
		                           static_cast<int>(rawSize)) == static_cast<int>(rawSize);
#endif
#ifdef HE_HAVE_ZSTD
	case LandscapeCodec::Backend::Zstd:
	{
		const size_t r = ZSTD_decompress(dst, rawSize, src, stored);
		return !ZSTD_isError(r) && r == rawSize;
	}
#endif
	default: return false;
	}
}

bool fail(std::string* error, const char* why)
{
	if (error) *error = why;
	return false;
}

struct Header
{
	LandscapeGeometry geometry;
	LandscapeCodec    codec;
	int               hLevels = 0, wLevels = 0;
	bool              water = false;   // flag bit 1: two streams after the weights
};

bool readHeader(const uint8_t* d, size_t size, Header& h, std::string* error)
{
	if (size < kLandscapeTileHeaderBytes) return fail(error, "tile blob shorter than its header");
	if (get32(d) != kLandscapeTileMagic) return fail(error, "not a landscape tile (bad magic)");
	if (d[4] != kLandscapeTileVersion) return fail(error, "unknown landscape tile version");
	if (d[5] > static_cast<uint8_t>(LandscapeCodec::Backend::Zstd)) return fail(error, "unknown tile backend");
	h.codec.backend   = static_cast<LandscapeCodec::Backend>(d[5]);
	h.codec.predictor = (d[6] & 1) != 0;
	h.water           = (d[6] & 2) != 0;
	h.hLevels         = d[7];
	h.wLevels         = d[8];
	h.geometry.channels = d[9];
	h.geometry.hCells   = get16(d + 10);
	h.geometry.wTexels  = get16(d + 12);
	uint32_t bits = get32(d + 14);
	std::memcpy(&h.geometry.tileM, &bits, 4);
	if (!h.geometry.valid()) return fail(error, "tile geometry is not valid");
	if (h.hLevels != h.geometry.heightLevels() || h.wLevels != h.geometry.weightLevels())
		return fail(error, "tile level counts do not match its geometry");
	return true;
}
} // namespace

// ── LandscapeGeometry ────────────────────────────────────────────────────────
bool LandscapeGeometry::valid() const
{
	if (!(tileM > 0.0f) || !std::isfinite(tileM)) return false;
	if (!isPow2(hCells) || !isPow2(wTexels) || hCells > 4096 || wTexels > 2048) return false;
	if (channels < 1 || channels > 16) return false;
	const int f = floorCells(*this);
	return hCells >= f && wTexels >= f;
}

int LandscapeGeometry::heightLevels() const { return levelCount(hCells, floorCells(*this)); }
int LandscapeGeometry::weightLevels() const { return levelCount(wTexels, floorCells(*this)); }

size_t LandscapeGeometry::heightBytes(int levels) const
{
	const int n = (levels < 0) ? heightLevels() : std::min(levels, heightLevels());
	size_t s = 0;
	for (int k = 0; k < n; ++k) s += static_cast<size_t>(heightDim(k)) * heightDim(k) * 2;
	return s;
}

size_t LandscapeGeometry::weightBytes(int levels) const
{
	const int n = (levels < 0) ? weightLevels() : std::min(levels, weightLevels());
	size_t s = 0;
	for (int k = 0; k < n; ++k) s += static_cast<size_t>(weightDim(k)) * weightDim(k) * channels;
	return s;
}

// ── LandscapeTile ────────────────────────────────────────────────────────────
size_t LandscapeTile::bytes() const
{
	size_t s = 0;
	for (const auto& h : heights) s += h.size() * 2;
	for (const auto& w : weights) s += w.size();
	s += waterCoverage.size() + waterOwner.size() * 2;
	return s;
}

void LandscapeTile::buildMips()
{
	const LandscapeGeometry& g = geometry;
	heights.resize(1 + 0);
	weights.resize(1 + 0);
	const int n0 = g.heightDim(0);
	for (int k = 1; k < g.heightLevels(); ++k)
	{
		const int dn = g.heightDim(k);
		std::vector<uint16_t> lv(static_cast<size_t>(dn) * dn);
		for (int z = 0; z < dn; ++z)
			for (int x = 0; x < dn; ++x) lv[static_cast<size_t>(z) * dn + x] = heights[0][static_cast<size_t>(z << k) * n0 + (x << k)];
		heights.push_back(std::move(lv));
	}
	const int C = g.channels;
	for (int k = 1; k < g.weightLevels(); ++k)
	{
		const int dn = g.weightDim(k), sn = g.weightDim(k - 1);
		std::vector<uint8_t> lv(static_cast<size_t>(dn) * dn * C);
		for (int z = 0; z < dn; ++z)
			for (int x = 0; x < dn; ++x)
				for (int ch = 0; ch < C; ++ch)
				{
					const auto at = [&](int xx, int zz) { return static_cast<int>(weights[k - 1][(static_cast<size_t>(zz) * sn + xx) * C + ch]); };
					lv[(static_cast<size_t>(z) * dn + x) * C + ch] =
					    static_cast<uint8_t>((at(2 * x, 2 * z) + at(2 * x + 1, 2 * z) + at(2 * x, 2 * z + 1) + at(2 * x + 1, 2 * z + 1) + 2) / 4);
				}
		weights.push_back(std::move(lv));
	}
}

// ── LandscapeCodec ───────────────────────────────────────────────────────────
bool LandscapeCodec::available(Backend b)
{
	switch (b)
	{
	case Backend::Store: return true;
#ifdef HE_HAVE_LZ4
	case Backend::Lz4: return true;
#endif
#ifdef HE_HAVE_ZSTD
	case Backend::Zstd: return true;
#endif
	default: return false;
	}
}

LandscapeCodec LandscapeCodec::best()
{
	LandscapeCodec c;
	c.predictor = true;
	c.backend = available(Backend::Zstd) ? Backend::Zstd : (available(Backend::Lz4) ? Backend::Lz4 : Backend::Store);
	return c;
}

std::vector<uint8_t> encodeLandscapeTile(const LandscapeTile& tile, LandscapeCodec codec)
{
	const LandscapeGeometry& g = tile.geometry;
	if (!LandscapeCodec::available(codec.backend)) codec.backend = LandscapeCodec::Backend::Store;
	std::vector<uint8_t> out;
	if (!g.valid() || static_cast<int>(tile.heights.size()) != g.heightLevels() || static_cast<int>(tile.weights.size()) != g.weightLevels())
		return out;
	put32(out, kLandscapeTileMagic);
	out.push_back(kLandscapeTileVersion);
	out.push_back(static_cast<uint8_t>(codec.backend));
	const bool water = tile.hasWater() && tile.waterCoverage.size() == static_cast<size_t>(g.weightDim(0)) * g.weightDim(0);
	out.push_back(static_cast<uint8_t>((codec.predictor ? 1 : 0) | (water ? 2 : 0)));
	out.push_back(static_cast<uint8_t>(g.heightLevels()));
	out.push_back(static_cast<uint8_t>(g.weightLevels()));
	out.push_back(g.channels);
	put16(out, g.hCells);
	put16(out, g.wTexels);
	uint32_t bits;
	std::memcpy(&bits, &g.tileM, 4);
	put32(out, bits);

	std::vector<uint8_t> tmp;
	const auto stream = [&](const uint8_t* p, size_t n) {
		const auto c = compress(codec.backend, codec.zstdLevel, p, n);
		put32(out, static_cast<uint32_t>(n));
		put32(out, static_cast<uint32_t>(c.size()));
		out.insert(out.end(), c.begin(), c.end());
	};
	for (int k = 0; k < g.heightLevels(); ++k)
	{
		if (tile.heights[k].size() != static_cast<size_t>(g.heightDim(k)) * g.heightDim(k)) return {};
		if (codec.predictor) { heightForward(tile.heights[k], g.heightDim(k), g.heightDim(k), tmp); stream(tmp.data(), tmp.size()); }
		else stream(reinterpret_cast<const uint8_t*>(tile.heights[k].data()), tile.heights[k].size() * 2);
	}
	for (int k = 0; k < g.weightLevels(); ++k)
	{
		if (tile.weights[k].size() != static_cast<size_t>(g.weightDim(k)) * g.weightDim(k) * g.channels) return {};
		if (codec.predictor) { weightForward(tile.weights[k], g.weightDim(k), g.weightDim(k), g.channels, tmp); stream(tmp.data(), tmp.size()); }
		else stream(tile.weights[k].data(), tile.weights[k].size());
	}
	if (water)
	{
		const int w = g.weightDim(0);
		if (codec.predictor) { weightForward(tile.waterCoverage, w, w, 1, tmp); stream(tmp.data(), tmp.size()); }
		else stream(tile.waterCoverage.data(), tile.waterCoverage.size());
		if (codec.predictor) { heightForward(tile.waterOwner, w, w, tmp); stream(tmp.data(), tmp.size()); }
		else stream(reinterpret_cast<const uint8_t*>(tile.waterOwner.data()), tile.waterOwner.size() * 2);
	}
	return out;
}

bool peekLandscapeTile(const uint8_t* data, size_t size, LandscapeGeometry& geometry, LandscapeCodec* codec, std::string* error)
{
	Header h;
	if (!readHeader(data, size, h, error)) return false;
	geometry = h.geometry;
	if (codec) *codec = h.codec;
	return true;
}

bool decodeLandscapeTile(const uint8_t* data, size_t size, LandscapeTile& out, int maxLevels, std::string* error)
{
	Header h;
	if (!readHeader(data, size, h, error)) return false;
	if (!LandscapeCodec::available(h.codec.backend)) return fail(error, "this build cannot read the tile's backend");
	const LandscapeGeometry& g = h.geometry;
	out = LandscapeTile{};
	out.geometry = g;
	const int hl = maxLevels < 0 ? h.hLevels : std::min(maxLevels, h.hLevels);
	const int wl = maxLevels < 0 ? h.wLevels : std::min(maxLevels, h.wLevels);

	size_t pos = kLandscapeTileHeaderBytes;
	std::vector<uint8_t> raw;
	const auto next = [&](size_t expectedRaw, const uint8_t*& payload, uint32_t& stored) {
		if (size - pos < 8) return fail(error, "tile stream header runs past the blob");
		const uint32_t rawSize = get32(data + pos);
		stored = get32(data + pos + 4);
		if (rawSize != expectedRaw) return fail(error, "tile stream size does not match its geometry");
		if (stored > size - pos - 8) return fail(error, "tile stream runs past the blob");
		payload = data + pos + 8;
		pos += 8 + stored;
		return true;
	};
	for (int k = 0; k < h.hLevels; ++k)
	{
		const size_t rawN = static_cast<size_t>(g.heightDim(k)) * g.heightDim(k) * 2;
		const uint8_t* p = nullptr; uint32_t stored = 0;
		if (!next(rawN, p, stored)) return false;
		if (k >= hl) continue;
		raw.resize(rawN);
		if (!decompress(h.codec.backend, p, stored, raw.data(), rawN)) return fail(error, "tile height stream is corrupt");
		std::vector<uint16_t> v;
		if (h.codec.predictor) heightInverse(raw.data(), g.heightDim(k), g.heightDim(k), v);
		else { v.resize(rawN / 2); std::memcpy(v.data(), raw.data(), rawN); }
		out.heights.push_back(std::move(v));
	}
	for (int k = 0; k < h.wLevels; ++k)
	{
		const size_t rawN = static_cast<size_t>(g.weightDim(k)) * g.weightDim(k) * g.channels;
		const uint8_t* p = nullptr; uint32_t stored = 0;
		if (!next(rawN, p, stored)) return false;
		if (k >= wl) continue;
		raw.resize(rawN);
		if (!decompress(h.codec.backend, p, stored, raw.data(), rawN)) return fail(error, "tile weight stream is corrupt");
		std::vector<uint8_t> v;
		if (h.codec.predictor) weightInverse(raw.data(), g.weightDim(k), g.weightDim(k), g.channels, v);
		else v = raw;
		out.weights.push_back(std::move(v));
	}
	if (h.water && wl > 0)   // level-0 data: kept under any maxLevels that keeps level 0
	{
		const int w = g.weightDim(0);
		const size_t texels = static_cast<size_t>(w) * w;
		const uint8_t* p = nullptr; uint32_t stored = 0;
		if (!next(texels, p, stored)) return false;
		raw.resize(texels);
		if (!decompress(h.codec.backend, p, stored, raw.data(), texels)) return fail(error, "tile water coverage stream is corrupt");
		if (h.codec.predictor) weightInverse(raw.data(), w, w, 1, out.waterCoverage);
		else out.waterCoverage = raw;
		if (!next(texels * 2, p, stored)) return false;
		raw.resize(texels * 2);
		if (!decompress(h.codec.backend, p, stored, raw.data(), texels * 2)) return fail(error, "tile water owner stream is corrupt");
		if (h.codec.predictor) heightInverse(raw.data(), w, w, out.waterOwner);
		else { out.waterOwner.resize(texels); std::memcpy(out.waterOwner.data(), raw.data(), texels * 2); }
	}
	return true;
}

// ── grids (the coarse world) ─────────────────────────────────────────────────
namespace
{
constexpr uint32_t kGridMagic = 0x31474c48u;   // 'HLG1'
constexpr size_t   kGridHeaderBytes = 28;
constexpr uint8_t  kKindHeights = 1, kKindWeights = 2;

std::vector<uint8_t> encodeGrid(uint8_t kind, const uint8_t* data, size_t rawSize, int w, int h, int channels, bool predictor,
                                LandscapeCodec codec, const std::vector<uint8_t>& transformed)
{
	if (!LandscapeCodec::available(codec.backend)) codec.backend = LandscapeCodec::Backend::Store;
	const uint8_t* src = predictor ? transformed.data() : data;
	const auto c = compress(codec.backend, codec.zstdLevel, src, rawSize);
	std::vector<uint8_t> out;
	put32(out, kGridMagic);
	out.push_back(1);
	out.push_back(static_cast<uint8_t>(codec.backend));
	out.push_back(predictor ? 1 : 0);
	out.push_back(kind);
	out.push_back(static_cast<uint8_t>(channels));
	out.push_back(0); out.push_back(0); out.push_back(0);
	put32(out, static_cast<uint32_t>(w));
	put32(out, static_cast<uint32_t>(h));
	put32(out, static_cast<uint32_t>(rawSize));
	put32(out, static_cast<uint32_t>(c.size()));
	out.insert(out.end(), c.begin(), c.end());
	return out;
}

// Reads and validates the header and returns the decoded (inverse-transformed) payload in `raw` (kind-sized).
bool decodeGrid(const uint8_t* d, size_t size, uint8_t wantKind, int& w, int& h, int& channels, std::vector<uint8_t>& raw,
                bool& predictor, std::string* error)
{
	if (size < kGridHeaderBytes) return fail(error, "grid blob shorter than its header");
	if (get32(d) != kGridMagic) return fail(error, "not a landscape grid (bad magic)");
	if (d[4] != 1) return fail(error, "unknown landscape grid version");
	if (d[5] > static_cast<uint8_t>(LandscapeCodec::Backend::Zstd)) return fail(error, "unknown grid backend");
	const auto backend = static_cast<LandscapeCodec::Backend>(d[5]);
	predictor = (d[6] & 1) != 0;
	if (d[7] != wantKind) return fail(error, "grid is not of the expected kind");
	channels = d[8];
	const uint32_t gw = get32(d + 12), gh = get32(d + 16), rawSize = get32(d + 20), stored = get32(d + 24);
	if (gw == 0 || gh == 0 || gw > 65536 || gh > 65536) return fail(error, "grid size is not valid");
	if (wantKind == kKindWeights && (channels < 1 || channels > 16)) return fail(error, "grid channel count is not valid");
	const uint64_t expect = static_cast<uint64_t>(gw) * gh * (wantKind == kKindHeights ? 2 : channels);
	if (expect != rawSize || expect > (1ull << 31)) return fail(error, "grid payload size does not match its dimensions");
	if (stored > size - kGridHeaderBytes) return fail(error, "grid payload runs past the blob");
	if (!LandscapeCodec::available(backend)) return fail(error, "this build cannot read the grid's backend");
	w = static_cast<int>(gw);
	h = static_cast<int>(gh);
	raw.resize(rawSize);
	if (!decompress(backend, d + kGridHeaderBytes, stored, raw.data(), rawSize)) return fail(error, "grid payload is corrupt");
	return true;
}
} // namespace

std::vector<uint8_t> encodeLandscapeHeightGrid(const std::vector<uint16_t>& v, int w, int h, LandscapeCodec codec)
{
	if (w <= 0 || h <= 0 || v.size() != static_cast<size_t>(w) * h) return {};
	std::vector<uint8_t> tmp;
	if (codec.predictor) heightForward(v, w, h, tmp);
	return encodeGrid(kKindHeights, reinterpret_cast<const uint8_t*>(v.data()), v.size() * 2, w, h, 1, codec.predictor, codec, tmp);
}

bool decodeLandscapeHeightGrid(const uint8_t* data, size_t size, std::vector<uint16_t>& out, int& w, int& h, std::string* error)
{
	std::vector<uint8_t> raw;
	int ch = 0;
	bool pred = false;
	if (!decodeGrid(data, size, kKindHeights, w, h, ch, raw, pred, error)) return false;
	if (pred) heightInverse(raw.data(), w, h, out);
	else { out.resize(raw.size() / 2); std::memcpy(out.data(), raw.data(), raw.size()); }
	return true;
}

std::vector<uint8_t> encodeLandscapeWeightGrid(const std::vector<uint8_t>& v, int w, int h, int channels, LandscapeCodec codec)
{
	if (w <= 0 || h <= 0 || channels < 1 || channels > 16 || v.size() != static_cast<size_t>(w) * h * channels) return {};
	std::vector<uint8_t> tmp;
	if (codec.predictor) weightForward(v, w, h, channels, tmp);
	return encodeGrid(kKindWeights, v.data(), v.size(), w, h, channels, codec.predictor, codec, tmp);
}

bool decodeLandscapeWeightGrid(const uint8_t* data, size_t size, std::vector<uint8_t>& out, int& w, int& h, int& channels, std::string* error)
{
	std::vector<uint8_t> raw;
	bool pred = false;
	if (!decodeGrid(data, size, kKindWeights, w, h, channels, raw, pred, error)) return false;
	if (pred) weightInverse(raw.data(), w, h, channels, out);
	else out = std::move(raw);
	return true;
}

} // namespace HE
