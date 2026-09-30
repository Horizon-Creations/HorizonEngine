#include "doctest.h"

#include "QuickSettingsFavorites.h"

#include <string>

// The pin in Preferences asks favoritesContain() once per row, every frame.
// It replaced a version that built ",<list>," and ",<key>," as strings and
// searched one in the other, so the thing to hold is that both give the same
// answer — a key is pinned exactly when it is one whole comma-separated token.

using HE::Ed::favoritesContain;

namespace
{
	// The old spelling, kept here as the reference the new one is held to.
	bool viaConcatenation(const std::string& list, const std::string& key)
	{
		const std::string hay = "," + list + ",";
		return hay.find("," + key + ",") != std::string::npos;
	}
} // namespace

TEST_CASE("quick settings: a key is pinned when it is a whole token of the list")
{
	const std::string list = "backend,vsync,grid,bloom,ssao";
	CHECK(favoritesContain(list, "backend"));   // first
	CHECK(favoritesContain(list, "grid"));      // middle
	CHECK(favoritesContain(list, "ssao"));      // last

	// Part of a token is not the token: "ss" is in "ssao", "vsync" contains
	// "sync", "bloom" starts with "bloo".
	CHECK_FALSE(favoritesContain(list, "ss"));
	CHECK_FALSE(favoritesContain(list, "sync"));
	CHECK_FALSE(favoritesContain(list, "bloo"));
	CHECK_FALSE(favoritesContain(list, "ssao2"));
	// Two tokens are not one. (The old search said yes here; no setting key
	// contains a comma, so nothing depended on it.)
	CHECK_FALSE(favoritesContain(list, "grid,bloom"));

	CHECK_FALSE(favoritesContain("", "grid"));
	CHECK(favoritesContain("grid", "grid"));
}

TEST_CASE("quick settings: the walk agrees with the string search it replaced")
{
	// Including the lists a hand-edited config.json can hold: doubled, leading
	// and trailing commas.
	const char* lists[] = { "", "a", "a,b", ",a", "a,", ",,a,,b,,", "ab,a", "a,ab", "b,ab,abc",
	                        "backend,vsync,grid,bloom,ssao" };
	const char* keys[]  = { "a", "b", "ab", "abc", "c", "grid", "vsync", "sync" };
	for (const char* l : lists)
		for (const char* k : keys)
		{
			CAPTURE(std::string(l));
			CAPTURE(std::string(k));
			CHECK(favoritesContain(l, k) == viaConcatenation(l, k));
		}

	// The one deliberate difference: an empty key is never pinned. The old
	// search found ",," in any list with an empty token (and in the empty
	// list), which no real setting key can be.
	CHECK_FALSE(favoritesContain(",,", ""));
	CHECK_FALSE(favoritesContain("", ""));
}
