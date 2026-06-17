#include "doctest.h"
#include <Memory/Ref.h>

namespace {
    struct Counter : RefCounted {
        int& alive;
        explicit Counter(int& a) : alive(a) { ++alive; }
        ~Counter() override { --alive; }
    };

    struct Node : RefCounted {
        int value;
        explicit Node(int v) : value(v) {}
    };
}

TEST_CASE("Ref default is null")
{
    Ref<Counter> r;
    CHECK(!r);
    CHECK(r.get() == nullptr);
}

TEST_CASE("Ref acquires on construction, releases on destruction")
{
    int alive = 0;
    {
        Ref<Counter> r(new Counter(alive));
        CHECK(alive == 1);
        CHECK(r);
        CHECK(r->alive == alive);
    }
    CHECK(alive == 0);
}

TEST_CASE("Ref copy shares ownership")
{
    int alive = 0;
    Ref<Counter> a(new Counter(alive));
    {
        Ref<Counter> b = a;
        CHECK(alive == 1);
        CHECK(b.get() == a.get());
    }
    CHECK(alive == 1); // b released, a still holds
    a.reset();
    CHECK(alive == 0);
}

TEST_CASE("Ref move transfers ownership without an extra acquire/release")
{
    int alive = 0;
    Ref<Counter> a(new Counter(alive));
    Ref<Counter> b = std::move(a);
    CHECK(!a);
    CHECK(b);
    CHECK(alive == 1);
    b.reset();
    CHECK(alive == 0);
}

TEST_CASE("Ref copy-assignment releases previous and acquires new")
{
    int alive = 0;
    Ref<Counter> a(new Counter(alive)); // alive = 1
    Ref<Counter> b(new Counter(alive)); // alive = 2
    a = b; // a releases its Counter → alive = 1; acquires b's → alive stays 1
    CHECK(alive == 1);
    CHECK(a.get() == b.get());
    b.reset();
    CHECK(alive == 1); // a still holds
    a.reset();
    CHECK(alive == 0);
}

TEST_CASE("Ref move-assignment releases previous")
{
    int alive = 0;
    Ref<Counter> a(new Counter(alive)); // alive = 1
    Ref<Counter> b(new Counter(alive)); // alive = 2
    a = std::move(b); // a releases first Counter → alive = 1; takes b's → alive stays 1
    CHECK(!b);
    CHECK(alive == 1);
    a.reset();
    CHECK(alive == 0);
}

TEST_CASE("Ref self copy-assignment is safe")
{
    int alive = 0;
    Ref<Counter> r(new Counter(alive));
    r = r; // NOLINT(clang-diagnostic-self-assign-overloaded)
    CHECK(alive == 1);
    CHECK(r);
}

TEST_CASE("Ref self move-assignment is safe")
{
    int alive = 0;
    Ref<Counter> r(new Counter(alive));
    r = std::move(r); // NOLINT(clang-diagnostic-self-move)
    // After self-move, r may be null (we cleared it). Object must not double-free.
    CHECK(alive <= 1); // not crashed
}

TEST_CASE("makeRef constructs in-place and returns a live Ref")
{
    int alive = 0;
    {
        auto r = makeRef<Counter>(alive);
        CHECK(alive == 1);
        CHECK(r);
        CHECK(r->alive == 1);
    }
    CHECK(alive == 0);
}

TEST_CASE("RefCounted copy ctor resets refcount to zero")
{
    auto a = makeRef<Node>(7);
    CHECK(a->useCount() == 1);

    Node* b = new Node(*a.get()); // copy-constructs RefCounted — refcount resets to 0
    CHECK(b->value == 7);
    CHECK(b->useCount() == 0);

    Ref<Node> rb(b); // acquires → refcount becomes 1
    CHECK(rb->useCount() == 1);
    // rb goes out of scope → deletes b via virtual ~Node → ~RefCounted
}

TEST_CASE("Ref operator== and operator!=")
{
    int alive = 0;
    Ref<Counter> a(new Counter(alive));
    Ref<Counter> b = a;
    Ref<Counter> c(new Counter(alive));
    CHECK(a == b);
    CHECK(a != c);
}
