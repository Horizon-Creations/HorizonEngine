#include "ContentManager/AssetRefScan.h"
#include "ContentManager/HAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace HE::AssetRefs
{
namespace
{

// ── What the scan reads ──────────────────────────────────────────────────────
// Every chunk whose entire payload is one JSON document. Deliberately a SUPERSET
// of AssetRefRetarget's isJsonChunk(): that list is missing the three type-registry
// chunks (documented as JSON in HAsset.h), and a JSON payload treated as binary is
// structurally invisible — a quoted value has no length prefix in front of it, and
// a document starting at offset 0 cannot be found by scanning backwards for one.
bool isJsonPayloadChunk(uint32_t id)
{
	return id == HAsset::CHUNK_MGRF || id == HAsset::CHUNK_UIWT || id == HAsset::CHUNK_UIWG
	    || id == HAsset::CHUNK_HCGR || id == HAsset::CHUNK_IACT || id == HAsset::CHUNK_IMAP
	    || id == HAsset::CHUNK_PTGR || id == HAsset::CHUNK_ASMG
	    || id == HAsset::CHUNK_STDF || id == HAsset::CHUNK_ENDF || id == HAsset::CHUNK_SGTP;
}

// Chunks holding plain UTF-8 source with no length prefix (script bodies, shader
// text, a HorizonCode base-class name). The binary walk cannot see a path in
// there — nothing precedes it that decodes as a length — so they are matched as
// TEXT. That is honest for a report even though the rename path cannot rewrite
// them: a Lua script naming an asset really does break when the asset goes.
bool isTextPayloadChunk(uint32_t id)
{
	return id == HAsset::CHUNK_SRC || id == HAsset::CHUNK_HCBC;
}

uint32_t readU32(const uint8_t* p) { uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); return v; }

// A path is stored complete, so a hit that is part of a longer path-ish token is
// not a reference. Used only for the free-text forms (script sources, loose
// .lua/.py/.h/.cpp), where there is no length prefix to confirm the boundary.
bool isPathChar(unsigned char c)
{
	return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\';
}

// ── The compiled query ───────────────────────────────────────────────────────
struct Query
{
	// Whole-value matches ("Tex/Rock.hasset"), in both the content-relative and
	// the project-relative spelling.
	std::vector<std::string> exact;
	// Folder targets. A reference matches only when it points at something INSIDE
	// the folder ("Tex/Rock.hasset" for "Tex"), never at the bare folder name:
	// nothing stores a reference TO a folder, while plenty of unrelated strings —
	// an entity called "Player", a variable named after the folder — are equal to
	// it, and every one of those would be reported as a referrer of the whole
	// subtree. Stored WITHOUT the trailing '/', which the matchers add.
	std::vector<std::string> prefixes;
	std::vector<HE::UUID>    uuids;

	// The cheap gate. Path needles are searched as plain substrings; uuids are
	// matched by the DECIMAL TEXT of their `hi` half, which is how both JSON
	// encodings spell them ([hi,lo] arrays and {"hi":…,"lo":…} objects).
	//
	// The uuid half is a SET, not more needles: a folder delete has one target per
	// contained asset, and searching a thousand separate needles across every file
	// in the project is a thousand passes over the same bytes. One scan for a run
	// of digits plus a hash lookup is one pass regardless of how many uuids the
	// query holds.
	std::vector<std::string>        textNeedles;
	std::unordered_set<std::string> uuidDigits;
	std::size_t                     longestNeedle = 0;

	bool empty() const { return textNeedles.empty() && uuidDigits.empty(); }
};

// Does one stored value spell a reference to a target?
bool valueMatches(const std::string& value, const Query& q)
{
	for (const std::string& p : q.exact)
		if (value == p) return true;
	for (const std::string& p : q.prefixes)
		if (value.size() > p.size() + 1 &&
		    value.compare(0, p.size(), p) == 0 && value[p.size()] == '/') return true;
	return false;
}

bool matchesUuid(const Query& q, uint64_t hi, uint64_t lo)
{
	for (const HE::UUID& id : q.uuids)
		if (id.hi == hi && id.lo == lo) return true;
	return false;
}

Query compile(const ScanTargets& targets, const std::string& contentDirName)
{
	Query q;
	auto addExact = [&](const std::string& p)
	{
		if (p.empty()) return;
		q.exact.push_back(p);
		if (!contentDirName.empty()) q.exact.push_back(contentDirName + "/" + p);
	};
	for (const std::string& p : targets.paths) addExact(p);
	for (const std::string& p : targets.pathPrefixes)
	{
		if (p.empty()) continue;
		q.prefixes.push_back(p);
		if (!contentDirName.empty()) q.prefixes.push_back(contentDirName + "/" + p);
	}
	for (const HE::UUID& id : targets.uuids)
	{
		// An unset reference serialises as [0,0]; a null target would match every
		// component that simply has nothing assigned.
		if (id == HE::UUID{}) continue;
		q.uuids.push_back(id);
		q.uuidDigits.insert(std::to_string(id.hi));
	}

	std::unordered_set<std::string> seen;
	for (const std::string& p : q.exact)    if (seen.insert(p).second) q.textNeedles.push_back(p);
	for (const std::string& p : q.prefixes) if (seen.insert(p).second) q.textNeedles.push_back(p);
	for (const std::string& n : q.textNeedles) q.longestNeedle = (std::max)(q.longestNeedle, n.size());
	for (const std::string& d : q.uuidDigits)  q.longestNeedle = (std::max)(q.longestNeedle, d.size());
	return q;
}

// Is this digit run one of the query's uuid halves? A `hi` is 19-20 digits and
// carries the RFC-4122 version nibble, so a run that is part of a longer number
// (a float's mantissa, an index buffer rendered as text) is not one — the run is
// therefore taken maximally and compared whole.
bool digitRunMatches(std::string_view hay, std::size_t& i, const Query& q)
{
	const std::size_t start = i;
	while (i < hay.size() && hay[i] >= '0' && hay[i] <= '9') ++i;
	const std::size_t len = i - start;
	if (len == 0) return false;
	return q.uuidDigits.count(std::string(hay.substr(start, len))) > 0;
}

bool blockMentions(std::string_view view, const Query& q)
{
	for (const std::string& n : q.textNeedles)
		if (!n.empty() && view.find(n) != std::string_view::npos) return true;
	if (q.uuidDigits.empty()) return false;
	for (std::size_t i = 0; i < view.size(); )
	{
		if (view[i] < '0' || view[i] > '9') { ++i; continue; }
		if (digitRunMatches(view, i, q)) return true;
	}
	return false;
}

// ── The cheap gate ───────────────────────────────────────────────────────────
// A file whose bytes never contain any needle cannot hold any of the references,
// so it is never read whole or parsed. Streamed in overlapping blocks so a
// multi-GB cooked asset is never resident just to be ruled out.
//
// Tri-state on purpose. Collapsing "could not read" into "does not mention it"
// is exactly the failure a delete dialog must not have: an unreadable scene is
// then indistinguishable from an unrelated one, and the dialog would state that
// nothing references an asset it never managed to look at.
enum class Gate { Miss, Hit, Unreadable };

Gate gateFile(const fs::path& file, const Query& q)
{
	if (q.longestNeedle == 0) return Gate::Miss;

	std::ifstream f(file, std::ios::binary);
	if (!f) return Gate::Unreadable;

	constexpr std::size_t kBlock = 256 * 1024;
	const std::size_t overlap = q.longestNeedle;   // a needle split across two blocks
	std::string buf(kBlock + overlap, '\0');
	std::size_t carry = 0;
	while (f)
	{
		f.read(buf.data() + carry, static_cast<std::streamsize>(kBlock));
		const std::size_t got = carry + static_cast<std::size_t>(f.gcount());
		if (got == 0) break;
		if (blockMentions(std::string_view(buf.data(), got), q)) return Gate::Hit;
		carry = (std::min)(got, overlap);
		std::memmove(buf.data(), buf.data() + got - carry, carry);
	}
	// badbit is a real I/O error; eofbit alone is the normal end. Without this
	// check a half-read file answers "no reference" from the part it did read.
	if (f.bad()) return Gate::Unreadable;
	return Gate::Miss;
}

// ── JSON walking ─────────────────────────────────────────────────────────────
// Two shapes carry an asset id in JSON, and they are not interchangeable:
//   [hi, lo]                  — scene components (SceneSerializer::uuidToJson)
//   {"hi": …, "lo": …}        — graph chunks (HE::graph::uuidToJson): ASMG clip
//                               ids, PTGR mesh/material ids
// Both are matched. `insideComponents` guards the scene case: at ENTITY level the
// very same [hi,lo] shape spells the entity's own identity and its parent/children
// links, which are not asset references at all.
bool jsonHoldsRef(const json& node, const Query& q, bool insideComponents, RefKind& kindOut)
{
	if (node.is_string())
	{
		if (valueMatches(node.get_ref<const std::string&>(), q)) { kindOut = RefKind::Path; return true; }
		return false;
	}
	if (node.is_object())
	{
		if (!q.uuids.empty())
		{
			const auto hiIt = node.find("hi"), loIt = node.find("lo");
			if (hiIt != node.end() && loIt != node.end() &&
			    hiIt->is_number_unsigned() && loIt->is_number_unsigned() &&
			    matchesUuid(q, hiIt->get<uint64_t>(), loIt->get<uint64_t>()))
			{ kindOut = RefKind::Uuid; return true; }
		}
		for (auto it = node.begin(); it != node.end(); ++it)
		{
			// "components" is the only place a scene's [hi,lo] pairs are assets.
			// Everything above it (uuid/parent/children) addresses ENTITIES.
			const bool comps = insideComponents || it.key() == "components";
			if (jsonHoldsRef(it.value(), q, comps, kindOut)) return true;
		}
		return false;
	}
	if (node.is_array())
	{
		if (insideComponents && !q.uuids.empty() && node.size() == 2 &&
		    node[0].is_number_unsigned() && node[1].is_number_unsigned() &&
		    matchesUuid(q, node[0].get<uint64_t>(), node[1].get<uint64_t>()))
		{ kindOut = RefKind::Uuid; return true; }
		for (const json& child : node)
			if (jsonHoldsRef(child, q, insideComponents, kindOut)) return true;
		return false;
	}
	return false;
}

// `parsed` reports whether the text was JSON at all — the callers treat "could
// not be parsed" as "could not be checked", never as "holds no reference".
bool jsonTextHoldsRef(std::string_view text, const Query& q, bool insideComponents,
                      RefKind& kindOut, bool& parsed)
{
	const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
	parsed = !doc.is_discarded();
	if (!parsed) return false;
	return jsonHoldsRef(doc, q, insideComponents, kindOut);
}

bool textHoldsRef(std::string_view hay, const Query& q)
{
	auto tokenAt = [&](std::size_t p, const std::string& needle, bool asPrefix) -> bool
	{
		if (p > 0 && isPathChar(static_cast<unsigned char>(hay[p - 1]))) return false;
		const std::size_t end = p + needle.size();
		if (asPrefix)
			return end < hay.size() && hay[end] == '/';   // only something INSIDE the folder
		return end >= hay.size() || !isPathChar(static_cast<unsigned char>(hay[end]));
	};
	auto search = [&](const std::vector<std::string>& needles, bool asPrefix)
	{
		for (const std::string& n : needles)
		{
			if (n.empty()) continue;
			for (std::size_t p = hay.find(n); p != std::string_view::npos; p = hay.find(n, p + 1))
				if (tokenAt(p, n, asPrefix)) return true;
		}
		return false;
	};
	return search(q.exact, false) || search(q.prefixes, true);
}

// ── Binary chunk walking ─────────────────────────────────────────────────────
// The document a hit sits inside, when that hit landed in a JSON blob stored as
// one length-prefixed string (a material's node graph inside MTRL). Identified
// structurally — a '{' whose length prefix lands exactly on the matching final
// '}' — which no float or index buffer imitates. The backwards search is capped:
// in a region with no '{' at all (a zeroed or pure-text buffer) it would
// otherwise walk back over the whole chunk once per hit.
bool enclosingJsonAt(const std::vector<uint8_t>& data, std::size_t at,
                     std::size_t& begin, uint32_t& len)
{
	constexpr std::size_t kMaxLookback = 1u * 1024u * 1024u;
	const std::size_t floorPos = (at > kMaxLookback) ? at - kMaxLookback : sizeof(uint32_t);
	for (std::size_t q = at; q-- > floorPos; )
	{
		if (data[q] != '{') continue;
		if (q < sizeof(uint32_t)) break;
		const uint32_t n = readU32(data.data() + q - sizeof(uint32_t));
		if (n < 2 || n > data.size() - q) continue;
		if (q + n <= at) break;                 // this document ends before the hit
		if (data[q + n - 1] != '}') continue;
		begin = q;
		len   = n;
		return true;
	}
	return false;
}

std::size_t nextHit(std::string_view hay, std::size_t start, const Query& q)
{
	std::size_t best = std::string_view::npos;
	for (const std::string& n : q.textNeedles)
	{
		if (n.empty()) continue;
		const std::size_t p = hay.find(n, start);
		if (p < best) best = p;
	}
	if (!q.uuidDigits.empty())
	{
		for (std::size_t i = start; i < hay.size() && i < best; )
		{
			if (hay[i] < '0' || hay[i] > '9') { ++i; continue; }
			const std::size_t runStart = i;
			if (digitRunMatches(hay, i, q)) { best = (std::min)(best, runStart); break; }
			if (i == runStart) ++i;   // digitRunMatches always advances, but never trust that silently
		}
	}
	return best;
}

bool binaryChunkHoldsRef(const std::vector<uint8_t>& data, const Query& q, RefKind& kindOut)
{
	if (q.empty()) return false;
	const std::string_view hay(reinterpret_cast<const char*>(data.data()), data.size());

	// Driven by occurrences, not by the chunk size: a mesh's vertex buffer that
	// mentions nothing costs one substring search and is done.
	std::size_t from = 0;
	for (std::size_t hit = nextHit(hay, 0, q); hit != std::string_view::npos;
	     hit = nextHit(hay, from, q))
	{
		from = hit + 1;

		// A stored path is a length-prefixed string: the uint32 in the four bytes
		// before it IS its length. Matching that (rather than the raw text) keeps
		// "Content/Rock" out of "Content/Rock2.hasset" and out of the middle of a
		// buffer that happens to contain the characters.
		if (hit >= sizeof(uint32_t))
		{
			const uint32_t len = readU32(data.data() + hit - sizeof(uint32_t));
			if (len > 0 && len <= data.size() - hit)
			{
				const std::string value(reinterpret_cast<const char*>(data.data() + hit), len);
				if (valueMatches(value, q)) { kindOut = RefKind::Path; return true; }
			}
		}

		std::size_t jsonBegin = 0;
		uint32_t    jsonLen   = 0;
		if (enclosingJsonAt(data, hit, jsonBegin, jsonLen))
		{
			const std::string_view doc(reinterpret_cast<const char*>(data.data() + jsonBegin), jsonLen);
			bool parsed = false;
			if (jsonTextHoldsRef(doc, q, /*insideComponents=*/false, kindOut, parsed)) return true;
			from = jsonBegin + jsonLen;   // never re-enter a document already examined
		}
	}
	return false;
}

// ── Per-file scanners ────────────────────────────────────────────────────────
// A truncated read must never look like a complete one: the caller turns "could
// not read" into ScanResult::incomplete, which is what stops the dialog from
// claiming nothing references the asset.
bool readWhole(const fs::path& p, std::vector<uint8_t>& out)
{
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return !f.bad();
}

bool hassetHoldsRef(const std::vector<uint8_t>& blob, const Query& q,
                    RefKind& kindOut, bool& unreadable)
{
	HAsset::Reader r;
	if (!r.openData(blob))
	{
		// Not a readable .hasset — a truncated file, or one being written right
		// now. It mentioned the target (it passed the gate), so silence here would
		// be a false "nothing references it".
		unreadable = true;
		return false;
	}

	for (const auto& c : r.chunks())
	{
		// META is the asset's OWN name and path. Without this skip every asset
		// reports itself, and a folder query reports every asset inside it.
		if (c.id == HAsset::CHUNK_META) continue;

		const std::string_view text(reinterpret_cast<const char*>(c.data.data()), c.data.size());
		if (isJsonPayloadChunk(c.id))
		{
			bool parsed = false;
			if (jsonTextHoldsRef(text, q, /*insideComponents=*/false, kindOut, parsed)) return true;
			if (!parsed) unreadable = true;
			continue;
		}
		if (isTextPayloadChunk(c.id))
		{
			if (textHoldsRef(text, q)) { kindOut = RefKind::Path; return true; }
			continue;
		}
		if (binaryChunkHoldsRef(c.data, q, kindOut)) return true;
	}
	return false;
}

// A .hescene is JSON written by the editor — EXCEPT right after Create ▸ Scene,
// which writes an HAsset stub at the same path that holds nothing but its own
// META. Sniffing the magic keeps the JSON parser from choking on it.
bool sceneHoldsRef(const std::vector<uint8_t>& blob, const Query& q,
                   RefKind& kindOut, bool& unreadable)
{
	if (blob.size() >= 4 && std::memcmp(blob.data(), HAsset::k_magic, 4) == 0)
		return hassetHoldsRef(blob, q, kindOut, unreadable);
	const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
	bool parsed = false;
	if (jsonTextHoldsRef(text, q, /*insideComponents=*/false, kindOut, parsed)) return true;
	if (parsed) return false;
	// Scenes are the single largest source of references, so an unparseable one
	// (mid-save, hand-edited) gets a text answer AND marks the result incomplete
	// rather than quietly counting as "does not reference it".
	unreadable = true;
	return textHoldsRef(text, q) ? (kindOut = RefKind::Path, true) : false;
}

bool jsonFileHoldsRef(const std::vector<uint8_t>& blob, const Query& q,
                      RefKind& kindOut, bool& unreadable)
{
	const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
	bool parsed = false;
	if (jsonTextHoldsRef(text, q, /*insideComponents=*/false, kindOut, parsed)) return true;
	if (parsed) return false;
	unreadable = true;
	return textHoldsRef(text, q) ? (kindOut = RefKind::Path, true) : false;
}

bool textFileHoldsRef(const std::vector<uint8_t>& blob, const Query& q, RefKind& kindOut)
{
	const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
	if (textHoldsRef(text, q)) { kindOut = RefKind::Path; return true; }
	return false;
}

std::string lowerExt(const fs::path& p)
{
	std::string e = p.extension().string();
	for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}

bool isUnder(const std::string& path, const std::string& dir)
{
	if (dir.empty() || path.size() <= dir.size()) return false;
	if (path.compare(0, dir.size(), dir) != 0)    return false;
	const char sep = path[dir.size()];
	return sep == '/' || sep == '\\';
}

} // namespace

