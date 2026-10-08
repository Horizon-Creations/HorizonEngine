#include "HorizonScene/SceneJsonParse.h"
#include <JobSystem/JobSystem.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <string_view>
#include <vector>

using json = nlohmann::json;

namespace
{

struct Span { size_t begin = 0; size_t end = 0; };   // [begin, end) in the text

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Index just past the closing quote of the string that opens at `i`, or npos.
size_t skipString(const std::string& t, size_t i)
{
	for (size_t j = i + 1; j < t.size(); ++j)
	{
		if (t[j] == '\\') { ++j; continue; }
		if (t[j] == '"') return j + 1;
	}
	return std::string::npos;
}

// The top-level "entities" array and the span of each of its elements. False on
// any shape this does not expect; the caller then parses the whole text. Bracket
// kinds are not matched up here: a mismatch makes a span that does not parse,
// and that falls back too.
bool findEntities(const std::string& t, Span& array, std::vector<Span>& elements)
{
	size_t depth        = 0;
	bool   expectKey    = false;   // depth 1: the next string is a key
	bool   entitiesNext = false;   // depth 1: the value of "entities" comes next
	bool   found        = false;
	bool   inArray      = false;   // inside the entities array (depth 2 = between elements)
	size_t elemBegin    = 0;
	for (size_t i = 0; i < t.size(); ++i)
	{
		const char c = t[i];
		if (isSpace(c)) continue;
		if (c == '"')
		{
			const size_t j = skipString(t, i);
			if (j == std::string::npos) return false;
			if (depth == 1 && expectKey)
			{
				const std::string_view key(t.data() + i + 1, j - i - 2);
				// An escaped key could still spell "entities" once decoded.
				if (key.find('\\') != std::string_view::npos) return false;
				if (key == "entities")
				{
					if (found) return false;   // the key twice: let the real parser decide
					found        = true;
					entitiesNext = true;
				}
				expectKey = false;
			}
			else if ((depth == 1 && entitiesNext) || (inArray && depth == 2))
			{
				return false;   // "entities" is a string, or an element is one
			}
			i = j - 1;
			continue;
		}
		switch (c)
		{
		case '{':
		case '[':
			if (depth == 0 && c != '{') return false;
			if (depth == 1 && entitiesNext)
			{
				if (c != '[') return false;
				entitiesNext = false;
				inArray      = true;
				array.begin  = i;
			}
			else if (inArray && depth == 2)
			{
				if (c != '{') return false;   // entities are objects
				elemBegin = i;
			}
			++depth;
			if (depth == 1) expectKey = true;
			break;
		case '}':
		case ']':
			if (depth == 0) return false;
			--depth;
			if (inArray && depth == 2)
			{
				elements.push_back({ elemBegin, i + 1 });
			}
			else if (inArray && depth == 1)
			{
				inArray   = false;
				array.end = i + 1;
			}
			break;
		case ',':
			if (depth == 1) expectKey = true;
			break;
		case ':':
			break;
		default:
			// A number, true/false/null: fine anywhere but as the entities value
			// or one of its elements.
			if ((depth == 1 && entitiesNext) || (inArray && depth == 2)) return false;
			break;
		}
	}
	return found && depth == 0 && !inArray && array.end > array.begin;
}

json parseWhole(const std::string& text)
{
	return json::parse(text, nullptr, /*allow_exceptions=*/false);
}

json decodeWhole(const std::vector<uint8_t>& data)
{
	return json::from_cbor(data, /*strict=*/true, /*allow_exceptions=*/false);
}

// ── CBOR: element boundaries from the item heads ─────────────────────────────
// nlohmann's to_cbor writes definite lengths only, so every container says up
// front how many items follow and every string how many bytes. Walking the heads
// skips whole entities without decoding a single value.

constexpr size_t kNpos = ~size_t(0);

struct CborHead
{
	uint8_t  major = 0;
	uint64_t arg   = 0;   // the value, length or item count
	size_t   bytes = 0;   // size of the head itself
};

bool readHead(const std::vector<uint8_t>& d, size_t i, CborHead& h)
{
	if (i >= d.size()) return false;
	const uint8_t initial    = d[i];
	const uint8_t additional = initial & 0x1f;
	h.major = initial >> 5;
	size_t extra = 0;
	if (additional < 24)       { h.arg = additional; h.bytes = 1; return true; }
	else if (additional == 24) extra = 1;
	else if (additional == 25) extra = 2;
	else if (additional == 26) extra = 4;
	else if (additional == 27) extra = 8;
	else return false;   // reserved, or an indefinite length / break
	if (d.size() - i - 1 < extra) return false;
	uint64_t v = 0;
	for (size_t k = 0; k < extra; ++k) v = (v << 8) | d[i + 1 + k];
	h.arg   = v;
	h.bytes = 1 + extra;
	return true;
}

// Index just past the item that starts at `i`, or kNpos.
size_t skipItem(const std::vector<uint8_t>& d, size_t i, int depth)
{
	if (depth > 512) return kNpos;
	CborHead h;
	if (!readHead(d, i, h)) return kNpos;
	size_t j = i + h.bytes;
	switch (h.major)
	{
	case 0: case 1:   // integers
	case 7:           // simple values and floats: the head carries them
		return j;
	case 2: case 3:   // byte and text strings
		return h.arg > d.size() - j ? kNpos : j + static_cast<size_t>(h.arg);
	case 4: case 5:
	{
		uint64_t items = h.arg;
		if (h.major == 5)
		{
			if (items > (~uint64_t(0)) / 2) return kNpos;
			items *= 2;
		}
		if (items > d.size() - j) return kNpos;   // every item is at least one byte
		for (uint64_t k = 0; k < items && j != kNpos; ++k) j = skipItem(d, j, depth + 1);
		return j;
	}
	default:          // tags: from_cbor rejects them, let it say so
		return kNpos;
	}
}

// The CBOR twin of findEntities: the top-level map's "entities" array and the
// span of each element, false on any shape this does not expect.
bool findEntitiesCbor(const std::vector<uint8_t>& d, Span& array, std::vector<Span>& elements)
{
	CborHead top;
	if (!readHead(d, 0, top) || top.major != 5) return false;
	size_t j     = top.bytes;
	bool   found = false;
	for (uint64_t k = 0; k < top.arg; ++k)
	{
		CborHead key;
		if (!readHead(d, j, key)) return false;
		const size_t keyEnd = skipItem(d, j, 1);
		if (keyEnd == kNpos) return false;
		const bool isEntities = key.major == 3 && key.arg == 8 &&
		                        std::memcmp(d.data() + j + key.bytes, "entities", 8) == 0;
		j = keyEnd;
		if (!isEntities)
		{
			j = skipItem(d, j, 1);
			if (j == kNpos) return false;
			continue;
		}
		if (found) return false;   // the key twice: let the real decoder decide
		found = true;
		CborHead arr;
		if (!readHead(d, j, arr) || arr.major != 4) return false;
		array.begin = j;
		j += arr.bytes;
		if (arr.arg > d.size() - j) return false;
		elements.reserve(static_cast<size_t>(arr.arg));
		for (uint64_t e = 0; e < arr.arg; ++e)
		{
			CborHead el;
			if (!readHead(d, j, el) || el.major != 5) return false;   // entities are maps
			const size_t end = skipItem(d, j, 2);
			if (end == kNpos) return false;
			elements.push_back({ j, end });
			j = end;
		}
		array.end = j;
	}
	return found && j == d.size() && array.end > array.begin;
}

// ── The split itself, shared by text and CBOR ────────────────────────────────
// Parses the elements in pieces on the job pool and the skeleton (the scene with
// an empty entities array) here, then moves the elements into the skeleton.
// Returns a discarded json when the caller should take the whole parse instead.
template <class ParseElement, class ParseSkeleton>
json splitParse(const std::vector<Span>& elements, size_t arrayBytes, size_t minPieceBytes,
                const ParseElement& parseElement, const ParseSkeleton& parseSkeleton,
                size_t* piecesUsed)
{
	// Pieces of whole elements, cut by bytes (the environment entity alone can be
	// kilobytes), at least minPieceBytes each and a few per worker so a slow one
	// does not hold up the rest.
	ThreadPool& pool   = globalPool();
	size_t      pieces = std::min({ arrayBytes / minPieceBytes, (pool.threadCount() + 1) * 4,
	                                elements.size() });
	if (pieces < 2) return json(json::value_t::discarded);
	std::vector<size_t> first{ 0 };
	{
		const size_t target     = arrayBytes / pieces;
		size_t       pieceStart = elements[0].begin;
		for (size_t e = 1; e < elements.size() && first.size() < pieces; ++e)
			if (elements[e].begin - pieceStart >= target)
			{
				first.push_back(e);
				pieceStart = elements[e].begin;
			}
		first.push_back(elements.size());
	}
	const size_t n = first.size() - 1;

	// Each element parses straight out of the input, no copy; jobs only touch
	// their own slots of `parsed`.
	std::vector<json> parsed(elements.size());
	std::atomic<bool> bad{ false };
	auto parseRange = [&](size_t lo, size_t hi)
	{
		for (size_t e = lo; e < hi && !bad.load(std::memory_order_relaxed); ++e)
		{
			json v = parseElement(elements[e]);
			if (v.is_discarded() || !v.is_object())
			{
				bad.store(true, std::memory_order_relaxed);
				return;
			}
			parsed[e] = std::move(v);
		}
	};

	std::vector<HE::JobHandle> jobs;
	jobs.reserve(n - 1);
	for (size_t k = 1; k < n; ++k)
	{
		HE::JobDesc desc;
		desc.name     = "SceneParse";
		desc.priority = HE::JobPriority::High;   // the main thread waits on it
		const size_t lo = first[k], hi = first[k + 1];
		jobs.push_back(pool.schedule([&parseRange, lo, hi] { parseRange(lo, hi); }, std::move(desc)));
	}

	// Meanwhile here: everything around the array, then the first piece. Every
	// job refers to this frame's locals, so nothing may leave before all are done.
	json               scene;
	std::exception_ptr error;
	try
	{
		scene = parseSkeleton();
		parseRange(first[0], first[1]);
	}
	catch (...)
	{
		error = std::current_exception();
		bad.store(true, std::memory_order_relaxed);
	}
	for (const HE::JobHandle& job : jobs)
	{
		try { job.wait(); }
		catch (...) { if (!error) error = std::current_exception(); }
	}
	if (error) std::rethrow_exception(error);

	if (bad.load() || scene.is_discarded() || !scene.is_object()) return json(json::value_t::discarded);
	const auto it = scene.find("entities");
	if (it == scene.end() || !it->is_array() || !it->empty()) return json(json::value_t::discarded);
	json::array_t& out = it->get_ref<json::array_t&>();
	out.reserve(parsed.size());
	for (json& v : parsed) out.push_back(std::move(v));
	if (piecesUsed) *piecesUsed = n;
	return scene;
}

} // namespace

