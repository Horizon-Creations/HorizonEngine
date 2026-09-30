#include "doctest.h"

#include "McpLockBook.h"

#include <algorithm>
#include <cstdint>
#include <vector>

// ─── Whose is an external lock ───────────────────────────────────────────────
// The bug this book exists to fix (docs/mcp-editor-integration-plan.md §20.3):
// the editor handed MCP locks back only once it had NO client left, so a client
// that hung up while another stayed connected left its locks to the survivor.
// Every case below is one side of "per client, not per editor".

using HE::Ed::McpLockBook;

namespace {

bool has(const std::vector<std::uint64_t>& v, std::uint64_t s)
{
	return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("McpLockBook: a client's locks go back when THAT client goes, not when the last one does")
{
	McpLockBook book;
	book.take(100, /*client=*/1, 0);
	book.take(200, /*client=*/1, 0);
	book.take(300, /*client=*/2, 0);

	// The scenario from the two-editor run: the holder hangs up, client 2 stays.
	const auto released = book.clientGone(1);
	CHECK(released.size() == 2);
	CHECK(has(released, 100));
	CHECK(has(released, 200));

	// Client 2's lock is untouched, and it did not inherit client 1's.
	CHECK(book.size() == 1);
	CHECK(book.holds(300, 2));
	CHECK_FALSE(book.holds(100, 2));
	CHECK_FALSE(book.holds(200, 2));
}

TEST_CASE("McpLockBook: two clients on one subject — it stays until the second leaves")
{
	McpLockBook book;
	book.take(100, 1, 0);
	book.take(100, 2, 0);
	CHECK(book.size() == 1);   // one session lock, two holders

	CHECK(book.clientGone(1).empty());   // pulling it now would rob client 2
	CHECK(book.holds(100, 2));

	const auto released = book.clientGone(2);
	REQUIRE(released.size() == 1);
	CHECK(released[0] == 100);
	CHECK(book.empty());
}

TEST_CASE("McpLockBook: take is idempotent, subject 0 is nothing, a stranger's goodbye changes nothing")
{
	McpLockBook book;
	book.take(100, 1, 5);
	book.take(100, 1, 9);   // same client again
	REQUIRE(book.size() == 1);
	CHECK(book.entries()[0].holders.size() == 1);
	// The stamp is the first ask's: a re-take must not push the re-ask back.
	CHECK(book.entries()[0].lastAskedMs == 5);

	book.take(0, 1, 0);
	CHECK(book.size() == 1);

	CHECK(book.clientGone(7).empty());   // never held anything
	CHECK(book.holds(100, 1));
}

TEST_CASE("McpLockBook: the anonymous client never disconnects, only takeAll frees it")
{
	McpLockBook book;
	book.take(100, /*anonymous=*/0, 0);
	book.take(200, 3, 0);

	// A real client leaving does not take the anonymous caller's lock with it…
	CHECK(book.clientGone(3) == std::vector<std::uint64_t>{ 200 });
	CHECK(book.holds(100, 0));

	// …the "no client left" sweep does.
	const auto all = book.takeAll();
	CHECK(all == std::vector<std::uint64_t>{ 100 });
	CHECK(book.empty());
}