HE::UUID assetUuidOfFile(const std::string& absolutePath)
{
	// Streamed rather than HAsset::Reader::open'd: this is called for every asset
	// under a folder about to be deleted, and Reader::open reads EVERY chunk
	// payload into memory — gigabytes of vertex and pixel data to reach 16 bytes
	// of header. Same shape as ContentManager::scanDirInto, which streams past
	// each payload for exactly this reason.
	std::ifstream f(absolutePath, std::ios::binary);
	if (!f.is_open()) return HE::UUID{};

	HAsset::FileHeader hdr{};
	f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!f || std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0) return HE::UUID{};
	// Pre-v2 files persist no id at all — reported as null, which callers read as
	// "no UUID to look for", never as "matches everything".
	if (hdr.version < 2) return HE::UUID{};

	for (uint32_t i = 0; i < hdr.chunk_count; ++i)
	{
		HAsset::ChunkHeader ch{};
		f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
		if (!f) return HE::UUID{};
		if (ch.id != HAsset::CHUNK_META)
		{
			f.seekg(static_cast<std::streamoff>(ch.size), std::ios::cur);
			if (!f) return HE::UUID{};
			continue;
		}
		// Field order mirrors ContentManager's buildMetaChunk: type, hi, lo, name, path.
		if (ch.size < sizeof(uint16_t) + 2 * sizeof(uint64_t)) return HE::UUID{};
		std::vector<uint8_t> meta(ch.size);
		f.read(reinterpret_cast<char*>(meta.data()), static_cast<std::streamsize>(ch.size));
		if (!f) return HE::UUID{};
		std::size_t off = sizeof(uint16_t);
		HE::UUID id;
		if (!HAsset::Reader::readPOD(meta, off, id.hi)) return HE::UUID{};
		if (!HAsset::Reader::readPOD(meta, off, id.lo)) return HE::UUID{};
		return id;
	}
	return HE::UUID{};
}

