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
	uint32_t flags;          // reserved: always written as 0, never read. Compression
	                         // and encryption happen in the .hpak container, not here.
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
// Pack-time COOKED static-mesh vertex buffer: the exact interleaved 8-float
// (pos3+norm3+uv2) layout both GPU backends build at runtime, plus vertexCount
// and the precomputed AABB. Replaces VERT/NORM/TEXC in a cooked pak so the
// runtime uploads it directly (no per-load interleave loop, no AABB scan).
// INDX is kept as-is. Absent in loose/editor assets (SoA fallback).
inline constexpr uint32_t CHUNK_MVBO = makeChunkId('M','V','B','O');
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
inline constexpr uint32_t CHUNK_SLNG = makeChunkId('S','L','N','G'); // script language (1 byte; absent → Lua)
inline constexpr uint32_t CHUNK_MGRF = makeChunkId('M','G','R','F'); // material-function node graph (JSON)
inline constexpr uint32_t CHUNK_PSHD = makeChunkId('P','S','H','D'); // precompiled per-backend material shaders

// UI Widget
inline constexpr uint32_t CHUNK_UIWT = makeChunkId('U','I','W','T'); // UI widget tree (JSON)
inline constexpr uint32_t CHUNK_UIWG = makeChunkId('U','I','W','G'); // UI widget logic graph (JSON)
inline constexpr uint32_t CHUNK_HCGR = makeChunkId('H','C','G','R'); // HorizonCode class graph (JSON)
inline constexpr uint32_t CHUNK_HCBC = makeChunkId('H','C','B','C'); // HorizonCode base class name (UTF-8; absent → plain Object)

// Input
inline constexpr uint32_t CHUNK_IACT = makeChunkId('I','A','C','T'); // input action definition (JSON)
inline constexpr uint32_t CHUNK_IMAP = makeChunkId('I','M','A','P'); // input mapping context (JSON)

// Particle System
inline constexpr uint32_t CHUNK_PTGR = makeChunkId('P','T','G','R'); // particle emitter node graph (JSON)
inline constexpr uint32_t CHUNK_PPSD = makeChunkId('P','P','S','D'); // precompiled per-backend particle color/alpha-over-life shaders

// Animator State Machine
inline constexpr uint32_t CHUNK_ASMG = makeChunkId('A','S','M','G'); // states/transitions/params graph (JSON)

// User-defined types (HE::TypeRegistry round-trips the JSON)
inline constexpr uint32_t CHUNK_STDF = makeChunkId('S','T','D','F'); // struct definition (JSON)
inline constexpr uint32_t CHUNK_ENDF = makeChunkId('E','N','D','F'); // enum definition (JSON)
inline constexpr uint32_t CHUNK_SGTP = makeChunkId('S','G','T','P'); // savegame template (JSON, struct-def shape)

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

