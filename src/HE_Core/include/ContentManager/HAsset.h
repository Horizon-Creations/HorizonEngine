#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  .hasset  —  Horizon Engine binary asset format
//
//  Layout:
//    [ HAssetFileHeader  (32 bytes, fixed) ]
//    [ HAssetChunkHeader + data ] × chunk_count
//
//  All values are little-endian.
// ─────────────────────────────────────────────────────────────────────────────

namespace HAsset
{

// ── Magic & version ───────────────────────────────────────────────────────────
inline constexpr char     k_magic[4]  = { 'H', 'A', 'S', 'T' };
// v2: META chunk carries the asset UUID (16 bytes) between type and name.
inline constexpr uint16_t k_version   = 2;

// ── File header (32 bytes) ────────────────────────────────────────────────────
#pragma pack(push, 1)
struct FileHeader
{
	char     magic[4];       // "HAST"
	uint16_t version;        // k_version
	uint16_t asset_type;     // HE::AssetType cast to uint16
	uint32_t chunk_count;
	uint32_t flags;          // reserved / future use (compression, encryption)
	uint8_t  reserved[16];   // pad to 32 bytes
};
static_assert(sizeof(FileHeader) == 32, "HAsset::FileHeader must be 32 bytes");

// ── Chunk header (12 bytes) ───────────────────────────────────────────────────
struct ChunkHeader
{
	uint32_t id;       // 4-char tag packed as uint32 (little-endian)
	uint64_t size;     // byte length of the data that follows
};
static_assert(sizeof(ChunkHeader) == 12, "HAsset::ChunkHeader must be 12 bytes");
#pragma pack(pop)

// ── Chunk ID helpers ──────────────────────────────────────────────────────────
inline constexpr uint32_t makeChunkId(char a, char b, char c, char d) noexcept
{
	return  static_cast<uint32_t>(static_cast<uint8_t>(a))
		  | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8
		  | static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16
		  | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

// Common
inline constexpr uint32_t CHUNK_META = makeChunkId('M','E','T','A');

// Mesh
inline constexpr uint32_t CHUNK_VERT = makeChunkId('V','E','R','T');
inline constexpr uint32_t CHUNK_INDX = makeChunkId('I','N','D','X');
inline constexpr uint32_t CHUNK_NORM = makeChunkId('N','O','R','M');
inline constexpr uint32_t CHUNK_TEXC = makeChunkId('T','E','X','C'); // UVs
inline constexpr uint32_t CHUNK_BONE = makeChunkId('B','O','N','E'); // boneIDs
inline constexpr uint32_t CHUNK_BWGT = makeChunkId('B','W','G','T'); // bone weights
inline constexpr uint32_t CHUNK_SKEL = makeChunkId('S','K','E','L'); // skeleton hierarchy
inline constexpr uint32_t CHUNK_MREF = makeChunkId('M','R','E','F'); // material path
inline constexpr uint32_t CHUNK_MRFU = makeChunkId('M','R','F','U'); // material UUID (pack-time baked; POD HE::UUID)

// Texture
inline constexpr uint32_t CHUNK_TXMI = makeChunkId('T','X','M','I'); // texture meta
inline constexpr uint32_t CHUNK_PIXL = makeChunkId('P','I','X','L'); // pixel data

// Audio
inline constexpr uint32_t CHUNK_AUMI = makeChunkId('A','U','M','I'); // audio meta
inline constexpr uint32_t CHUNK_PCMD = makeChunkId('P','C','M','D'); // PCM data

// Material
inline constexpr uint32_t CHUNK_MTRL = makeChunkId('M','T','R','L'); // shader path + tex refs
inline constexpr uint32_t CHUNK_MTLU = makeChunkId('M','T','L','U'); // shader UUID + texture UUIDs (pack-time baked)

// Script / Shader (source text as UTF-8)
inline constexpr uint32_t CHUNK_SRC  = makeChunkId('S','R','C',' ');

// Font
inline constexpr uint32_t CHUNK_FNTD = makeChunkId('F','N','T','D'); // raw font bytes
inline constexpr uint32_t CHUNK_FNTI = makeChunkId('F','N','T','I'); // font meta (size)

// Scene
inline constexpr uint32_t CHUNK_SCNE = makeChunkId('S','C','N','E'); // object path list
inline constexpr uint32_t CHUNK_SCNU = makeChunkId('S','C','N','U'); // object UUIDs (pack-time baked)

// Animation
inline constexpr uint32_t CHUNK_ANIM = makeChunkId('A','N','I','M'); // duration + channels

// ─────────────────────────────────────────────────────────────────────────────
//  Writer
// ─────────────────────────────────────────────────────────────────────────────
class Writer
{
public:
	struct Chunk
	{
		uint32_t             id;
		std::vector<uint8_t> data;
	};

	void addChunk(uint32_t id, const void* data, uint64_t size)
	{
		Chunk c;
		c.id = id;
		c.data.resize(static_cast<size_t>(size));
		if (size > 0 && data)
			std::memcpy(c.data.data(), data, static_cast<size_t>(size));
		m_chunks.push_back(std::move(c));
	}

	// Convenience: write a length-prefixed string into a byte buffer
	static void appendString(std::vector<uint8_t>& buf, const std::string& s)
	{
		uint32_t len = static_cast<uint32_t>(s.size());
		appendPOD(buf, len);
		buf.insert(buf.end(), s.begin(), s.end());
	}

	// Convenience: write a POD value into a byte buffer
	template<typename T>
	static void appendPOD(std::vector<uint8_t>& buf, const T& value)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
		buf.insert(buf.end(), p, p + sizeof(T));
	}

	// Convenience: write a vector of POD elements
	template<typename T>
	static void appendVec(std::vector<uint8_t>& buf, const std::vector<T>& vec)
	{
		uint64_t count = vec.size();
		appendPOD(buf, count);
		if (count > 0)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(vec.data());
			buf.insert(buf.end(), p, p + count * sizeof(T));
		}
	}