ScanResult findReferrers(const ScanTargets& targets, const ScanRequest& request)
{
	ScanResult result;
	if (targets.empty() || request.contentRoot.empty()) return result;

	const Query q = compile(targets, request.contentDirName);
	if (q.empty()) return result;

	const std::unordered_set<std::string> excludeFiles(request.excludeFiles.begin(),
	                                                    request.excludeFiles.end());

	auto cancelled = [&]{ return request.isCancelled && request.isCancelled(); };

	auto isExcluded = [&](const std::string& abs)
	{
		if (excludeFiles.count(abs)) return true;
		for (const std::string& dir : request.excludeUnder)
			if (isUnder(abs, dir) || abs == dir) return true;
		return false;
	};

	auto contentRelative = [&](const fs::path& p) -> std::string
	{
		std::error_code ec;
		const fs::path rel = fs::relative(p, request.contentRoot, ec);
		if (!ec && !rel.empty() && rel.native()[0] != '.') return rel.generic_string();
		if (!request.projectRoot.empty())
		{
			ec.clear();
			const fs::path pr = fs::relative(p, request.projectRoot, ec);
			if (!ec && !pr.empty() && pr.native()[0] != '.') return pr.generic_string();
		}
		return p.filename().string();
	};

	auto examine = [&](const fs::path& p) -> bool   // false = stop the walk
	{
		const std::string abs = p.string();
		if (isExcluded(abs)) return true;

		const std::string ext = lowerExt(p);
		const bool isHasset = ext == ".hasset";
		const bool isScene  = ext == ".hescene";
		const bool isJsonF  = ext == ".heproj" || ext == ".hcode";
		const bool isTextF  = ext == ".lua" || ext == ".py" ||
		                      ext == ".h"   || ext == ".hpp" || ext == ".hh" ||
		                      ext == ".cpp" || ext == ".cc"  || ext == ".cxx" || ext == ".c";
		if (!isHasset && !isScene && !isJsonF && !isTextF) return true;

		const Gate gate = gateFile(p, q);
		if (gate == Gate::Unreadable) { result.incomplete = true; return true; }
		++result.filesScanned;
		if (gate == Gate::Miss) return true;

		std::vector<uint8_t> blob;
		if (!readWhole(p, blob) || blob.empty())
		{
			result.incomplete = true;   // it mentioned the target but could not be read
			return true;
		}

		RefKind kind       = RefKind::Path;
		bool    unreadable = false;
		bool    hit        = false;
		if (isHasset)      hit = hassetHoldsRef(blob, q, kind, unreadable);
		else if (isScene)  hit = sceneHoldsRef(blob, q, kind, unreadable);
		else if (isJsonF)  hit = jsonFileHoldsRef(blob, q, kind, unreadable);
		else               hit = textFileHoldsRef(blob, q, kind);
		if (unreadable) result.incomplete = true;
		if (!hit) return true;

		result.referrers.push_back(Referrer{ abs, contentRelative(p), kind });
		if (result.referrers.size() >= request.maxReferrers)
		{
			result.truncated = true;
			return false;
		}
		return true;
	};

	// Walks one tree, honouring cancellation and reporting an abandoned walk.
	auto walk = [&](const fs::path& root) -> bool   // false = stop everything
	{
		std::error_code ec;
		if (!fs::is_directory(root, ec)) return true;
		fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
		const fs::recursive_directory_iterator end;
		for (; !ec && it != end; it.increment(ec))
		{
			if (cancelled()) { result.incomplete = true; return false; }
			std::error_code fec;
			if (!it->is_regular_file(fec) || fec) { fec.clear(); continue; }
			if (!examine(it->path())) return false;
		}
		// An abandoned walk is a LOWER bound, and the dialog says so rather than
		// claiming nothing references the asset.
		if (ec) result.incomplete = true;
		return true;
	};

	{
		std::error_code ec;
		if (!fs::is_directory(request.contentRoot, ec)) result.incomplete = true;
	}
	if (!walk(request.contentRoot)) return result;

	// The project manifest names the startup scene, a project-root
	// GameInstance.hcode is a HorizonCode graph nothing else walks, and a C++
	// project's Source/ tree holds level scripts that name assets by path — all
	// three sit BESIDE Content/, so the tree walk above never sees them.
	if (!request.projectRoot.empty())
	{
		std::error_code pec;
		fs::directory_iterator pit(request.projectRoot, fs::directory_options::skip_permission_denied, pec);
		const fs::directory_iterator pend;
		for (; !pec && pit != pend; pit.increment(pec))
		{
			if (cancelled()) { result.incomplete = true; return result; }
			std::error_code fec;
			if (!pit->is_regular_file(fec) || fec) { fec.clear(); continue; }
			const std::string ext = lowerExt(pit->path());
			if (ext != ".heproj" && ext != ".hcode") continue;
			if (!examine(pit->path())) return result;
		}
		if (pec) result.incomplete = true;
		if (!walk(fs::path(request.projectRoot) / "Source")) return result;
	}

	return result;
}

} // namespace HE::AssetRefs
