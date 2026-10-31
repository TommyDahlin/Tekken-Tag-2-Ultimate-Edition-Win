#include "net/lockstep.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace cafe::net {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

constexpr size_t kPayloadSize = 8;

std::array<uint8_t, kPayloadSize> encode(const PadSample& s) {
    return {static_cast<uint8_t>(s.buttons & 0xFF),
            static_cast<uint8_t>((s.buttons >> 8) & 0xFF),
            static_cast<uint8_t>((s.buttons >> 16) & 0xFF),
            static_cast<uint8_t>((s.buttons >> 24) & 0xFF),
            static_cast<uint8_t>(s.left_x),
            static_cast<uint8_t>(s.left_y),
            static_cast<uint8_t>(s.right_x),
            static_cast<uint8_t>(s.right_y)};
}

PadSample decode(const std::vector<uint8_t>& p) {
    PadSample s;
    if (p.size() != kPayloadSize) return s; // not ours: no input
    s.buttons = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                (static_cast<uint32_t>(p[3]) << 24);
    s.left_x = static_cast<int8_t>(p[4]);
    s.left_y = static_cast<int8_t>(p[5]);
    s.right_x = static_cast<int8_t>(p[6]);
    s.right_y = static_cast<int8_t>(p[7]);
    return s;
}

} // namespace

Lockstep::Lockstep(RollbackNetSession& session, const LockstepConfig& config)
    : m_session(session), m_config(config) {
    m_config.delay_frames = std::max<uint32_t>(1, m_config.delay_frames); // 0 would have both wait on each other
}

void Lockstep::flush_unsent() {
    while (!m_unsent.empty()) {
        const uint32_t label = m_unsent.front();
        const auto it = m_mine.find(label);
        if (it == m_mine.end()) {
            m_unsent.pop_front();
            continue;
        }
        const auto payload = encode(it->second);
        if (!m_session.submit_local_input(label, payload)) return; // not connected yet
        m_unsent.pop_front();
    }
}

bool Lockstep::sync_point(bool ready) {
    if (m_started) return true;
    m_session.update(now_ms());
    if (!ready) return false;
    const uint64_t began = now_ms();
    uint64_t last_said = began;
    for (;;) {
        if (!m_ready_sent) m_ready_sent = m_session.send_ready();
        if (m_ready_sent && m_session.peer_ready()) break;
        const uint64_t now = now_ms();
        if (now - last_said >= 3000) {
            last_said = now;
            std::fprintf(stderr, "ttt2: lockstep: at the sync point, waiting for the other player (%llu ms)\n",
                         static_cast<unsigned long long>(now - began));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
        m_session.update(now_ms());
    }
    // Both are here: count from now, with nothing left over from before.
    m_poll = 0;
    m_mine.clear();
    m_unsent.clear();
    m_heard = true;
    m_started = true;
    return true;
}

std::optional<Lockstep::Frame> Lockstep::step(const PadSample& local) {
    const uint32_t n = ++m_poll;
    const uint32_t d = m_config.delay_frames;

    m_session.update(now_ms());
    m_mine[n + d] = local;
    m_unsent.push_back(n + d);
    flush_unsent();

    PadSample mine, theirs;
    if (n > d) {
        mine = m_mine[n];
        const uint64_t began = now_ms();
        uint64_t last_progress = began;
        uint64_t last_said = began;
        bool waited = false;
        for (;;) {
            std::vector<uint8_t> payload;
            if (m_session.consume_remote_input(n, payload)) {
                theirs = decode(payload);
                m_heard = true;
                break;
            }
            const uint64_t now = now_ms();
            waited = true;
            if (m_heard && now - last_progress > m_config.timeout_ms) return std::nullopt;
            if (now - last_said >= 3000) {
                last_said = now;
                const RollbackNetStats& stats = m_session.stats();
                std::fprintf(stderr,
                             "ttt2: lockstep: poll %u is waiting for the other player's input (%llu ms; %s; sent %u, "
                             "received %u, resent %u)\n",
                             n, static_cast<unsigned long long>(now - began),
                             m_heard ? "heard before" : "not heard yet", stats.packets_sent, stats.packets_received,
                             stats.packets_resent);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            m_session.update(now_ms());
            flush_unsent();
        }
        if (waited) {
            ++m_stalls;
            m_longest_wait_ms = std::max(m_longest_wait_ms, static_cast<uint32_t>(now_ms() - began));
        }
    }
    // What has been used (or never will be) can go.
    m_mine.erase(m_mine.begin(), m_mine.upper_bound(n));

    return m_config.is_host ? Frame{mine, theirs} : Frame{theirs, mine};
}

} // namespace cafe::net
