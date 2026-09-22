#include "Net/LossyTransport.h"

#include "NetLog.h"

#include <algorithm>
#include <utility>

namespace HE::Net {

std::unique_ptr<LossyTransport> LossyTransport::wrap(std::unique_ptr<ITransport> inner,
                                                     Config cfg) {
    if (!inner) {
        HE_LOG_ERROR(Net, "LossyTransport::wrap called without an inner transport");
        return nullptr;
    }
    std::unique_ptr<LossyTransport> t(new LossyTransport());
    t->m_inner = std::move(inner);
    t->m_cfg   = cfg;
    t->m_rng.seed(cfg.seed);
    HE_LOG_DEBUG(Net, "Lossy transport armed: loss %.1f%%, reorder %.1f%%, dup %.1f%%, "
                      "latency %u ± %u ms, seed %u",
                 static_cast<double>(cfg.lossPercent), static_cast<double>(cfg.reorderPercent),
                 static_cast<double>(cfg.duplicatePercent), cfg.latencyMs, cfg.jitterMs,
                 cfg.seed);
    return t;
}

LossyTransport::~LossyTransport() = default;

bool LossyTransport::roll(float percent) {
    if (percent <= 0.f) return false;
    if (percent >= 100.f) return true;
    std::uniform_real_distribution<float> d(0.f, 100.f);
    return d(m_rng) < percent;
}

void LossyTransport::enqueue(ConnectionId conn, const std::uint8_t* data, std::size_t len,
                             SendMode mode, std::uint64_t dueMs) {
    Delayed d;
    d.dueMs = dueMs;
    d.order = m_counter++;
    d.conn  = conn;
    d.mode  = mode;
    d.data.assign(data, data + len);
    m_queue.push_back(std::move(d));
}

void LossyTransport::send(ConnectionId conn, const std::uint8_t* data,
                          std::size_t len, SendMode mode) {
    m_stats.offered++;
    if (roll(m_cfg.lossPercent)) {
        m_stats.dropped++;
        return;
    }

    std::uint64_t due = m_nowMs + m_cfg.latencyMs;
    if (m_cfg.jitterMs) {
        std::uniform_int_distribution<int> j(-static_cast<int>(m_cfg.jitterMs),
                                             static_cast<int>(m_cfg.jitterMs));
        const int off = j(m_rng);
        due = (off < 0 && static_cast<std::uint64_t>(-off) > due) ? 0 : due + off;
    }
    if (roll(m_cfg.reorderPercent)) {
        // Slipping a datagram behind the ones sent after it is what reorder
        // IS on a real path; the extra delay is at least one tick so it can
        // never land in the same update() as its successors.
        std::uniform_int_distribution<std::uint32_t> r(1, std::max(1u, m_cfg.reorderDelayMs));
        due += r(m_rng);
        m_stats.reordered++;
    }
    enqueue(conn, data, len, mode, due);
    if (roll(m_cfg.duplicatePercent)) {
        // The copy takes its own path through latency and jitter, so the two
        // can arrive in either order — as duplicates on a real network do.
        std::uint64_t dupDue = m_nowMs + m_cfg.latencyMs;
        if (m_cfg.jitterMs) {
            std::uniform_int_distribution<int> j(0, static_cast<int>(m_cfg.jitterMs));
            dupDue += j(m_rng);
        }
        enqueue(conn, data, len, mode, dupDue);
        m_stats.duplicated++;
    }
}

void LossyTransport::update() {
    // Deliver in due-time order, ties by submission order, so two datagrams
    // that fall due in one tick keep their relative order unless reorder
    // deliberately moved one.
    std::vector<Delayed> due;
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->dueMs <= m_nowMs) {
            due.push_back(std::move(*it));
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
    std::sort(due.begin(), due.end(), [](const Delayed& a, const Delayed& b) {
        return a.dueMs != b.dueMs ? a.dueMs < b.dueMs : a.order < b.order;
    });
    for (Delayed& d : due) {
        m_inner->send(d.conn, d.data.data(), d.data.size(), d.mode);
        m_stats.delivered++;
    }
    m_inner->update();
}

void LossyTransport::flush() {
    if (m_queue.empty()) { m_inner->update(); return; }
    std::uint64_t latest = 0;
    for (const Delayed& d : m_queue) latest = std::max(latest, d.dueMs);
    if (latest > m_nowMs) m_nowMs = latest;
    update();
}

bool LossyTransport::poll(NetEvent& out) {
    return m_inner->poll(out);
}

void LossyTransport::disconnect(ConnectionId conn) {
    // Anything still in flight to that peer is lost with the link, as it would be.
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [conn](const Delayed& d) { return d.conn == conn; }),
                  m_queue.end());
    m_inner->disconnect(conn);
}

std::size_t LossyTransport::connectionCount() const {
    return m_inner->connectionCount();
}

} // namespace HE::Net