// Read ONLY the 32-byte file header (asset type + version), never the chunk
// payloads. Reader::open() below pulls every chunk into memory, which is the wrong
// tool for a directory scan that just asks "what kind of asset is this?": on a
// content tree holding a few hundred-megabyte meshes, filling a dropdown that way
// reads the entire tree off disk. Returns false if the file is missing or not an
// .hasset.
inline bool readAssetTypeFromFile(const std::string& filePath, uint16_t& typeOut)
{
	std::ifstream f(filePath, std::ios::binary);
	if (!f.is_open()) return false;
	FileHeader hdr{};
	f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!f || std::memcmp(hdr.magic, k_magic, 4) != 0) return false;
	typeOut = hdr.asset_type;
	return true;
}

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
		// Open at the end so the real file size is known up front: every declared
		// size in the file is untrusted and has to be bounded against it before a
		// single byte is allocated (the in-memory openData() path bounds against
		// data.size() the same way).
		std::ifstream f(filePath, std::ios::binary | std::ios::ate);
		if (!f.is_open())
			return false;

		const std::streamoff fileEnd = f.tellg();
		if (fileEnd < static_cast<std::streamoff>(sizeof(FileHeader)))
			return false;
		const uint64_t fileSize = static_cast<uint64_t>(fileEnd);
		f.seekg(0, std::ios::beg);

		f.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
		if (!f || std::memcmp(m_header.magic, k_magic, 4) != 0)
			return false;

		m_chunks.clear();
		// chunk_count is attacker-controlled uint32: reserving it verbatim would
		// allocate gigabytes for a corrupt header (and throw bad_alloc) before a
		// single chunk is read. Every chunk costs at least its 12-byte header, so
		// the file size is a hard upper bound on how many can possibly follow.
		const uint64_t maxChunks = (fileSize - sizeof(FileHeader)) / sizeof(ChunkHeader);
		m_chunks.reserve(static_cast<size_t>(
			m_header.chunk_count < maxChunks ? m_header.chunk_count : maxChunks));

		uint64_t offset = sizeof(FileHeader);
		for (uint32_t i = 0; i < m_header.chunk_count; ++i)
		{
			ChunkHeader ch{};
			f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
			if (!f) break;
			offset += sizeof(ChunkHeader);   // the read succeeded → still <= fileSize

			// Validate the declared size BEFORE allocating, exactly like openData():
			// a corrupt/hostile size would otherwise resize() to gigabytes (throwing
			// bad_alloc out of the parse) and the short read afterwards would leave
			// the tail silently zero-filled. Compare against the remaining bytes —
			// `offset + ch.size` could wrap, since size is an untrusted uint64.
			if (ch.size > fileSize - offset) return false;

			Chunk c;
			c.id = ch.id;
			c.data.resize(static_cast<size_t>(ch.size));
			if (ch.size > 0)
				f.read(reinterpret_cast<char*>(c.data.data()),
					   static_cast<std::streamsize>(ch.size));
			offset += ch.size;
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
		// Same cap as open(): chunk_count is untrusted, and a blind reserve of a
		// garbage uint32 throws bad_alloc before the loop can reject anything.
		const size_t maxChunks = (data.size() - sizeof(FileHeader)) / sizeof(ChunkHeader);
		m_chunks.reserve(m_header.chunk_count < maxChunks
						 ? static_cast<size_t>(m_header.chunk_count) : maxChunks);

		size_t offset = sizeof(FileHeader);
		for (uint32_t i = 0; i < m_header.chunk_count; ++i)
		{
			if (offset + sizeof(ChunkHeader) > data.size()) break;
			ChunkHeader ch{};
			std::memcpy(&ch, data.data() + offset, sizeof(ch));
			offset += sizeof(ChunkHeader);

			// Validate the declared size BEFORE allocating: a corrupt/hostile size
			// would otherwise throw bad_alloc/length_error out of the resize.
			// Compare against the remaining bytes — `offset + ch.size` could wrap
			// (size is attacker-controlled uint64) and slip past the check. Compare
			// as uint64 too: casting size down to size_t first would let a value
			// like 0x1'0000'0004 truncate to 4 and pass on a 32-bit build.
			if (ch.size > static_cast<uint64_t>(data.size() - offset)) return false;
			Chunk c;
			c.id = ch.id;
			c.data.resize(static_cast<size_t>(ch.size));
			if (ch.size > 0)
			{
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

	// All chunks in file order — for transforms that reserialize an asset
	// (e.g. the pack-time path→UUID reference rewrite).
	const std::vector<Chunk>& chunks() const { return m_chunks; }

	// ── Read helpers ──────────────────────────────────────────────────────────

	// Read a length-prefixed string from a buffer at offset (offset is advanced)
	static bool readString(const std::vector<uint8_t>& buf, size_t& offset, std::string& out)
	{
		if (offset + sizeof(uint32_t) > buf.size()) return false;
		uint32_t len = 0;
		std::memcpy(&len, buf.data() + offset, sizeof(len));
		offset += sizeof(len);
		// Subtract instead of adding: len is a file value up to 4 GiB, so on a
		// 32-bit build `offset + len` wraps and a truncated string would pass.
		if (len > buf.size() - offset) return false;
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
		// Check the count by DIVISION, never by forming count * sizeof(T): count
		// comes straight from the file, so the product wraps around for a hostile
		// or corrupt value and lets an absurd count sail past the bounds test —
		// after which resize() allocates count * sizeof(T) (bad_alloc → the parse
		// aborts the process) and the memcpy below runs off the end of the buffer.
		// readPOD only advances offset when it fits, so buf.size() - offset is safe.
		if (count > (buf.size() - offset) / sizeof(T)) return false;
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
		// Sanity-check the count BEFORE resizing. Every element costs at least its
		// 4-byte length prefix, so a count larger than the bytes left can only come
		// from a truncated or corrupt chunk — and resizing to it allocates
		// count * sizeof(std::string), which for a garbage 64-bit value throws
		// bad_alloc and aborts the process instead of failing the parse.
		//
		// This is reachable from any append-only chunk tail: a preceding truncated
		// readString leaves `offset` advanced past its length prefix before it
		// returns false, so the next field reads its count from a misaligned
		// position. The POD overload has always had this guard; this one had not.
		if (count > (buf.size() - offset) / sizeof(uint32_t)) return false;
		out.resize(static_cast<size_t>(count));
		for (auto& s : out)
			if (!readString(buf, offset, s)) return false;
		return true;
	}

private:
	FileHeader         m_header{};
	std::vector<Chunk> m_chunks;
};

// ── TXMI header codec (width / height / channels) ─────────────────────────────
// These three fields USED to be written as size_t, which made the on-disk chunk
// layout depend on the writing build's pointer width (24 bytes on 64-bit vs 12 on
// 32-bit for the same texture). They are uint32 now; the legacy 64-bit layout is
// still read, told apart by the chunk size alone:
//   legacy 64-bit : 24 B (width/height/channels only) or 30 B (+ mip/format/srgb tail)
//   current       : 12 B + the 6-byte tail = 18 B
// so "chunk >= 24 bytes" can only be the legacy layout, and a legacy 32-bit chunk
// is byte-identical to the current one and needs no special case. Every reader of
// TXMI must go through readTextureHeader (ContentManager's loader and the packer's
// cookTexture both do) or old .hasset files decode to garbage dimensions.
// CAUTION: the discriminator holds only while the current layout stays under 24
// bytes, i.e. the optional tail after channels stays ≤ 11 bytes (6 today). A
// bigger tail needs a real version marker instead.
inline constexpr size_t kTextureHeaderLegacyMinSize = 24;

// Reads width/height/channels at `offset` (advanced past them). False = truncated.
// `buf` must be the WHOLE TXMI chunk starting at offset 0 — the layout is chosen
// from its total size.
inline bool readTextureHeader(const std::vector<uint8_t>& buf, size_t& offset,
                              uint32_t& width, uint32_t& height, uint32_t& channels)
{
	if (buf.size() >= kTextureHeaderLegacyMinSize)
	{
		uint64_t w = 0, h = 0, c = 0;
		if (!Reader::readPOD(buf, offset, w) || !Reader::readPOD(buf, offset, h)
		    || !Reader::readPOD(buf, offset, c)) return false;
		width    = static_cast<uint32_t>(w);
		height   = static_cast<uint32_t>(h);
		channels = static_cast<uint32_t>(c);
		return true;
	}
	return Reader::readPOD(buf, offset, width) && Reader::readPOD(buf, offset, height)
	    && Reader::readPOD(buf, offset, channels);
}

// Writes the current (uint32) layout — never the legacy one, so a re-save of an
// old asset also migrates its TXMI chunk.
inline void appendTextureHeader(std::vector<uint8_t>& buf,
                                uint32_t width, uint32_t height, uint32_t channels)
{
	Writer::appendPOD(buf, width);
	Writer::appendPOD(buf, height);
	Writer::appendPOD(buf, channels);
}

} // namespace HAsset
