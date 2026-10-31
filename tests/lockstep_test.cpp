// Lockstep (runtime/src/net/lockstep.h): two players on the loopback, each on
// a thread, get the same input at every poll, delayed as promised, whichever
// starts first and whatever stalls.

#include "net/lockstep.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace net = cafe::net;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// What a player presses at poll n (every poll differs, so a mix-up shows).
net::PadSample pressed(bool host, uint32_t n) {
    net::PadSample s;
    s.buttons = (host ? 0x1000u : 0x2000u) | (n * 37u & 0xFFu);
    s.left_x = static_cast<int8_t>((n * 5) % 200 - 100);
    s.left_y = static_cast<int8_t>(host ? -static_cast<int>(n % 100) : static_cast<int>(n % 100));
    return s;
}

struct Player {
    std::vector<net::Lockstep::Frame> frames;
    uint32_t stalls = 0;
    bool gave_up = false;
};

void play(bool host, uint16_t local_port, uint16_t remote_port, uint32_t polls, uint32_t delay, int start_late_ms,
          int stall_every, bool sync, Player& out) {
    std::this_thread::sleep_for(std::chrono::milliseconds(start_late_ms));
    net::RollbackNetConfig config;
    config.enable_dtls = false;
    net::RollbackNetSession session;
    if (!session.start(config, "127.0.0.1", remote_port, local_port, "ttt2", "ttt2-rollback", host)) {
        out.gave_up = true;
        return;
    }
    net::LockstepConfig lockstep_config;
    lockstep_config.delay_frames = delay;
    lockstep_config.is_host = host;
    net::Lockstep lockstep(session, lockstep_config);
    if (sync) {
        // Some polls before the sync point (a menu being reached), then both are ready.
        for (int i = 0; i < 5; ++i) {
            if (lockstep.sync_point(false)) out.gave_up = true;
        }
        if (!lockstep.sync_point(true)) out.gave_up = true;
    }
    for (uint32_t n = 1; n <= polls; ++n) {
        // A frame that takes long (a load): the other side has to wait for it.
        if (stall_every != 0 && n % stall_every == 0) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const auto frame = lockstep.step(pressed(host, n));
        if (!frame) {
            out.gave_up = true;
            return;
        }
        out.frames.push_back(*frame);
    }
    out.stalls = lockstep.stalls();
}

void run(const std::string& name, uint16_t port_a, uint16_t port_b, uint32_t polls, uint32_t delay, int guest_late_ms,
         int host_stall_every, int guest_stall_every, bool sync = false) {
    Player host, guest;
    std::thread a([&] { play(true, port_a, port_b, polls, delay, 0, host_stall_every, sync, host); });
    std::thread b([&] { play(false, port_b, port_a, polls, delay, guest_late_ms, guest_stall_every, sync, guest); });
    a.join();
    b.join();

    check(!host.gave_up && !guest.gave_up, name + ": both play to the end");
    check(host.frames.size() == polls && guest.frames.size() == polls, name + ": every poll answered");
    size_t same = 0, host_ok = 0, guest_ok = 0;
    for (size_t i = 0; i < std::min(host.frames.size(), guest.frames.size()); ++i) {
        const uint32_t n = static_cast<uint32_t>(i + 1);
        same += host.frames[i].host == guest.frames[i].host && host.frames[i].guest == guest.frames[i].guest;
        // Poll n uses what was pressed at poll n - delay (nothing for the first polls).
        const net::PadSample want_host = n > delay ? pressed(true, n - delay) : net::PadSample{};
        const net::PadSample want_guest = n > delay ? pressed(false, n - delay) : net::PadSample{};
        host_ok += host.frames[i].host == want_host;
        guest_ok += host.frames[i].guest == want_guest;
    }
    check(same == polls, name + ": both machines see the same inputs at every poll (" + std::to_string(same) + ")");
    check(host_ok == polls, name + ": player 1 is the host's, delayed (" + std::to_string(host_ok) + ")");
    check(guest_ok == polls, name + ": player 2 is the guest's, delayed (" + std::to_string(guest_ok) + ")");
}

} // namespace

int main() {
    run("same start", 31020, 31021, 300, 3, 0, 0, 0);
    run("guest starts late", 31022, 31023, 200, 2, 400, 0, 0);
    run("stalls", 31024, 31025, 200, 3, 0, 17, 23);
    // A game that is still loading for seconds when the other one is already waiting.
    run("guest starts 3 s late", 31026, 31027, 120, 3, 3000, 0, 0);
    // Both ready at the sync point, one of them 2 s after the other: the counting starts together.
    run("sync point, guest 2 s late", 31028, 31029, 150, 3, 2000, 0, 0, true);
    if (g_failures == 0) std::printf("lockstep_test: ok\n");
    return g_failures == 0 ? 0 : 1;
}
