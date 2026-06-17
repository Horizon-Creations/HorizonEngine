#include "doctest.h"
#include <Events/EventBus.h>
#include <string>
#include <vector>

// ── Event types used across tests ─────────────────────────────────────────────
struct PlayerDied   { int score; };
struct ItemPickedUp { std::string name; int amount; };
struct Tick         {};

// ─── Basic subscribe / publish ────────────────────────────────────────────────

TEST_CASE("EventBus: no subscribers = publish is a no-op")
{
    EventBus bus;
    bus.publish(PlayerDied{ 0 }); // must not crash or throw
    CHECK(bus.totalSubscriberCount() == 0);
}

TEST_CASE("EventBus: single subscriber receives published event")
{
    EventBus bus;
    int received = 0;
    auto sub = bus.subscribe<PlayerDied>([&](const PlayerDied& e) {
        received = e.score;
    });
    bus.publish(PlayerDied{ 42 });
    CHECK(received == 42);
}

TEST_CASE("EventBus: multiple subscribers all receive the event")
{
    EventBus bus;
    int callA = 0, callB = 0;
    auto subA = bus.subscribe<Tick>([&](const Tick&) { ++callA; });
    auto subB = bus.subscribe<Tick>([&](const Tick&) { ++callB; });

    bus.publish(Tick{});
    CHECK(callA == 1);
    CHECK(callB == 1);
}

TEST_CASE("EventBus: publish only reaches handlers of matching type")
{
    EventBus bus;
    bool gotDied = false, gotPick = false;
    auto s1 = bus.subscribe<PlayerDied>  ([&](const PlayerDied&)   { gotDied = true; });
    auto s2 = bus.subscribe<ItemPickedUp>([&](const ItemPickedUp&) { gotPick = true; });

    bus.publish(PlayerDied{ 0 });
    CHECK(gotDied  == true);
    CHECK(gotPick  == false);
}

TEST_CASE("EventBus: event payload is correctly forwarded")
{
    EventBus bus;
    std::string name;
    int amount = 0;
    auto sub = bus.subscribe<ItemPickedUp>([&](const ItemPickedUp& e) {
        name   = e.name;
        amount = e.amount;
    });
    bus.publish(ItemPickedUp{ "Coin", 5 });
    CHECK(name   == "Coin");
    CHECK(amount == 5);
}

// ─── Subscription lifetime ────────────────────────────────────────────────────

TEST_CASE("EventBus: handler is removed when Subscription is destroyed")
{
    EventBus bus;
    int count = 0;
    {
        auto sub = bus.subscribe<Tick>([&](const Tick&) { ++count; });
        bus.publish(Tick{});
        CHECK(count == 1);
    } // sub destroyed → unsubscribed
    bus.publish(Tick{});
    CHECK(count == 1); // no second call
}

TEST_CASE("EventBus: Subscription::release() unregisters immediately")
{
    EventBus bus;
    int count = 0;
    auto sub = bus.subscribe<Tick>([&](const Tick&) { ++count; });
    sub.release();
    bus.publish(Tick{});
    CHECK(count == 0);
    CHECK(!sub.valid());
}

TEST_CASE("EventBus: double release is safe (no crash)")
{
    EventBus bus;
    auto sub = bus.subscribe<Tick>([](const Tick&) {});
    sub.release();
    sub.release(); // must not crash
}

TEST_CASE("EventBus: move-constructed Subscription transfers ownership")
{
    EventBus bus;
    int count = 0;
    auto s1 = bus.subscribe<Tick>([&](const Tick&) { ++count; });
    auto s2 = std::move(s1);        // s1 is now invalid, s2 holds the subscription

    CHECK(!s1.valid());
    CHECK(s2.valid());

    bus.publish(Tick{});
    CHECK(count == 1);
}

TEST_CASE("EventBus: move-assigned Subscription transfers and releases old")
{
    EventBus bus;
    int countA = 0, countB = 0;
    auto sA = bus.subscribe<Tick>([&](const Tick&) { ++countA; });
    auto sB = bus.subscribe<Tick>([&](const Tick&) { ++countB; });

    sA = std::move(sB); // sA (old) is released; sB transferred to sA
    bus.publish(Tick{});
    CHECK(countA == 0); // old sA handler gone
    CHECK(countB == 1); // sB handler still active via sA
}

// ─── Subscriber counts ────────────────────────────────────────────────────────

TEST_CASE("EventBus: subscriberCount reflects registrations and removals")
{
    EventBus bus;
    CHECK(bus.subscriberCount<Tick>() == 0);

    auto s1 = bus.subscribe<Tick>([](const Tick&) {});
    CHECK(bus.subscriberCount<Tick>() == 1);

    auto s2 = bus.subscribe<Tick>([](const Tick&) {});
    CHECK(bus.subscriberCount<Tick>() == 2);

    s1.release();
    CHECK(bus.subscriberCount<Tick>() == 1);

    s2.release();
    CHECK(bus.subscriberCount<Tick>() == 0);
}

TEST_CASE("EventBus: totalSubscriberCount sums across event types")
{
    EventBus bus;
    auto s1 = bus.subscribe<Tick>      ([](const Tick&)         {});
    auto s2 = bus.subscribe<PlayerDied>([](const PlayerDied&)   {});
    auto s3 = bus.subscribe<PlayerDied>([](const PlayerDied&)   {});

    CHECK(bus.totalSubscriberCount() == 3);
    s2.release();
    CHECK(bus.totalSubscriberCount() == 2);
}

// ─── Subscribe/unsubscribe during dispatch ────────────────────────────────────

TEST_CASE("EventBus: subscribing during dispatch does not affect current publish")
{
    EventBus bus;
    int count = 0;
    Subscription inner;

    auto outer = bus.subscribe<Tick>([&](const Tick&) {
        ++count;
        // subscribe a new handler inside the callback
        inner = bus.subscribe<Tick>([&](const Tick&) { ++count; });
    });

    bus.publish(Tick{});
    CHECK(count == 1); // only outer ran; inner is not in the snapshot

    bus.publish(Tick{});
    CHECK(count == 3); // outer + inner both run
}

TEST_CASE("EventBus: unsubscribing during dispatch does not crash")
{
    EventBus bus;
    int count = 0;
    Subscription selfSub;

    selfSub = bus.subscribe<Tick>([&](const Tick&) {
        ++count;
        selfSub.release(); // unsubscribe self during handler
    });

    bus.publish(Tick{}); // must not crash
    CHECK(count == 1);

    bus.publish(Tick{}); // handler was removed — no second call
    CHECK(count == 1);
}
