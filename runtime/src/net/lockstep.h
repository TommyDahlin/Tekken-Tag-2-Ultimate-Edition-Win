#pragma once

// Lockstep input exchange over a RollbackNetSession.
//
// Both players poll their pad once per game frame (a "poll", counted from 1).
// With `delay_frames` = D, the sample taken at poll n is sent as input n + D and
// used at poll n + D, by both machines; a poll waits until the other player's
// input for it has arrived. So both games get the same two inputs at every
// poll, whatever the network does (a late packet stalls the game, never
// diverges it), and because each side sends D polls ahead, neither ever waits
// on the other at the same time. The first D polls use no input (the peer's
// first input is labelled D + 1), which also lets the two games start at
// different times: the one that starts first runs D polls and waits.
//
// The host is player 1 and the guest player 2 on both machines.

#include "net/rollback_net.h"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>

namespace cafe::net {

struct PadSample {
    uint32_t buttons = 0;
    int8_t left_x = 0, left_y = 0, right_x = 0, right_y = 0; // -127..127, up positive

    friend bool operator==(const PadSample&, const PadSample&) = default;
};

struct LockstepConfig {
    uint32_t delay_frames = 3;
    bool is_host = false;
    // Once the peer has been heard, this much silence ends the lockstep (step
    // gives up); before it has been heard, step waits as long as it takes.
    uint32_t timeout_ms = 10000;
};

class Lockstep {
  public:
    struct Frame {
        PadSample host; // player 1
        PadSample guest; // player 2
    };

    Lockstep(RollbackNetSession& session, const LockstepConfig& config);

    // The sync point. Before the lockstep starts, call this once per poll with
    // whether this player is ready (at the menu both start from): it returns
    // false until both have been. The call that makes `ready` true waits for
    // the other player's, then starts the counting at poll 0 on both machines
    // and returns true (and always does from then on). So two games that booted
    // at different speeds, or one a minute after the other, begin together.
    bool sync_point(bool ready);
    bool started() const { return m_started; }

    // One poll: takes this player's sample and returns what both players do at
    // this poll, waiting for the peer if need be. Empty when the peer has gone
    // quiet for longer than the timeout.
    std::optional<Frame> step(const PadSample& local);

    uint32_t poll() const { return m_poll; }
    bool heard_peer() const { return m_heard; }
    // Polls that had to wait for the peer, and the longest wait.
    uint32_t stalls() const { return m_stalls; }
    uint32_t longest_wait_ms() const { return m_longest_wait_ms; }

  private:
    void flush_unsent();

    RollbackNetSession& m_session;
    LockstepConfig m_config;
    uint32_t m_poll = 0;
    bool m_heard = false;
    bool m_started = false;
    bool m_ready_sent = false;
    uint32_t m_stalls = 0;
    uint32_t m_longest_wait_ms = 0;
    std::map<uint32_t, PadSample> m_mine; // own samples by the poll they are used at
    std::deque<uint32_t> m_unsent;        // labels not handed to the session yet (not connected)
};

} // namespace cafe::net
