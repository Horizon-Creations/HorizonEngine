#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/TerrainMeshGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace HE
{
namespace
{
void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

bool inWorld(const LandscapeTileSource& s, LandscapeTileKey k) { return k.x >= 0 && k.z >= 0 && k.x < s.tilesX() && k.z < s.tilesZ(); }
} // namespace

// ── Procedural ───────────────────────────────────────────────────────────────
LandscapeTile ProceduralLandscapeSource::generate(LandscapeTileKey key) const
{
	const LandscapeGeometry& g = m_p.geometry;
	LandscapeTile t;
	t.geometry = g;
	const double worldM = static_cast<double>(std::max(m_p.tilesX, m_p.tilesZ)) * g.tileM;
	const double x0 = key.x * static_cast<double>(g.tileM), z0 = key.z * static_cast<double>(g.tileM);
	const double rasterH = g.tileM / g.hCells, rasterW = g.tileM / g.wTexels;
	const auto fbmAt = [&](double wx, double wz, int seed, int octaves, float freq) {
		return terrainFbm(seed, static_cast<float>(wx / worldM), static_cast<float>(wz / worldM), octaves, freq, 2.0f, 0.5f);
	};

	const int n = g.heightDim(0);
	std::vector<uint16_t> h0(static_cast<size_t>(n) * n);
	std::mt19937 rng(static_cast<uint32_t>(key.x * 73856093u ^ key.z * 19349663u));
	for (int z = 0; z < n; ++z)
		for (int x = 0; x < n; ++x)
		{
			const float f = fbmAt(x0 + x * rasterH, z0 + z * rasterH, m_p.seed, m_p.octaves, m_p.baseFrequency);
			int v = static_cast<int>(std::lround(f * 65535.0f));
			if (m_p.roughLsb > 0) v += static_cast<int>(rng() % (2u * m_p.roughLsb + 1u)) - m_p.roughLsb;
			h0[static_cast<size_t>(z) * n + x] = static_cast<uint16_t>(std::clamp(v, 0, 65535));
		}
	t.heights.push_back(std::move(h0));

	// Weights: height bands x slope x per-layer noise, normalised to 255.
	const int W = g.wTexels, C = g.channels, step = std::max(1, static_cast<int>(std::lround(rasterW / rasterH)));
	std::vector<uint8_t> w0(static_cast<size_t>(W) * W * C);
	std::vector<float>   s(C);
	const auto& H = t.heights[0];
	for (int z = 0; z < W; ++z)
		for (int x = 0; x < W; ++x)
		{
			const int hx = std::min(x * step, n - 1), hz = std::min(z * step, n - 1);
			const int hx1 = std::min(hx + 1, n - 1), hz1 = std::min(hz + 1, n - 1);
			const float hv = H[static_cast<size_t>(hz) * n + hx] / 65535.0f;
			const float dx = (H[static_cast<size_t>(hz) * n + hx1] - H[static_cast<size_t>(hz) * n + hx]) / 65535.0f;
			const float dz = (H[static_cast<size_t>(hz1) * n + hx] - H[static_cast<size_t>(hz) * n + hx]) / 65535.0f;
			const float slope = std::min(1.0f, std::sqrt(dx * dx + dz * dz) * m_p.heightScale / static_cast<float>(rasterH));
			float sum = 0.0f;
			for (int k = 0; k < C; ++k)
			{
				const float center = (k + 0.5f) / C;
				const float band   = std::max(0.0f, 1.0f - std::abs(hv - center) * C * 0.7f);
				const float nz     = fbmAt(x0 + x * rasterW, z0 + z * rasterW, 100 + k, 3, 400.0f);
				s[k] = band * (k % 3 == 0 ? 1.0f - slope : 0.4f + 0.6f * slope) + 0.25f * nz;
				sum += s[k];
			}
			int acc = 0, best = 0;
			uint8_t* p = &w0[(static_cast<size_t>(z) * W + x) * C];
			for (int k = 0; k < C; ++k)
			{
				p[k] = static_cast<uint8_t>(std::lround(255.0f * s[k] / std::max(sum, 1e-6f)));
				acc += p[k];
				if (p[k] > p[best]) best = k;
			}
			p[best] = static_cast<uint8_t>(std::clamp(p[best] + 255 - acc, 0, 255));
		}
	t.weights.push_back(std::move(w0));
	t.buildMips();
	return t;
}

bool ProceduralLandscapeSource::load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const
{
	if (!inWorld(*this, key) || !m_p.geometry.valid()) return false;
	out = generate(key);
	if (maxLevels >= 0)
	{
		out.heights.resize(std::min<size_t>(out.heights.size(), static_cast<size_t>(maxLevels)));
		out.weights.resize(std::min<size_t>(out.weights.size(), static_cast<size_t>(maxLevels)));
	}
	return true;
}

// ── Pack writer ──────────────────────────────────────────────────────────────
LandscapePackWriter::~LandscapePackWriter()
{
	if (m_open) m_out.close();
}

bool LandscapePackWriter::begin(const std::string& path, const LandscapeGeometry& geometry, int tilesX, int tilesZ)
{
	if (!geometry.valid() || tilesX <= 0 || tilesZ <= 0) return false;
	m_geometry = geometry;
	m_tilesX = tilesX;
	m_tilesZ = tilesZ;
	const size_t n = static_cast<size_t>(tilesX) * tilesZ;
	m_offset.assign(n, 0);
	m_size.assign(n, 0);
	m_written = 0;
	m_out.open(path, std::ios::binary | std::ios::trunc);
	if (!m_out) return false;
	m_open = true;
	// The header and the index are written by finish(); leave their room now.
	const uint64_t room = kLandscapePackHeaderBytes + n * kLandscapePackIndexEntryBytes;
	std::vector<char> zeros(1 << 16, 0);
	for (uint64_t left = room; left;)
	{
		const size_t c = static_cast<size_t>(std::min<uint64_t>(left, zeros.size()));
		m_out.write(zeros.data(), static_cast<std::streamsize>(c));
		left -= c;
	}
	m_pos = room;
	return static_cast<bool>(m_out);
}

bool LandscapePackWriter::add(LandscapeTileKey key, const std::vector<uint8_t>& blob)
{
	if (!m_open || key.x < 0 || key.z < 0 || key.x >= m_tilesX || key.z >= m_tilesZ || blob.empty() || blob.size() > 0xFFFFFFFFull) return false;
	LandscapeGeometry g;
	if (!peekLandscapeTile(blob.data(), blob.size(), g) || g != m_geometry) return false;
	const size_t idx = static_cast<size_t>(key.z) * m_tilesX + key.x;
	if (m_size[idx] != 0) return false;   // each tile once
	m_out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
	m_offset[idx] = m_pos;
	m_size[idx]   = static_cast<uint32_t>(blob.size());
	m_pos += blob.size();
	++m_written;
	return static_cast<bool>(m_out);
}

bool LandscapePackWriter::finish()
{
	if (!m_open) return false;
	std::vector<uint8_t> head(kLandscapePackHeaderBytes + m_offset.size() * kLandscapePackIndexEntryBytes, 0);
	put32(&head[0], kLandscapePackMagic);
	put32(&head[4], kLandscapePackVersion);
	put32(&head[8], static_cast<uint32_t>(m_tilesX));
	put32(&head[12], static_cast<uint32_t>(m_tilesZ));
	uint32_t bits;
	std::memcpy(&bits, &m_geometry.tileM, 4);
	put32(&head[16], bits);
	put16(&head[20], m_geometry.hCells);
	put16(&head[22], m_geometry.wTexels);
	head[24] = m_geometry.channels;
	for (size_t i = 0; i < m_offset.size(); ++i)
	{
		put64(&head[kLandscapePackHeaderBytes + i * kLandscapePackIndexEntryBytes], m_offset[i]);
		put32(&head[kLandscapePackHeaderBytes + i * kLandscapePackIndexEntryBytes + 8], m_size[i]);
	}
	m_out.seekp(0);
	m_out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
	m_out.close();
	m_open = false;
	return !m_out.fail();
}

// ── Pack source ──────────────────────────────────────────────────────────────
LandscapePackSource::~LandscapePackSource()
{
#ifndef _WIN32
	if (m_fd >= 0) ::close(m_fd);
#endif
}

bool LandscapePackSource::readAt(uint64_t offset, uint8_t* dst, size_t n) const
{
#ifndef _WIN32
	size_t done = 0;
	while (done < n)
	{
		const ssize_t r = ::pread(m_fd, dst + done, n - done, static_cast<off_t>(offset + done));
		if (r <= 0) return false;
		done += static_cast<size_t>(r);
	}
	return true;
#else
	std::lock_guard<std::mutex> lock(m_mutex);
	m_in.clear();
	m_in.seekg(static_cast<std::streamoff>(offset));
	return static_cast<bool>(m_in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n)));