	// std::string is not POD — serialize each element with appendString so the
	// format is portable across platforms with different sizeof(std::string).
	static void appendVec(std::vector<uint8_t>& buf, const std::vector<std::string>& vec)
	{
		uint64_t count = vec.size();
		appendPOD(buf, count);
		for (const auto& s : vec)
			appendString(buf, s);
	}

	bool write(const std::string& filePath, uint16_t assetType) const
	{
		std::ofstream f(filePath, std::ios::binary | std::ios::trunc);
		if (!f.is_open())
			return false;

		FileHeader hdr{};
		std::memcpy(hdr.magic, k_magic, 4);
		hdr.version     = k_version;
		hdr.asset_type  = assetType;
		hdr.chunk_count = static_cast<uint32_t>(m_chunks.size());
		hdr.flags       = 0;
		f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

		for (const auto& chunk : m_chunks)
		{
			ChunkHeader ch{};
			ch.id   = chunk.id;
			ch.size = chunk.data.size();
			f.write(reinterpret_cast<const char*>(&ch), sizeof(ch));
			if (ch.size > 0)
				f.write(reinterpret_cast<const char*>(chunk.data.data()),
						static_cast<std::streamsize>(ch.size));
		}
		return f.good();
	}

	// Serialize to an in-memory buffer (same layout as write(), no disk I/O).
	std::vector<uint8_t> toBytes(uint16_t assetType) const
	{
		FileHeader hdr{};
		std::memcpy(hdr.magic, k_magic, 4);
		hdr.version     = k_version;
		hdr.asset_type  = assetType;
		hdr.chunk_count = static_cast<uint32_t>(m_chunks.size());
		hdr.flags       = 0;

		std::vector<uint8_t> out(sizeof(hdr));
		std::memcpy(out.data(), &hdr, sizeof(hdr));

		for (const auto& chunk : m_chunks)
		{
			ChunkHeader ch{};
			ch.id   = chunk.id;
			ch.size = chunk.data.size();
			const auto* chp = reinterpret_cast<const uint8_t*>(&ch);
			out.insert(out.end(), chp, chp + sizeof(ch));
			out.insert(out.end(), chunk.data.begin(), chunk.data.end());
		}
		return out;
	}

private:
	std::vector<Chunk> m_chunks;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Reader
// ─────────────────────────────────────────────────────────────────────────────
class Reader
{
public:
	struct Chunk
	{
		uint32_t             id;
		std::vector<uint8_t> data;
	};

