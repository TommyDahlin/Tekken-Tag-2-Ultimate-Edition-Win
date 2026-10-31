// The rollback transport (runtime/src/net/rollback_net.h): two sessions on
// the loopback exchange inputs, plain and over DTLS.

#include "net/rollback_net.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace net = cafe::net;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

// Runs both sessions until `done` or two seconds pass.
template <typename Done>
bool pump(net::RollbackNetSession& a, net::RollbackNetSession& b, Done done) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        a.update(now_ms());
        b.update(now_ms());
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void exchange(bool dtls, uint16_t port_a, uint16_t port_b) {
    const std::string mode = dtls ? "dtls" : "plain";
    net::RollbackNetConfig config;
    config.enable_dtls = dtls;

    net::RollbackNetSession server, client;
    check(server.start(config, "127.0.0.1", port_b, port_a, "ttt2", "ttt2-rollback", true), mode + ": server starts");
    check(client.start(config, "127.0.0.1", port_a, port_b, "ttt2", "ttt2-rollback", false), mode + ": client starts");

    check(pump(server, client, [&] { return server.is_connected() && client.is_connected(); }),
          mode + ": both connect");

    const std::array<uint8_t, 4> from_server{1, 2, 3, 4};
    const std::array<uint8_t, 4> from_client{9, 8, 7, 6};
    check(server.submit_local_input(10, from_server), mode + ": server submits");
    check(client.submit_local_input(20, from_client), mode + ": client submits");

    std::vector<uint8_t> at_client, at_server;
    bool got_client = false, got_server = false;
    pump(server, client, [&] {
        got_client = got_client || client.consume_remote_input(10, at_client);
        got_server = got_server || server.consume_remote_input(20, at_server);
        return got_client && got_server;
    });
    check(got_client && at_client == std::vector<uint8_t>(from_server.begin(), from_server.end()),
          mode + ": client receives the server's input");
    check(got_server && at_server == std::vector<uint8_t>(from_client.begin(), from_client.end()),
          mode + ": server receives the client's input");

    check(server.stats().send_errors == 0 && client.stats().send_errors == 0, mode + ": no send errors");
    check(server.stats().packets_sent > 0 && client.stats().packets_received > 0, mode + ": packets counted");
}

} // namespace

int main() {
    exchange(false, 31010, 31011);
    if (cafe::net::dtls_available()) {
        exchange(true, 31012, 31013);
    } else {
        std::printf("rollback_net_test: no OpenSSL in this build, DTLS skipped\n");
    }
    if (g_failures == 0) std::printf("rollback_net_test: ok\n");
    return g_failures == 0 ? 0 : 1;
}
