#include "ContentManager/AssetRefRetarget.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace HE::AssetRefs
{

bool retargetValue(std::string& value, const std::vector<Rule>& rules)
{
	for (const Rule& r : rules)
	{
		if (r.from.empty()) continue;
		if (value == r.from) { value = r.to; return true; }
		if (r.prefix && value.size() > r.from.size() &&
		    value.compare(0, r.from.size(), r.from) == 0 && value[r.from.size()] == '/')
		{ value = r.to + value.substr(r.from.size()); return true; }
	}
	return false;
}

namespace
{

// Chunks whose ENTIRE payload is one JSON document (see ContentManager::saveAsset).
// Everything else is treated as a binary chunk that may contain length-prefixed
// strings — including MTRL, which holds plain path fields AND an embedded node
// graph JSON, so it needs both shapes.
bool isJsonChunk(uint32_t id)
{
	return id == HAsset::CHUNK_MGRF || id == HAsset::CHUNK_UIWT || id == HAsset::CHUNK_UIWG
	    || id == HAsset::CHUNK_HCGR || id == HAsset::CHUNK_IACT || id == HAsset::CHUNK_IMAP
	    || id == HAsset::CHUNK_PTGR || id == HAsset::CHUNK_ASMG;
}

uint32_t readU32(const uint8_t* p) { uint32_t v = 0; std::memcpy(&v, p, sizeof(v)); return v; }
void     appendU32(std::vector<uint8_t>& out, uint32_t v)
{ const auto* p = reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(), p, p + sizeof(v)); }

// True when `data[at]` starts a length-prefixed string (uint32 length in the four
// bytes before it) whose whole value a rule rewrites. On success `out` holds the
// new value and `len` the old one's length.
bool prefixedStringAt(const std::vector<uint8_t>& data, size_t at,
                      const std::vector<Rule>& rules, uint32_t& len, std::string& out)
{
	if (at < sizeof(uint32_t)) return false;
	len = readU32(data.data() + at - sizeof(uint32_t));
	if (len == 0 || len > data.size() - at) return false;
	out.assign(reinterpret_cast<const char*>(data.data() + at), len);
	return retargetValue(out, rules);
}

// True when the occurrence at `at` sits inside a JSON document that is itself
// stored as a length-prefixed string (a material's node graph inside MTRL). The
// document is identified structurally — it starts at a '{' whose length prefix
// lands exactly on the matching final '}' — which no float or index buffer
// imitates. On success [begin, begin+len) is that document.
bool enclosingJsonAt(const std::vector<uint8_t>& data, size_t at,
                     size_t& begin, uint32_t& len)
{
	for (size_t q = at; q-- > sizeof(uint32_t); )
	{
		if (data[q] != '{') continue;
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

// Does the file contain any of the rules' paths anywhere? Streams in blocks that
// overlap by the longest needle, so an asset whose payload is hundreds of
// megabytes of pixels is never held in memory just to rule it out.
bool fileMentions(const std::filesystem::path& file, const std::vector<Rule>& rules)
{
	size_t longest = 0;
	for (const Rule& r : rules) longest = std::max(longest, r.from.size());
	if (longest == 0) return false;

	std::ifstream f(file, std::ios::binary);
	if (!f) return false;

	constexpr size_t kBlock = 256 * 1024;
	std::string buf(kBlock + longest, '\0');
	size_t carry = 0; // bytes kept from the previous block (a split occurrence)
	while (f)
	{
		f.read(buf.data() + carry, static_cast<std::streamsize>(kBlock));
		const size_t got = carry + static_cast<size_t>(f.gcount());
		if (got == 0) break;
		const std::string_view view(buf.data(), got);
		for (const Rule& r : rules)
			if (!r.from.empty() && view.find(r.from) != std::string_view::npos) return true;
		carry = std::min(got, longest - 1);
		std::memmove(buf.data(), buf.data() + got - carry, carry);
	}
	return false;
}

// Offset of the next occurrence of any rule's `from` at or after `start`.
size_t nextHit(const std::vector<uint8_t>& data, size_t start, const std::vector<Rule>& rules)
{
	const std::string_view hay(reinterpret_cast<const char*>(data.data()), data.size());
	size_t best = std::string_view::npos;
	for (const Rule& r : rules)
	{
		if (r.from.empty()) continue;
		const size_t p = hay.find(r.from, start);
		if (p < best) best = p;
	}
	return best;
}

// Binary chunk: rewrite the path strings in it, leaving every other byte alone.
// One forward pass — each hit is either a length-prefixed path (rewritten with
// its prefix) or a path inside an embedded JSON document (the whole document is
// rewritten and re-prefixed), and anything else is copied through.
bool retargetBinaryChunk(std::vector<uint8_t>& data, const std::vector<Rule>& rules)
{
	std::vector<uint8_t> out;
	bool   changed = false;
	size_t copied  = 0; // everything before this is already in `out`
	for (size_t hit = nextHit(data, 0, rules); hit != std::string_view::npos;
	     hit = nextHit(data, copied, rules))
	{
		uint32_t    len = 0;
		std::string value;
		if (prefixedStringAt(data, hit, rules, len, value))
		{
			out.insert(out.end(), data.begin() + copied, data.begin() + (hit - sizeof(uint32_t)));
			appendU32(out, static_cast<uint32_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
			copied  = hit + len;
			changed = true;
			continue;
		}

		size_t   jsonBegin = 0;
		uint32_t jsonLen   = 0;
		if (enclosingJsonAt(data, hit, jsonBegin, jsonLen) && jsonBegin >= copied + sizeof(uint32_t))
		{
			std::string doc(reinterpret_cast<const char*>(data.data() + jsonBegin), jsonLen);
			if (retargetJsonText(doc, rules))
			{
				out.insert(out.end(), data.begin() + copied,
				           data.begin() + (jsonBegin - sizeof(uint32_t)));
				appendU32(out, static_cast<uint32_t>(doc.size()));
				out.insert(out.end(), doc.begin(), doc.end());
				copied  = jsonBegin + jsonLen;
				changed = true;
				continue;
			}
		}

		// A mention that is not a stored reference (part of a longer path, some
		// unrelated text) — copy up to and past it and keep looking.
		out.insert(out.end(), data.begin() + copied, data.begin() + hit + 1);
		copied = hit + 1;
	}
	if (!changed) return false;
	out.insert(out.end(), data.begin() + copied, data.end());
	data = std::move(out);
	return true;
}

} // namespace

std::vector<Rule> moveRules(const std::string& oldRel, const std::string& newRel,
                            bool folder, const std::string& contentDirName)
{
	std::vector<Rule> rules;
	if (oldRel.empty() || newRel.empty() || oldRel == newRel) return rules;
	rules.push_back({ oldRel, newRel, folder });
	// Scenes are referenced project-relative ("Content/Level.hescene" — what
	// scene.load and the .heproj startup entry store), so that form needs its own
	// rule; it is not a prefix of the content-relative one.
	if (!contentDirName.empty())
		rules.push_back({ contentDirName + "/" + oldRel, contentDirName + "/" + newRel, folder });
	return rules;
}

bool retargetJsonText(std::string& text, const std::vector<Rule>& rules)
{
	std::string out;
	out.reserve(text.size());
	bool   changed = false;
	size_t i       = 0;
	while (i < text.size())
	{
		if (text[i] != '"') { out += text[i++]; continue; }
		// Collect the string body verbatim (escapes included, so an escaped quote
		// doesn't end it early — a path never contains one, so a value that does
		// simply won't match any rule).
		std::string raw;
		size_t      j      = i + 1;
		bool        closed = false;
		while (j < text.size())
		{
			if (text[j] == '\\' && j + 1 < text.size()) { raw += text[j]; raw += text[j + 1]; j += 2; continue; }
			if (text[j] == '"') { closed = true; break; }
			raw += text[j++];
		}
		if (!closed) { out.append(text, i, std::string::npos); i = text.size(); break; }
		if (retargetValue(raw, rules)) changed = true;
		out += '"';
		out += raw;
		out += '"';
		i = j + 1;
	}
	if (!changed) return false;
	text = std::move(out);
	return true;
}

bool retargetBlob(std::vector<uint8_t>& blob, const std::vector<Rule>& rules)
{
	HAsset::Reader reader;
	if (!reader.openData(blob)) return false;

	HAsset::Writer writer;
	bool changed = false;
	for (const auto& chunk : reader.chunks())
	{
		std::vector<uint8_t> data = chunk.data;
		if (isJsonChunk(chunk.id))
		{
			std::string text(reinterpret_cast<const char*>(data.data()), data.size());
			if (retargetJsonText(text, rules))
			{
				data.assign(text.begin(), text.end());
				changed = true;
			}
		}
		else if (retargetBinaryChunk(data, rules))
			changed = true;

		writer.addChunk(chunk.id, data.data(), data.size());
	}
	if (!changed) return false;
	blob = writer.toBytes(reader.assetType());
	return true;
}

size_t retargetTree(const std::string& root, const std::vector<Rule>& rules)
{
	namespace fs = std::filesystem;
	if (root.empty() || rules.empty()) return 0;
	std::error_code ec;
	if (!fs::is_directory(root, ec)) return 0;

	size_t rewritten = 0;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	const fs::recursive_directory_iterator end;
	// increment(ec) rather than the range-for: its operator++ THROWS on an
	// unreadable subdirectory, and this runs inside an editor frame.
	for (; !ec && it != end; it.increment(ec))
	{
		std::error_code fec;
		if (!it->is_regular_file(fec) || fec) { fec.clear(); continue; }
		if (it->path().extension() != ".hasset") continue;

		// Cheap gate: an asset that doesn't contain the path text anywhere cannot
		// reference it, so it is never read whole, parsed or rewritten.
		if (!fileMentions(it->path(), rules)) continue;

		std::vector<uint8_t> blob;
		{
			std::ifstream f(it->path(), std::ios::binary);
			if (!f) continue;
			blob.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		if (blob.empty() || !retargetBlob(blob, rules)) continue;

		// Temp + rename, the same way HAsset::Writer::write commits an asset: one
		// rename touches EVERY referring asset in the tree, and truncating each in
		// place made every one of them a window in which a crash leaves a
		// half-written file whose META — the UUID scenes point at — is gone.
		const fs::path tmpPath = it->path().string() + ".tmp";
		{
			std::ofstream o(tmpPath, std::ios::binary | std::ios::trunc);
			if (!o) continue;
			o.write(reinterpret_cast<const char*>(blob.data()),
			        static_cast<std::streamsize>(blob.size()));
			// Closed and checked here rather than after the scope: the destructor
			// flushes, so `if (!o)` before it would pass for a write the disk has
			// yet to reject.
			o.close();
			if (!o)
			{
				std::error_code rec;
				fs::remove(tmpPath, rec);
				continue;
			}
		}
		std::error_code rnec;
		fs::rename(tmpPath, it->path(), rnec);
		if (rnec)
		{
			std::error_code rec;
			fs::remove(tmpPath, rec);
			continue;
		}
		++rewritten;
		HE_LOG_INFO(Asset, "%s",
			("AssetRefs: retargeted references in " + it->path().filename().string()).c_str());
	}
	return rewritten;
}

} // namespace HE::AssetRefs