#endif
}

std::shared_ptr<LandscapePackSource> LandscapePackSource::open(const std::string& path, std::string* error)
{
	const auto fail = [&](const char* why) -> std::shared_ptr<LandscapePackSource> {
		if (error) *error = why;
		return nullptr;
	};
	std::error_code ec;
	const uint64_t fileSize = std::filesystem::file_size(path, ec);
	if (ec) return fail("pack file not found");
	if (fileSize < kLandscapePackHeaderBytes) return fail("pack file shorter than its header");

	std::shared_ptr<LandscapePackSource> src(new LandscapePackSource());
	src->m_fileSize = fileSize;
#ifndef _WIN32
	src->m_fd = ::open(path.c_str(), O_RDONLY);
	if (src->m_fd < 0) return fail("pack file cannot be opened");
#else
	src->m_in.open(path, std::ios::binary);
	if (!src->m_in) return fail("pack file cannot be opened");
#endif
	uint8_t head[kLandscapePackHeaderBytes];
	if (!src->readAt(0, head, sizeof(head))) return fail("pack header cannot be read");
	if (get32(head) != kLandscapePackMagic) return fail("not a landscape pack (bad magic)");
	if (get32(head + 4) != kLandscapePackVersion) return fail("unknown landscape pack version");
	const uint32_t tx = get32(head + 8), tz = get32(head + 12);
	if (tx == 0 || tz == 0 || tx > 65536 || tz > 65536) return fail("pack world size is not valid");
	uint32_t bits = get32(head + 16);
	std::memcpy(&src->m_geometry.tileM, &bits, 4);
	src->m_geometry.hCells   = get16(head + 20);
	src->m_geometry.wTexels  = get16(head + 22);
	src->m_geometry.channels = head[24];
	if (!src->m_geometry.valid()) return fail("pack geometry is not valid");
	src->m_tilesX = static_cast<int>(tx);
	src->m_tilesZ = static_cast<int>(tz);

	const size_t n = static_cast<size_t>(tx) * tz;
	const uint64_t indexEnd = kLandscapePackHeaderBytes + static_cast<uint64_t>(n) * kLandscapePackIndexEntryBytes;
	if (indexEnd > fileSize) return fail("pack index runs past the end of the file");
	std::vector<uint8_t> index(n * kLandscapePackIndexEntryBytes);
	if (!src->readAt(kLandscapePackHeaderBytes, index.data(), index.size())) return fail("pack index cannot be read");
	src->m_offset.resize(n);
	src->m_size.resize(n);
	for (size_t i = 0; i < n; ++i)
	{
		src->m_offset[i] = get64(&index[i * kLandscapePackIndexEntryBytes]);
		src->m_size[i]   = get32(&index[i * kLandscapePackIndexEntryBytes + 8]);
		if (src->m_size[i] && (src->m_offset[i] < indexEnd || src->m_offset[i] + src->m_size[i] > fileSize))
			return fail("pack index points outside the file");
	}
	return src;
}

bool LandscapePackSource::has(LandscapeTileKey key) const { return storedSize(key) != 0; }

uint32_t LandscapePackSource::storedSize(LandscapeTileKey key) const
{
	return inWorld(*this, key) ? m_size[static_cast<size_t>(key.z) * m_tilesX + key.x] : 0;
}

bool LandscapePackSource::load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const
{
	if (!inWorld(*this, key)) return false;
	const size_t idx = static_cast<size_t>(key.z) * m_tilesX + key.x;
	if (m_size[idx] == 0) return false;
	std::vector<uint8_t> blob(m_size[idx]);
	if (!readAt(m_offset[idx], blob.data(), blob.size())) return false;
	if (!decodeLandscapeTile(blob.data(), blob.size(), out, maxLevels)) return false;
	return out.geometry == m_geometry;
}

} // namespace HE
