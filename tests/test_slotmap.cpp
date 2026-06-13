#include "doctest.h"
#include <SlotMap.h>
#include <string>

TEST_CASE("SlotMap insert and get")
{
	SlotMap<std::string> map;
	SlotHandle a = map.insert("alpha");
	SlotHandle b = map.insert("beta");

	REQUIRE(map.get(a) != nullptr);
	REQUIRE(map.get(b) != nullptr);
	CHECK(*map.get(a) == "alpha");
	CHECK(*map.get(b) == "beta");
	CHECK(map.isValid(a));
	CHECK(map.isValid(b));
}

TEST_CASE("SlotMap remove invalidates handle")
{
	SlotMap<int> map;
	SlotHandle h = map.insert(42);
	REQUIRE(map.isValid(h));

	map.remove(h);
	CHECK_FALSE(map.isValid(h));
	CHECK(map.get(h) == nullptr);
}

TEST_CASE("SlotMap slot reuse bumps generation")
{
	SlotMap<int> map;
	SlotHandle first = map.insert(1);
	map.remove(first);

	SlotHandle second = map.insert(2);
	// Same slot may be reused, but the stale handle must stay invalid
	CHECK_FALSE(map.isValid(first));
	CHECK(map.isValid(second));
	CHECK(*map.get(second) == 2);
}

TEST_CASE("SlotMap remove middle keeps others valid")
{
	SlotMap<int> map;
	SlotHandle a = map.insert(10);
	SlotHandle b = map.insert(20);
	SlotHandle c = map.insert(30);

	map.remove(b);
	REQUIRE(map.isValid(a));
	REQUIRE(map.isValid(c));
	CHECK(*map.get(a) == 10);
	CHECK(*map.get(c) == 30);
}