namespace HE
{

json parseSceneText(const std::string& text, size_t minPieceBytes, size_t* piecesUsed)
{
	if (piecesUsed) *piecesUsed = 1;
	minPieceBytes = std::max<size_t>(minPieceBytes, 1);
	if (text.size() < 2 * minPieceBytes) return parseWhole(text);

	Span              array;
	std::vector<Span> elements;
	if (!findEntities(text, array, elements) || elements.size() < 2) return parseWhole(text);

	const size_t arrayBytes = array.end - array.begin;
	json scene = splitParse(
		elements, arrayBytes, minPieceBytes,
		[&text](const Span& s)
		{
			return json::parse(text.data() + s.begin, text.data() + s.end, nullptr,
			                   /*allow_exceptions=*/false);
		},
		[&text, &array, arrayBytes]
		{
			std::string skeleton;
			skeleton.reserve(text.size() - arrayBytes + 2);
			skeleton.append(text, 0, array.begin).append("[]").append(text, array.end, std::string::npos);
			return json::parse(skeleton, nullptr, /*allow_exceptions=*/false);
		},
		piecesUsed);
	return scene.is_discarded() ? parseWhole(text) : scene;
}

json parseSceneCbor(const std::vector<uint8_t>& data, size_t minPieceBytes, size_t* piecesUsed)
{
	if (piecesUsed) *piecesUsed = 1;
	minPieceBytes = std::max<size_t>(minPieceBytes, 1);
	if (data.size() < 2 * minPieceBytes) return decodeWhole(data);

	Span              array;
	std::vector<Span> elements;
	if (!findEntitiesCbor(data, array, elements) || elements.size() < 2) return decodeWhole(data);

	const size_t arrayBytes = array.end - array.begin;
	json scene = splitParse(
		elements, arrayBytes, minPieceBytes,
		[&data](const Span& s)
		{
			return json::from_cbor(data.data() + s.begin, data.data() + s.end, /*strict=*/true,
			                       /*allow_exceptions=*/false);
		},
		[&data, &array, arrayBytes]
		{
			std::vector<uint8_t> skeleton;
			skeleton.reserve(data.size() - arrayBytes + 1);
			skeleton.insert(skeleton.end(), data.begin(), data.begin() + array.begin);
			skeleton.push_back(0x80);   // an empty array
			skeleton.insert(skeleton.end(), data.begin() + array.end, data.end());
			return json::from_cbor(skeleton, /*strict=*/true, /*allow_exceptions=*/false);
		},
		piecesUsed);
	return scene.is_discarded() ? decodeWhole(data) : scene;
}

} // namespace HE