	bool open(const std::string& filePath)
	{
		std::ifstream f(filePath, std::ios::binary);
		if (!f.is_open())
			return false;

		f.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
		if (!f || std::memcmp(m_header.magic, k_magic, 4) != 0)
			return false;

		m_chunks.clear();
		m_chunks.reserve(m_header.chunk_count);

		for (uint32_t i = 0; i < m_header.chunk_count; ++i)
		{
			ChunkHeader ch{};
			f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
			if (!f) break;

			Chunk c;
			c.id = ch.id;
			c.data.resize(static_cast<size_t>(ch.size));
			if (ch.size > 0)
				f.read(reinterpret_cast<char*>(c.data.data()),
					   static_cast<std::streamsize>(ch.size));
			m_chunks.push_back(std::move(c));
		}
		return true;
	}

	// Parse an in-memory .hasset blob (same format as open(), no disk I/O).
	bool openData(const std::vector<uint8_t>& data)
	{
		if (data.size() < sizeof(FileHeader)) return false;
		std::memcpy(&m_header, data.data(), sizeof(m_header));
		if (std::memcmp(m_header.magic, k_magic, 4) != 0) return false;

		m_chunks.clear();
		m_chunks.reserve(m_header.chunk_count);

		size_t offset = sizeof(FileHeader);
		for (uint32_t i = 0; i < m_header.chunk_count; ++i)
		{
			if (offset + sizeof(ChunkHeader) > data.size()) break;
			ChunkHeader ch{};
			std::memcpy(&ch, data.data() + offset, sizeof(ch));
			offset += sizeof(ChunkHeader);

			Chunk c;
			c.id = ch.id;
			c.data.resize(static_cast<size_t>(ch.size));
			if (ch.size > 0)
			{
				if (offset + static_cast<size_t>(ch.size) > data.size()) return false;
				std::memcpy(c.data.data(), data.data() + offset,
							static_cast<size_t>(ch.size));
				offset += static_cast<size_t>(ch.size);
			}
			m_chunks.push_back(std::move(c));
		}
		return true;
	}

	const FileHeader& header() const { return m_header; }
	uint16_t          assetType() const { return m_header.asset_type; }

	// Find first chunk with given id, nullptr if not found
	const Chunk* findChunk(uint32_t id) const
	{
		for (const auto& c : m_chunks)
			if (c.id == id) return &c;
		return nullptr;
	}

	// ── Read helpers ──────────────────────────────────────────────────────────

	// Read a length-prefixed string from a buffer at offset (offset is advanced)
	static bool readString(const std::vector<uint8_t>& buf, size_t& offset, std::string& out)
	{
		if (offset + sizeof(uint32_t) > buf.size()) return false;
		uint32_t len = 0;
		std::memcpy(&len, buf.data() + offset, sizeof(len));
		offset += sizeof(len);
		if (offset + len > buf.size()) return false;
		out.assign(reinterpret_cast<const char*>(buf.data() + offset), len);
		offset += len;
		return true;
	}

	template<typename T>
	static bool readPOD(const std::vector<uint8_t>& buf, size_t& offset, T& out)
	{
		if (offset + sizeof(T) > buf.size()) return false;
		std::memcpy(&out, buf.data() + offset, sizeof(T));
		offset += sizeof(T);
		return true;
	}

	template<typename T>
	static bool readVec(const std::vector<uint8_t>& buf, size_t& offset, std::vector<T>& out)
	{
		uint64_t count = 0;
		if (!readPOD(buf, offset, count)) return false;
		if (offset + count * sizeof(T) > buf.size()) return false;
		out.resize(static_cast<size_t>(count));
		if (count > 0)
		{
			std::memcpy(out.data(), buf.data() + offset,
						static_cast<size_t>(count) * sizeof(T));
			offset += static_cast<size_t>(count) * sizeof(T);
		}
		return true;
	}

	// std::string is not POD — read each element with readString so the format
	// is portable across platforms with different sizeof(std::string).
	static bool readVec(const std::vector<uint8_t>& buf, size_t& offset, std::vector<std::string>& out)
	{
		uint64_t count = 0;
		if (!readPOD(buf, offset, count)) return false;
		out.resize(static_cast<size_t>(count));
		for (auto& s : out)
			if (!readString(buf, offset, s)) return false;
		return true;
	}

private:
	FileHeader         m_header{};
	std::vector<Chunk> m_chunks;
};

} // namespace HAsset
