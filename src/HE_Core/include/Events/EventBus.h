#pragma once
#include "Types/Defines.h"
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <memory>
#include <algorithm>

// Forward declaration
class EventBus;

// RAII subscription handle. When destroyed (or explicitly released),
// the handler is automatically unregistered from the EventBus.
class HE_API Subscription
{
public:
    Subscription() = default;
    ~Subscription() { release(); }

    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    // Unsubscribe immediately, before destruction.
    void release();

    bool valid() const { return m_bus != nullptr; }

private:
    friend class EventBus;
    Subscription(EventBus* bus, std::type_index type, uint64_t id)
        : m_bus(bus), m_type(type), m_id(id) {}

    EventBus*       m_bus  = nullptr;
    std::type_index m_type { typeid(void) };
    uint64_t        m_id   = 0;
};

// Central event dispatcher. Supports typed publish/subscribe without
// inheritance or virtual dispatch on the event side.
//
// Usage:
//   struct PlayerDied { int score; };
//   EventBus bus;
//   auto sub = bus.subscribe<PlayerDied>([](const PlayerDied& e) { ... });
//   bus.publish(PlayerDied{ 42 });
//   // sub goes out of scope → auto-unsubscribed
class HE_API EventBus
{
public:
    template<typename TEvent>
    using Handler = std::function<void(const TEvent&)>;

    // Register a handler. Returns a Subscription that unregisters the handler
    // when it goes out of scope (or when Subscription::release() is called).
    template<typename TEvent>
    [[nodiscard]] Subscription subscribe(Handler<TEvent> handler)
    {
        const std::type_index type { typeid(TEvent) };
        const uint64_t        id   = ++m_nextId;

        auto& list = m_handlers[type];
        list.push_back({ id, [h = std::move(handler)](const void* ev) {
            h(*static_cast<const TEvent*>(ev));
        }});

        return Subscription(this, type, id);
    }

    // Dispatch an event to all registered handlers of that type.
    template<typename TEvent>
    void publish(const TEvent& event)
    {
        const std::type_index type { typeid(TEvent) };
        auto it = m_handlers.find(type);
        if (it == m_handlers.end()) return;

        // Copy list so handlers can safely subscribe/unsubscribe during dispatch.
        auto snapshot = it->second;
        for (auto& entry : snapshot)
            entry.fn(&event);
    }

    // Number of handlers currently registered for a given event type.
    template<typename TEvent>
    size_t subscriberCount() const
    {
        const std::type_index type { typeid(TEvent) };
        auto it = m_handlers.find(type);
        return it != m_handlers.end() ? it->second.size() : 0u;
    }

    // Total number of registered handlers across all event types.
    size_t totalSubscriberCount() const
    {
        size_t n = 0;
        for (auto& [t, v] : m_handlers) n += v.size();
        return n;
    }

private:
    friend class Subscription;

    void unsubscribe(std::type_index type, uint64_t id)
    {
        auto it = m_handlers.find(type);
        if (it == m_handlers.end()) return;
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
            [id](const Entry& e) { return e.id == id; }), list.end());
        if (list.empty()) m_handlers.erase(it);
    }

    struct Entry {
        uint64_t                       id;
        std::function<void(const void*)> fn;
    };

    std::unordered_map<std::type_index, std::vector<Entry>> m_handlers;
    uint64_t m_nextId = 0;
};

// ── Subscription implementation (needs EventBus to be complete) ──────────────

inline Subscription::Subscription(Subscription&& o) noexcept
    : m_bus(o.m_bus), m_type(o.m_type), m_id(o.m_id)
{
    o.m_bus = nullptr;
}

inline Subscription& Subscription::operator=(Subscription&& o) noexcept
{
    release();
    m_bus  = o.m_bus; m_type = o.m_type; m_id = o.m_id;
    o.m_bus = nullptr;
    return *this;
}

inline void Subscription::release()
{
    if (m_bus) { m_bus->unsubscribe(m_type, m_id); m_bus = nullptr; }
}
