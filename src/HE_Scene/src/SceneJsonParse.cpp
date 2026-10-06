#include "HorizonScene/SceneJsonParse.h"
#include <JobSystem/JobSystem.h>
#include <algorithm>
#include <atomic>
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

	// Pieces of whole elements, cut by bytes (the environment entity alone can be
	// kilobytes), at least minPieceBytes each and a few per worker so a slow one
	// does not hold up the rest.
	ThreadPool&  pool       = globalPool();
	const size_t arrayBytes = array.end - array.begin;
	size_t pieces = std::min({ arrayBytes / minPieceBytes, (pool.threadCount() + 1) * 4,
	                           elements.size() });
	if (pieces < 2) return parseWhole(text);
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

	// Each element parses straight out of `text`, no copy; jobs only touch their
	// own slots of `parsed`.
	std::vector<json> parsed(elements.size());
	std::atomic<bool> bad{ false };
	auto parseRange = [&](size_t lo, size_t hi)
	{
		for (size_t e = lo; e < hi && !bad.load(std::memory_order_relaxed); ++e)
		{
			json v = json::parse(text.data() + elements[e].begin, text.data() + elements[e].end,
			                     nullptr, /*allow_exceptions=*/false);
			if (v.is_discarded() || !v.is_object())
			{
				bad.store(true, std::memory_order_relaxed);
				return;
			}
			parsed[e] = std::move(v);
		}
	};

	std::vector<JobHandle> jobs;
	jobs.reserve(n - 1);
	for (size_t k = 1; k < n; ++k)
	{
		JobDesc desc;
		desc.name     = "SceneParse";
		desc.priority = JobPriority::High;   // the main thread waits on it
		const size_t lo = first[k], hi = first[k + 1];
		jobs.push_back(pool.schedule([&parseRange, lo, hi] { parseRange(lo, hi); }, std::move(desc)));
	}

	// Meanwhile here: everything around the array, then the first piece. Every
	// job refers to this frame's locals, so nothing may leave before all are done.
	json               scene;
	std::exception_ptr error;
	try
	{
		std::string skeleton;
		skeleton.reserve(text.size() - arrayBytes + 2);
		skeleton.append(text, 0, array.begin).append("[]").append(text, array.end, std::string::npos);
		scene = json::parse(skeleton, nullptr, /*allow_exceptions=*/false);
		parseRange(first[0], first[1]);
	}
	catch (...)
	{
		error = std::current_exception();
		bad.store(true, std::memory_order_relaxed);
	}
	for (const JobHandle& job : jobs)
	{
		try { job.wait(); }
		catch (...) { if (!error) error = std::current_exception(); }
	}
	if (error) std::rethrow_exception(error);

	if (bad.load() || scene.is_discarded() || !scene.is_object()) return parseWhole(text);
	const auto it = scene.find("entities");
	if (it == scene.end() || !it->is_array() || !it->empty()) return parseWhole(text);
	json::array_t& out = it->get_ref<json::array_t&>();
	out.reserve(parsed.size());
	for (json& v : parsed) out.push_back(std::move(v));
	if (piecesUsed) *piecesUsed = n;
	return scene;
}

} // namespace HE
