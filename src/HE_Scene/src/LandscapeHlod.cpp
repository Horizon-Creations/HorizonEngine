#include "HorizonScene/LandscapeHlod.h"
#include "HorizonScene/CellStreamer.h"

#include <glm/geometric.hpp>

#include <cstring>

namespace HE
{

LandscapeHlodMesh bakeCellHlod(const std::vector<LandscapeHlodSourceMesh>& sources)
{
	LandscapeHlodMesh out;
	for (const LandscapeHlodSourceMesh& s : sources)
	{
		if (s.indices.empty() || s.indices.size() % 3 != 0) continue;
		bool inBounds = true;
		for (uint32_t i : s.indices)
			if (i >= s.positions.size()) { inBounds = false; break; }
		if (!inBounds) continue;

		const uint32_t base = static_cast<uint32_t>(out.vertices.size());
		out.vertices.reserve(out.vertices.size() + s.positions.size());
		for (size_t i = 0; i < s.positions.size(); ++i)
		{
			LandscapeHlodVertex v;
			v.position = s.positions[i];
			v.normal   = i < s.normals.size() ? s.normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
			v.uv       = i < s.uvs.size() ? s.uvs[i] : glm::vec2(0.0f);
			out.vertices.push_back(v);
		}
		// A source without its own normals: the flat per-triangle normal, so the
		// merge still shades like a real mesh instead of pointing straight up.
		if (s.normals.size() < s.positions.size())
		{
			for (size_t t = 0; t + 2 < s.indices.size(); t += 3)
			{
				const uint32_t ia = s.indices[t], ib = s.indices[t + 1], ic = s.indices[t + 2];
				const glm::vec3 a = s.positions[ia], b = s.positions[ib], c = s.positions[ic];
				const glm::vec3 n = glm::cross(b - a, c - a);
				const float len = glm::length(n);
				if (len <= 0.0f) continue;
				const glm::vec3 flat = n / len;
				out.vertices[base + ia].normal = flat;
				out.vertices[base + ib].normal = flat;
				out.vertices[base + ic].normal = flat;
			}
		}
		out.indices.reserve(out.indices.size() + s.indices.size());
		for (uint32_t i : s.indices) out.indices.push_back(base + i);
	}
	return out;
}

void LandscapeHlodTable::set(int cellX, int cellZ, LandscapeHlodMesh mesh)
{
	if (mesh.empty()) { m_cells.erase({ cellX, cellZ }); return; }
	m_cells[{ cellX, cellZ }] = std::move(mesh);
}

const LandscapeHlodMesh* LandscapeHlodTable::find(int cellX, int cellZ) const
{
	const auto it = m_cells.find({ cellX, cellZ });
	return it == m_cells.end() ? nullptr : &it->second;
}

std::vector<LandscapeHlodCellKey> LandscapeHlodTable::cells() const
{
	std::vector<LandscapeHlodCellKey> out;
	out.reserve(m_cells.size());
	for (const auto& [k, mesh] : m_cells) out.push_back(k);
	return out;
}

size_t LandscapeHlodTable::bytes() const
{
	size_t b = 0;
	for (const auto& [k, mesh] : m_cells) b += mesh.bytes();
	return b;
}

namespace
{
constexpr uint32_t kHlodTableMagic   = 0x54444c48u;   // 'HLDT' little-endian
constexpr uint32_t kHlodTableVersion = 1;

template <class T>
void put(std::vector<uint8_t>& out, const T& v)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
	out.insert(out.end(), p, p + sizeof(T));
}

// Reads `n` bytes at `pos` into `out`, advancing `pos`. False: not enough bytes left.
bool get(const std::vector<uint8_t>& bytes, size_t& pos, void* out, size_t n)
{
	if (pos + n > bytes.size()) return false;
	std::memcpy(out, bytes.data() + pos, n);
	pos += n;
	return true;
}

template <class T>
bool getVal(const std::vector<uint8_t>& bytes, size_t& pos, T& out)
{
	return get(bytes, pos, &out, sizeof(T));
}
} // namespace

std::vector<uint8_t> LandscapeHlodTable::encode() const
{
	std::vector<uint8_t> out;
	put(out, kHlodTableMagic);
	put(out, kHlodTableVersion);
	put(out, static_cast<uint32_t>(m_cells.size()));
	for (const auto& [k, mesh] : m_cells)
	{
		put(out, static_cast<int32_t>(k.x));
		put(out, static_cast<int32_t>(k.z));
		put(out, static_cast<uint32_t>(mesh.vertices.size()));
		put(out, static_cast<uint32_t>(mesh.indices.size()));
		if (!mesh.vertices.empty())
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(mesh.vertices.data());
			out.insert(out.end(), p, p + mesh.vertices.size() * sizeof(LandscapeHlodVertex));
		}
		if (!mesh.indices.empty())
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(mesh.indices.data());
			out.insert(out.end(), p, p + mesh.indices.size() * sizeof(uint32_t));
		}
	}
	return out;
}

bool LandscapeHlodTable::decode(const std::vector<uint8_t>& bytes, LandscapeHlodTable& out)
{
	out.clear();
	size_t pos = 0;
	uint32_t magic = 0, version = 0, cellCount = 0;
	if (!getVal(bytes, pos, magic) || magic != kHlodTableMagic) return false;
	if (!getVal(bytes, pos, version) || version != kHlodTableVersion) return false;
	if (!getVal(bytes, pos, cellCount)) return false;
	for (uint32_t i = 0; i < cellCount; ++i)
	{
		int32_t x = 0, z = 0;
		uint32_t vertexCount = 0, indexCount = 0;
		if (!getVal(bytes, pos, x) || !getVal(bytes, pos, z)) return false;
		if (!getVal(bytes, pos, vertexCount) || !getVal(bytes, pos, indexCount)) return false;
		LandscapeHlodMesh mesh;
		mesh.vertices.resize(vertexCount);
		if (vertexCount > 0 && !get(bytes, pos, mesh.vertices.data(), vertexCount * sizeof(LandscapeHlodVertex)))
			return false;
		mesh.indices.resize(indexCount);
		if (indexCount > 0 && !get(bytes, pos, mesh.indices.data(), indexCount * sizeof(uint32_t)))
			return false;
		out.m_cells[{ x, z }] = std::move(mesh);
	}
	return true;
}

std::vector<LandscapeHlodCellKey> hlodCellsToDraw(const LandscapeHlodTable& table, const CellStreamer& streamer)
{
	std::vector<LandscapeHlodCellKey> out;
	for (const LandscapeHlodCellKey& k : table.cells())
		if (!streamer.isLoaded(k.x, k.z)) out.push_back(k);
	return out;
}

} // namespace HE
