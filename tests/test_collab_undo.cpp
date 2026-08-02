#include "doctest.h"

#include "../src/HE_Editor/CollabUndo.h"

#include <set>
#include <string>
#include <vector>

// ─── Per-user undo/redo in a shared session ──────────────────────────────────
// The point of this stack is that undo becomes an ordinary edit — an inverse
// operation republished like any other — rather than restoring a whole-world
// snapshot, which in a session would revert everyone else's work too.

namespace {

struct Harness {
    CollabUndo undo;

    // What the "world" currently holds, recorded by the apply handlers.
    std::vector<std::pair<std::uint64_t, std::vector<float>>> transformApplies;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> assetApplies;
    std::set<std::uint64_t> owned;

    Harness() {
        undo.setHandlers(
            [this](std::uint64_t subject, const float v[9]) {
                transformApplies.emplace_back(subject, std::vector<float>(v, v + 9));
            },
            [this](const std::string& path, const std::vector<std::uint8_t>& bytes) {
                assetApplies.emplace_back(path, bytes);
            },
            [this](std::uint64_t subject) { return owned.count(subject) > 0; });
    }
};

std::vector<float> t9(float base) {
    return { base, base + 1, base + 2, base + 3, base + 4,
             base + 5, base + 6, base + 7, base + 8 };
}

} // namespace

TEST_CASE("CollabUndo: undo re-applies the previous value, redo the new one")
{
    Harness h;
    h.owned.insert(7);

    const auto before = t9(0.0f);
    const auto after  = t9(100.0f);
    h.undo.recordTransform(7, before.data(), after.data());

    REQUIRE(h.undo.canUndo());
    REQUIRE_FALSE(h.undo.canRedo());

    REQUIRE(h.undo.undo());
    REQUIRE(h.transformApplies.size() == 1);
    CHECK(h.transformApplies[0].first == 7);
    CHECK(h.transformApplies[0].second == before);

    // Undo consumed the entry and handed it to redo.
    CHECK_FALSE(h.undo.canUndo());
    REQUIRE(h.undo.canRedo());

    REQUIRE(h.undo.redo());
    REQUIRE(h.transformApplies.size() == 2);
    CHECK(h.transformApplies[1].second == after);
    CHECK(h.undo.canUndo());
}

TEST_CASE("CollabUndo: several changes undo in reverse order")
{
    Harness h;
    h.owned.insert(1);

    h.undo.recordTransform(1, t9(0.0f).data(),  t9(10.0f).data());
    h.undo.recordTransform(1, t9(10.0f).data(), t9(20.0f).data());
    h.undo.recordTransform(1, t9(20.0f).data(), t9(30.0f).data());

    REQUIRE(h.undo.undo());
    CHECK(h.transformApplies.back().second == t9(20.0f));
    REQUIRE(h.undo.undo());
    CHECK(h.transformApplies.back().second == t9(10.0f));
    REQUIRE(h.undo.undo());
    CHECK(h.transformApplies.back().second == t9(0.0f));

    CHECK_FALSE(h.undo.canUndo());
}

TEST_CASE("CollabUndo: a new change discards the redo branch")
{
    Harness h;
    h.owned.insert(2);

    h.undo.recordTransform(2, t9(0.0f).data(), t9(5.0f).data());
    REQUIRE(h.undo.undo());
    REQUIRE(h.undo.canRedo());

    // Those redo entries describe a future that no longer exists.
    h.undo.recordTransform(2, t9(0.0f).data(), t9(9.0f).data());
    CHECK_FALSE(h.undo.canRedo());
}

TEST_CASE("CollabUndo: entries for subjects we no longer hold are dropped")
{
    Harness h;
    h.owned.insert(3);
    h.undo.recordTransform(3, t9(0.0f).data(), t9(1.0f).data());
    REQUIRE(h.undo.canUndo());

    // The lock went away — someone else may have changed it since, so the
    // recorded "before" is no longer a truthful inverse. Replaying it would
    // overwrite their work rather than undo ours.
    h.owned.clear();

    CHECK_FALSE(h.undo.undo());
    CHECK_FALSE(h.undo.canUndo());
    CHECK(h.transformApplies.empty());   // nothing was applied
}

TEST_CASE("CollabUndo: only the unowned entries are dropped, not the whole stack")
{
    Harness h;
    h.owned.insert(10);
    h.owned.insert(11);
    h.undo.recordTransform(10, t9(0.0f).data(), t9(1.0f).data());
    h.undo.recordTransform(11, t9(0.0f).data(), t9(2.0f).data());
    REQUIRE(h.undo.undoDepth() == 2);

    h.owned.erase(11);   // gave up one of them
    h.undo.dropUnowned();

    CHECK(h.undo.undoDepth() == 1);
    REQUIRE(h.undo.undo());
    CHECK(h.transformApplies.back().first == 10);
}

TEST_CASE("CollabUndo: asset changes undo to their previous bytes")
{
    Harness h;
    constexpr std::uint64_t kAsset = 0x8000'0000'0000'0009ull;
    h.owned.insert(kAsset);

    const std::vector<std::uint8_t> before{ 1, 2, 3 };
    const std::vector<std::uint8_t> after { 9, 9, 9, 9 };
    h.undo.recordAsset(kAsset, "Content/Materials/Steel.hmat", before, after);

    REQUIRE(h.undo.undo());
    REQUIRE(h.assetApplies.size() == 1);
    CHECK(h.assetApplies[0].first == "Content/Materials/Steel.hmat");
    CHECK(h.assetApplies[0].second == before);

    REQUIRE(h.undo.redo());
    CHECK(h.assetApplies.back().second == after);
}

TEST_CASE("CollabUndo: an asset with no previous state cannot be undone")
{
    Harness h;
    constexpr std::uint64_t kAsset = 0x8000'0000'0000'000Aull;
    h.owned.insert(kAsset);

    // Freshly created: undoing would mean deleting the file, which is a
    // different operation than this stack models.
    h.undo.recordAsset(kAsset, "Content/New.hmat", {}, { 1, 2, 3 });

    CHECK_FALSE(h.undo.undo());
    CHECK(h.assetApplies.empty());
}

TEST_CASE("CollabUndo: labels say what will actually happen")
{
    Harness h;
    h.owned.insert(0x8000'0000'0000'0001ull);
    h.undo.recordAsset(0x8000'0000'0000'0001ull, "Content/UI/MainMenu.huiw",
                       { 1 }, { 2 });

    // A bare "Undo" in the menu tells the user nothing in a session where
    // several kinds of change are in flight.
    CHECK(h.undo.undoLabel().find("MainMenu.huiw") != std::string::npos);

    REQUIRE(h.undo.undo());
    CHECK(h.undo.redoLabel().find("MainMenu.huiw") != std::string::npos);
}

TEST_CASE("CollabUndo: clearing empties both directions")
{
    Harness h;
    h.owned.insert(4);
    h.undo.recordTransform(4, t9(0.0f).data(), t9(1.0f).data());
    REQUIRE(h.undo.undo());
    REQUIRE(h.undo.canRedo());

    h.undo.clear();
    CHECK_FALSE(h.undo.canUndo());
    CHECK_FALSE(h.undo.canRedo());
}
