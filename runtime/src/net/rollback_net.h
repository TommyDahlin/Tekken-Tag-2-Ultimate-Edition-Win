#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace cafe::net {

enum class TransportProfile : uint8_t {
    KcpDtls = 0,
    EnetDtls = 1
};

struct RollbackNetConfig {
    TransportProfile profile = TransportProfile::KcpDtls;
    uint32_t max_payload_bytes = 64;
    uint32_t resend_interval_ms = 12;
    uint32_t heartbeat_interval_ms = 40;
    uint32_t timeout_ms = 3000;
    uint8_t base_input_delay_frames = 1;
    bool enable_dtls = true;
};

struct RollbackNetStats {
    uint32_t ping_ms = 0;
    uint32_t rtt_jitter_ms = 0;
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t packets_lost = 0;
    uint32_t packets_resent = 0;
    uint32_t send_errors = 0;
    uint32_t receive_errors = 0;
    uint32_t decode_errors = 0;
    uint32_t last_send_error = 0;
    uint32_t last_receive_error = 0;
    uint32_t local_input_delay_frames = 1;
};

// False in a build without OpenSSL: sessions must then start with enable_dtls off.
bool dtls_available();

class RollbackNetSession {
  public:
    RollbackNetSession();
    ~RollbackNetSession();

    RollbackNetSession(const RollbackNetSession&) = delete;
    RollbackNetSession& operator=(const RollbackNetSession&) = delete;

    bool start(const RollbackNetConfig& config, const std::string& remote_host, uint16_t remote_port,
               uint16_t local_port, const std::string& dtls_psk_identity,
               const std::string& dtls_psk_secret, bool is_server);
    void stop();

    void update(uint64_t now_ms);
    bool is_connected() const;

    // A reliable "I am ready" message and whether the other side's has come (for
    // two games to start something together, net/lockstep.h's sync point).
    // send_ready is false while not connected yet: call it again.
    bool send_ready();
    bool peer_ready() const;

    bool submit_local_input(uint32_t frame, std::span<const uint8_t> input);
    bool consume_remote_input(uint32_t frame, std::vector<uint8_t>& output);
    bool consume_remote_input_up_to(uint32_t frame, uint32_t max_behind_frames, std::vector<uint8_t>& output,
                                    uint32_t* source_frame = nullptr);
    bool consume_latest_remote_input(std::vector<uint8_t>& output, uint32_t* source_frame = nullptr);

    uint8_t recommended_input_delay_frames() const;
    const RollbackNetStats& stats() const;

  private:
    struct Impl;
    Impl* m_impl;
};

} // namespace cafe::net
