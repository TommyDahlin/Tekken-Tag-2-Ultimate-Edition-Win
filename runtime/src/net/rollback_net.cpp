#include "net/rollback_net.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <cstdio>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <openssl/ssl.h>
#include <openssl/err.h>

#if defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace cafe::net {
namespace {

constexpr uint32_t kMagic = 0x52524E31; // RRN1
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr uint32_t kMaxPacketSize = 1400;
constexpr uint8_t kPacketInput = 1;
constexpr uint8_t kPacketHeartbeat = 2;
constexpr uint8_t kPacketReady = 3;
constexpr size_t kMaxPending = 256; // unacknowledged packets kept for resending

struct PacketHeader {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint8_t type = 0;
    uint8_t reserved0 = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;
    uint32_t ack_bits = 0;
    uint32_t frame = 0;
    uint16_t payload_size = 0;
    uint16_t reserved1 = 0;
    uint32_t sent_ms = 0;
};

struct PendingPacket {
    uint32_t seq = 0;
    std::vector<uint8_t> encoded;
    uint64_t first_send_ms = 0;
    uint64_t last_send_ms = 0;
    uint32_t sends = 1; // times sent so far: the resends back off (12, 24, ... 192 ms)
};

uint16_t host_to_net16(uint16_t v) {
#if defined(_WIN32)
    return htons(v);
#else
    return htons(v);
#endif
}

uint32_t host_to_net32(uint32_t v) {
#if defined(_WIN32)
    return htonl(v);
#else
    return htonl(v);
#endif
}

uint16_t net_to_host16(uint16_t v) {
#if defined(_WIN32)
    return ntohs(v);
#else
    return ntohs(v);
#endif
}

uint32_t net_to_host32(uint32_t v) {
#if defined(_WIN32)
    return ntohl(v);
#else
    return ntohl(v);
#endif
}

void close_socket(SocketHandle s) {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

bool set_nonblocking(SocketHandle s) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void disable_udp_connreset(SocketHandle s) {
#if defined(_WIN32)
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    (void)WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior), nullptr, 0, &bytes_returned,
                   nullptr, nullptr);
#else
    (void)s;
#endif
}

#if defined(_WIN32)
bool ensure_winsock_initialized() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data {};
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    });
    return ok;
}
#endif

uint32_t last_socket_error_code() {
#if defined(_WIN32)
    return static_cast<uint32_t>(WSAGetLastError());
#else
    return static_cast<uint32_t>(errno);
#endif
}

std::optional<sockaddr_storage> resolve_udp_endpoint(const std::string& host, uint16_t port) {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const std::string port_str = std::to_string(port);
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) return std::nullopt;

    sockaddr_storage out {};
    std::memcpy(&out, res->ai_addr, static_cast<size_t>(res->ai_addrlen));
    freeaddrinfo(res);
    return out;
}

socklen_t sockaddr_len(const sockaddr_storage& storage) {
    if (storage.ss_family == AF_INET) return static_cast<socklen_t>(sizeof(sockaddr_in));
    if (storage.ss_family == AF_INET6) return static_cast<socklen_t>(sizeof(sockaddr_in6));
    return static_cast<socklen_t>(sizeof(sockaddr_storage));
}

bool sequence_newer(uint32_t a, uint32_t b) {
    return (a != b) && static_cast<uint32_t>(a - b) < 0x80000000u;
}

std::vector<uint8_t> encode_packet(const PacketHeader& in_header, std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    out.resize(kHeaderSize + payload.size());

    const uint32_t magic = host_to_net32(in_header.magic);
    const uint16_t version = host_to_net16(in_header.version);
    const uint32_t seq = host_to_net32(in_header.seq);
    const uint32_t ack = host_to_net32(in_header.ack);
    const uint32_t ack_bits = host_to_net32(in_header.ack_bits);
    const uint32_t frame = host_to_net32(in_header.frame);
    const uint16_t payload_size = host_to_net16(in_header.payload_size);
    const uint32_t sent_ms = host_to_net32(in_header.sent_ms);

    size_t off = 0;
    std::memcpy(out.data() + off, &magic, sizeof(magic));
    off += sizeof(magic);
    std::memcpy(out.data() + off, &version, sizeof(version));
    off += sizeof(version);
    out[off++] = in_header.type;
    out[off++] = in_header.reserved0;
    std::memcpy(out.data() + off, &seq, sizeof(seq));
    off += sizeof(seq);
    std::memcpy(out.data() + off, &ack, sizeof(ack));
    off += sizeof(ack);
    std::memcpy(out.data() + off, &ack_bits, sizeof(ack_bits));
    off += sizeof(ack_bits);
    std::memcpy(out.data() + off, &frame, sizeof(frame));
    off += sizeof(frame);
    std::memcpy(out.data() + off, &payload_size, sizeof(payload_size));
    off += sizeof(payload_size);
    std::memcpy(out.data() + off, &in_header.reserved1, sizeof(in_header.reserved1));
    off += sizeof(in_header.reserved1);
    std::memcpy(out.data() + off, &sent_ms, sizeof(sent_ms));
    off += sizeof(sent_ms);

    if (!payload.empty()) std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
    return out;
}

bool decode_packet(std::span<const uint8_t> packet, PacketHeader& out_header, std::span<const uint8_t>& out_payload) {
    if (packet.size() < kHeaderSize) return false;

    size_t off = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;
    uint32_t ack_bits = 0;
    uint32_t frame = 0;
    uint16_t payload_size = 0;
    uint16_t reserved1 = 0;
    uint32_t sent_ms = 0;

    std::memcpy(&magic, packet.data() + off, sizeof(magic));
    off += sizeof(magic);
    std::memcpy(&version, packet.data() + off, sizeof(version));
    off += sizeof(version);
    out_header.type = packet[off++];
    out_header.reserved0 = packet[off++];
    std::memcpy(&seq, packet.data() + off, sizeof(seq));
    off += sizeof(seq);
    std::memcpy(&ack, packet.data() + off, sizeof(ack));
    off += sizeof(ack);
    std::memcpy(&ack_bits, packet.data() + off, sizeof(ack_bits));
    off += sizeof(ack_bits);
    std::memcpy(&frame, packet.data() + off, sizeof(frame));
    off += sizeof(frame);
    std::memcpy(&payload_size, packet.data() + off, sizeof(payload_size));
    off += sizeof(payload_size);
    std::memcpy(&reserved1, packet.data() + off, sizeof(reserved1));
    off += sizeof(reserved1);
    std::memcpy(&sent_ms, packet.data() + off, sizeof(sent_ms));
    off += sizeof(sent_ms);

    out_header.magic = net_to_host32(magic);
    out_header.version = net_to_host16(version);
    out_header.seq = net_to_host32(seq);
    out_header.ack = net_to_host32(ack);
    out_header.ack_bits = net_to_host32(ack_bits);
    out_header.frame = net_to_host32(frame);
    out_header.payload_size = net_to_host16(payload_size);
    out_header.reserved1 = reserved1;
    out_header.sent_ms = net_to_host32(sent_ms);

    if (out_header.magic != kMagic || out_header.version != kVersion) return false;
    if (packet.size() < kHeaderSize + out_header.payload_size) return false;

    out_payload = packet.subspan(kHeaderSize, out_header.payload_size);
    return true;
}

class DtlsState {
  public:
    ~DtlsState() { reset(); }

    bool init(bool is_server, SocketHandle socket_fd, const sockaddr_storage& peer,
              std::span<const uint8_t> psk_identity, std::span<const uint8_t> psk_secret) {
        m_is_server = is_server;
        m_psk_identity.assign(psk_identity.begin(), psk_identity.end());
        m_psk_secret.assign(psk_secret.begin(), psk_secret.end());

        const SSL_METHOD* method = DTLS_method();
        m_ctx = SSL_CTX_new(method);
        if (!m_ctx) return false;
        SSL_CTX_set_min_proto_version(m_ctx, DTLS1_2_VERSION);
        SSL_CTX_set_cipher_list(m_ctx, "PSK-AES128-GCM-SHA256:PSK-AES128-CCM8");
        SSL_CTX_set_options(m_ctx, SSL_OP_NO_QUERY_MTU);
        SSL_CTX_set_read_ahead(m_ctx, 1);

        SSL_CTX_set_app_data(m_ctx, this);
        SSL_CTX_set_psk_client_callback(
            m_ctx, [](SSL* ssl, const char*, char* identity, unsigned int max_identity_len, unsigned char* psk,
                      unsigned int max_psk_len) -> unsigned int {
                SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
                auto* self = static_cast<DtlsState*>(SSL_CTX_get_app_data(ssl_ctx));
                if (!self) return 0;
                if (self->m_psk_identity.empty() || self->m_psk_secret.empty()) return 0;
                const size_t id_len =
                    std::min(static_cast<size_t>(max_identity_len - 1), self->m_psk_identity.size());
                std::memcpy(identity, self->m_psk_identity.data(), id_len);
                identity[id_len] = '\0';
                const size_t key_len = std::min(static_cast<size_t>(max_psk_len), self->m_psk_secret.size());
                std::memcpy(psk, self->m_psk_secret.data(), key_len);
                return static_cast<unsigned int>(key_len);
            });

        SSL_CTX_set_psk_server_callback(
            m_ctx, [](SSL* ssl, const char* identity, unsigned char* psk, unsigned int max_psk_len) -> unsigned int {
                SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
                auto* self = static_cast<DtlsState*>(SSL_CTX_get_app_data(ssl_ctx));
                if (!self || !identity) return 0;
                if (self->m_psk_identity.empty() || self->m_psk_secret.empty()) return 0;

                const std::string incoming(identity);
                const std::string expected(self->m_psk_identity.begin(), self->m_psk_identity.end());
                if (incoming != expected) return 0;
                const size_t key_len = std::min(static_cast<size_t>(max_psk_len), self->m_psk_secret.size());
                std::memcpy(psk, self->m_psk_secret.data(), key_len);
                return static_cast<unsigned int>(key_len);
            });

        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) return false;
        // With SSL_OP_NO_QUERY_MTU OpenSSL never asks the socket, and fails every
        // write (with no error to report) until an MTU is set.
        SSL_set_mtu(m_ssl, 1200);

        BIO* bio = BIO_new_dgram(socket_fd, BIO_NOCLOSE);
        if (!bio) return false;
        // The socket is connect()ed: without the peer the BIO sendto()s to nowhere.
        if (peer.ss_family == AF_INET) {
            const auto& in = reinterpret_cast<const sockaddr_in&>(peer);
            BIO_ADDR* addr = BIO_ADDR_new();
            if (!addr || BIO_ADDR_rawmake(addr, AF_INET, &in.sin_addr, sizeof(in.sin_addr), in.sin_port) != 1) {
                BIO_ADDR_free(addr);
                BIO_free(bio);
                return false;
            }
            BIO_ctrl_set_connected(bio, addr);
            BIO_ADDR_free(addr);
        }
        SSL_set_bio(m_ssl, bio, bio);
        if (is_server) {
            SSL_set_accept_state(m_ssl);
        } else {
            SSL_set_connect_state(m_ssl);
        }
        return true;
    }

    bool do_handshake() {
        if (!m_ssl) return false;
        const int rc = SSL_do_handshake(m_ssl);
        if (rc == 1) {
            m_established = true;
            return true;
        }
        const int err = SSL_get_error(m_ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // Retransmit lost handshake flights (a no-op until the timer expires).
            DTLSv1_handle_timeout(m_ssl);
            return false;
        }
        if (!m_error_logged) {
            m_error_logged = true;
            char text[256] = {};
            ERR_error_string_n(ERR_get_error(), text, sizeof(text));
            std::fprintf(stderr, "ttt2: rollback net: DTLS handshake failed as %s (ssl error %d: %s, socket error %u)\n",
                         m_is_server ? "server" : "client", err, text, last_socket_error_code());
        }
        return false;
    }

    bool established() const { return m_established; }

    int read(uint8_t* out, int max_len) {
        if (!m_ssl) return -1;
        return SSL_read(m_ssl, out, max_len);
    }

    int write(const uint8_t* data, int len) {
        if (!m_ssl) return -1;
        return SSL_write(m_ssl, data, len);
    }

    bool last_error_would_block(int rc) const {
        if (!m_ssl) return false;
        const int err = SSL_get_error(m_ssl, rc);
        return err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
    }

    void reset() {
        if (m_ssl) {
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        if (m_ctx) {
            SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
        m_psk_identity.clear();
        m_psk_secret.clear();
        m_established = false;
        m_error_logged = false;
    }

  private:
    SSL_CTX* m_ctx = nullptr;
    SSL* m_ssl = nullptr;
    bool m_is_server = false;
    bool m_established = false;
    bool m_error_logged = false;
    std::vector<uint8_t> m_psk_identity;
    std::vector<uint8_t> m_psk_secret;
};

} // namespace

struct RollbackNetSession::Impl {
    RollbackNetConfig config {};
    RollbackNetStats stats {};

    SocketHandle socket_fd = kInvalidSocket;
    sockaddr_storage remote_addr {};
    socklen_t remote_addr_len = 0;
    bool connected = false;
    bool started = false;
    bool is_server = false;
    bool use_connected_socket = false;
    bool use_dtls = false;

    DtlsState dtls {};
    bool dtls_ready = false;

    uint32_t local_seq = 1;
    uint32_t remote_latest_seq = 0;
    uint32_t remote_recv_mask = 0;
    uint64_t last_send_ms = 0;
    uint64_t last_receive_ms = 0;

    std::unordered_map<uint32_t, PendingPacket> pending {};
    std::unordered_map<uint32_t, std::vector<uint8_t>> remote_inputs {};
    uint32_t consumed_frame = 0; // the newest input frame taken: older ones that arrive again are dropped
    bool peer_ready = false;

    bool open_udp(const std::string& host, uint16_t remote_port, uint16_t local_port, bool connected_mode) {
#if defined(_WIN32)
        if (!ensure_winsock_initialized()) {
            std::fprintf(stderr, "ttt2: rollback net: WSAStartup failed\n");
            return false;
        }
#endif

        auto endpoint = resolve_udp_endpoint(host, remote_port);
        if (!endpoint.has_value()) {
            std::fprintf(stderr, "ttt2: rollback net: resolve failed for %s:%u\n", host.c_str(),
                         static_cast<unsigned>(remote_port));
            return false;
        }

        remote_addr = endpoint.value();
        remote_addr_len = sockaddr_len(remote_addr);

        socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_fd == kInvalidSocket) {
            std::fprintf(stderr, "ttt2: rollback net: socket() failed\n");
            return false;
        }

        disable_udp_connreset(socket_fd);
        use_connected_socket = connected_mode;

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(local_port);
        if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            std::fprintf(stderr, "ttt2: rollback net: bind() failed on local port %u\n", static_cast<unsigned>(local_port));
            close_socket(socket_fd);
            socket_fd = kInvalidSocket;
            return false;
        }

        if (connected_mode) {
            if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&remote_addr), remote_addr_len) != 0) {
                std::fprintf(stderr, "ttt2: rollback net: connect() failed to %s:%u\n", host.c_str(),
                             static_cast<unsigned>(remote_port));
                close_socket(socket_fd);
                socket_fd = kInvalidSocket;
                return false;
            }
        }

        if (!set_nonblocking(socket_fd)) {
            std::fprintf(stderr, "ttt2: rollback net: non-blocking setup failed\n");
            close_socket(socket_fd);
            socket_fd = kInvalidSocket;
            return false;
        }

        std::fprintf(stderr, "ttt2: rollback net: udp local_port=%u remote=%s:%u mode=%s\n",
                     static_cast<unsigned>(local_port), host.c_str(), static_cast<unsigned>(remote_port),
                     connected_mode ? "connected" : "sendto/recvfrom");
        return true;
    }

    void close_all() {
        pending.clear();
        remote_inputs.clear();
        peer_ready = false;
        consumed_frame = 0;
        dtls.reset();
        dtls_ready = false;
        use_dtls = false;
        connected = false;
        started = false;
        if (socket_fd != kInvalidSocket) {
            close_socket(socket_fd);
            socket_fd = kInvalidSocket;
        }
    }

    void on_ack(uint32_t ack, uint32_t ack_bits, uint64_t now_ms) {
        auto erase_if_acked = [&](uint32_t seq) {
            auto it = pending.find(seq);
            if (it == pending.end()) return;
            const uint64_t one_way = now_ms - it->second.first_send_ms;
            const uint32_t rtt_sample = static_cast<uint32_t>(std::min<uint64_t>(one_way * 2, 2000));
            if (stats.ping_ms == 0) {
                stats.ping_ms = rtt_sample;
            } else {
                const uint32_t prev = stats.ping_ms;
                stats.ping_ms = (prev * 7 + rtt_sample) / 8;
                const uint32_t abs_delta = (prev > rtt_sample) ? (prev - rtt_sample) : (rtt_sample - prev);
                stats.rtt_jitter_ms = (stats.rtt_jitter_ms * 3 + abs_delta) / 4;
            }
            pending.erase(it);
        };

        erase_if_acked(ack);
        for (uint32_t i = 0; i < 32; ++i) {
            if ((ack_bits & (1u << i)) == 0) continue;
            erase_if_acked(ack - (i + 1u));
        }
    }

    bool is_packet_new(uint32_t seq) {
        if (remote_latest_seq == 0) {
            remote_latest_seq = seq;
            remote_recv_mask = 0;
            return true;
        }

        if (sequence_newer(seq, remote_latest_seq)) {
            const uint32_t delta = seq - remote_latest_seq;
            if (delta >= 32) {
                remote_recv_mask = 0;
            } else {
                remote_recv_mask = (remote_recv_mask << delta) | (1u << (delta - 1u));
            }
            remote_latest_seq = seq;
            return true;
        }

        const uint32_t delta = remote_latest_seq - seq;
        if (delta == 0 || delta > 32) return false;
        const uint32_t bit = 1u << (delta - 1u);
        if ((remote_recv_mask & bit) != 0) return false;
        remote_recv_mask |= bit;
        return true;
    }

    void send_packet(uint8_t type, uint32_t frame, std::span<const uint8_t> payload, uint64_t now_ms,
                     bool reliable) {
        PacketHeader h {};
        h.type = type;
        h.seq = (type == kPacketHeartbeat) ? 0 : local_seq++; // heartbeats are not reliable: no sequence number
        h.ack = remote_latest_seq;
        h.ack_bits = remote_recv_mask;
        h.frame = frame;
        h.payload_size = static_cast<uint16_t>(payload.size());
        h.sent_ms = static_cast<uint32_t>(now_ms & std::numeric_limits<uint32_t>::max());

        auto packet = encode_packet(h, payload);
        bool sent_ok = false;
        if (use_dtls) {
            sent_ok = dtls.write(packet.data(), static_cast<int>(packet.size())) > 0;
        } else if (use_connected_socket) {
            sent_ok =
                send(socket_fd, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0) > 0;
        } else {
            sent_ok = sendto(socket_fd, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                             0, reinterpret_cast<const sockaddr*>(&remote_addr), remote_addr_len) > 0;
        }

        if (sent_ok) {
            stats.packets_sent++;
            last_send_ms = now_ms;
            stats.last_send_error = 0;
        } else {
            stats.send_errors++;
            stats.last_send_error = last_socket_error_code();
        }

        if (reliable && sent_ok) {
            PendingPacket p {};
            p.seq = h.seq;
            p.encoded = std::move(packet);
            p.first_send_ms = now_ms;
            p.last_send_ms = now_ms;
            pending[p.seq] = std::move(p);
            // A peer that never answers must not make this grow (and be resent
            // every few milliseconds) for ever: an input this old is of no use.
            if (pending.size() > kMaxPending) {
                auto oldest = pending.begin();
                for (auto it = pending.begin(); it != pending.end(); ++it) {
                    if (sequence_newer(oldest->first, it->first)) oldest = it;
                }
                pending.erase(oldest);
            }
        }
    }

    void process_incoming(uint64_t now_ms) {
        std::array<uint8_t, kMaxPacketSize> buffer {};
        for (;;) {
            int rc = 0;
            if (use_dtls) {
                rc = dtls.read(buffer.data(), static_cast<int>(buffer.size()));
                if (rc <= 0 && dtls.last_error_would_block(rc)) break;
            } else {
                if (use_connected_socket) {
                    rc = static_cast<int>(
                        recv(socket_fd, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0));
                } else {
                    sockaddr_storage from {};
                    socklen_t from_len = static_cast<socklen_t>(sizeof(from));
                    rc = static_cast<int>(recvfrom(socket_fd, reinterpret_cast<char*>(buffer.data()),
                                                   static_cast<int>(buffer.size()), 0,
                                                   reinterpret_cast<sockaddr*>(&from), &from_len));
                    if (rc > 0 && from.ss_family == AF_INET && remote_addr.ss_family == AF_INET) {
                        const auto* f = reinterpret_cast<const sockaddr_in*>(&from);
                        const auto* r = reinterpret_cast<const sockaddr_in*>(&remote_addr);
                        if (f->sin_addr.s_addr != r->sin_addr.s_addr || f->sin_port != r->sin_port) {
                            continue;
                        }
                    }
                }
                if (rc <= 0) {
#if defined(_WIN32)
                    const int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) break;
                    if (err == WSAECONNRESET || err == WSAENETRESET) continue;
#else
                    // Connected UDP on Linux surfaces ICMP port-unreachable as ECONNREFUSED
                    // while the peer is not up yet; treat it like Windows' CONNRESET.
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR || errno == ECONNREFUSED) continue;
#endif
                    stats.receive_errors++;
                    stats.last_receive_error = last_socket_error_code();
                    break;
                }
            }

            if (rc <= 0) break;
            stats.packets_received++;
            last_receive_ms = now_ms;
            connected = true; // a peer that went quiet (loading, say) and came back

            PacketHeader h {};
            std::span<const uint8_t> payload {};
            if (!decode_packet(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(rc)), h, payload)) {
                stats.decode_errors++;
                continue;
            }
            on_ack(h.ack, h.ack_bits, now_ms);

            if (h.type == kPacketHeartbeat) continue; // only its acks mattered
            // Inputs are told apart by their frame, not by the sequence window (which
            // is 32 packets wide): a peer that started its game seconds late must
            // still accept the inputs sent before it was there. The sequence still
            // feeds the acks that stop the sender's resends.
            (void)is_packet_new(h.seq);
            if (h.type == kPacketReady) {
                peer_ready = true;
                continue;
            }
            if (h.type == kPacketInput && h.frame > consumed_frame && remote_inputs.count(h.frame) == 0) {
                remote_inputs[h.frame] = std::vector<uint8_t>(payload.begin(), payload.end());
            }
        }
    }

    void resend_expired(uint64_t now_ms) {
        for (auto& [seq, packet] : pending) {
            // 12 ms, then 24, 48, 96 and 192: a peer that is not there yet is not flooded.
            const uint64_t interval = uint64_t{config.resend_interval_ms} << std::min<uint32_t>(packet.sends - 1, 4);
            if (now_ms - packet.last_send_ms < interval) continue;
            bool resent_ok = false;
            if (use_dtls) {
                resent_ok = dtls.write(packet.encoded.data(), static_cast<int>(packet.encoded.size())) > 0;
            } else if (use_connected_socket) {
                resent_ok = send(socket_fd, reinterpret_cast<const char*>(packet.encoded.data()),
                                 static_cast<int>(packet.encoded.size()), 0) > 0;
            } else {
                resent_ok = sendto(socket_fd, reinterpret_cast<const char*>(packet.encoded.data()),
                                   static_cast<int>(packet.encoded.size()), 0,
                                   reinterpret_cast<const sockaddr*>(&remote_addr), remote_addr_len) > 0;
            }
            if (resent_ok) {
                packet.last_send_ms = now_ms;
                ++packet.sends;
                stats.packets_resent++;
                stats.last_send_error = 0;
            } else {
                stats.send_errors++;
                stats.last_send_error = last_socket_error_code();
            }
        }
    }

    void recompute_delay() {
        const uint32_t fast_budget = (config.profile == TransportProfile::KcpDtls) ? 18u : 24u;
        const uint32_t total = stats.ping_ms + stats.rtt_jitter_ms;
        const uint32_t dynamic = std::clamp(total / fast_budget, 0u, 4u);
        stats.local_input_delay_frames = static_cast<uint32_t>(config.base_input_delay_frames) + dynamic;
    }
};

RollbackNetSession::RollbackNetSession() : m_impl(new Impl()) {}

RollbackNetSession::~RollbackNetSession() {
    if (m_impl) {
        m_impl->close_all();
        delete m_impl;
        m_impl = nullptr;
    }
}

bool RollbackNetSession::start(const RollbackNetConfig& config, const std::string& remote_host,
                               uint16_t remote_port, uint16_t local_port,
                               const std::string& dtls_psk_identity, const std::string& dtls_psk_secret,
                               bool is_server) {
    if (!m_impl) return false;
    m_impl->close_all();
    m_impl->config = config;
    m_impl->stats.local_input_delay_frames = std::max<uint8_t>(1, config.base_input_delay_frames);
    m_impl->is_server = is_server;

    if (!m_impl->open_udp(remote_host, remote_port, local_port, true)) return false;

    m_impl->started = true;
    m_impl->use_dtls = config.enable_dtls;
    if (!config.enable_dtls) {
        const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count());
        m_impl->dtls_ready = true;
        m_impl->connected = true;
        m_impl->last_send_ms = now;
        return true;
    }

    if (!m_impl->dtls.init(is_server, m_impl->socket_fd, m_impl->remote_addr,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(dtls_psk_identity.data()),
                                                    dtls_psk_identity.size()),
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(dtls_psk_secret.data()),
                                                    dtls_psk_secret.size()))) {
        m_impl->close_all();
        return false;
    }
    return true;
}

void RollbackNetSession::stop() {
    if (!m_impl) return;
    m_impl->close_all();
}

void RollbackNetSession::update(uint64_t now_ms) {
    if (!m_impl || !m_impl->started) return;

    if (!m_impl->dtls_ready) {
        if (m_impl->dtls.do_handshake()) {
            m_impl->dtls_ready = true;
            m_impl->connected = true;
            m_impl->last_receive_ms = now_ms;
            m_impl->last_send_ms = now_ms;
        } else {
            return;
        }
    }

    m_impl->process_incoming(now_ms);
    m_impl->resend_expired(now_ms);

    if (now_ms - m_impl->last_send_ms >= m_impl->config.heartbeat_interval_ms) {
        const std::array<uint8_t, 0> none {};
        m_impl->send_packet(kPacketHeartbeat, 0, std::span<const uint8_t>(none.data(), none.size()), now_ms, false);
    }

    if (m_impl->last_receive_ms != 0 && now_ms - m_impl->last_receive_ms > m_impl->config.timeout_ms) {
        m_impl->connected = false;
    }

    m_impl->stats.packets_lost = static_cast<uint32_t>(m_impl->pending.size());
    m_impl->recompute_delay();
}

bool RollbackNetSession::is_connected() const {
    return m_impl && m_impl->connected;
}

bool RollbackNetSession::send_ready() {
    if (!m_impl || !m_impl->connected) return false;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const std::array<uint8_t, 0> none {};
    m_impl->send_packet(kPacketReady, 0, std::span<const uint8_t>(none.data(), none.size()), now, true);
    return true;
}

bool RollbackNetSession::peer_ready() const {
    return m_impl && m_impl->peer_ready;
}

bool RollbackNetSession::submit_local_input(uint32_t frame, std::span<const uint8_t> input) {
    if (!m_impl || !m_impl->connected) return false;
    if (input.size() > m_impl->config.max_payload_bytes) return false;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    m_impl->send_packet(kPacketInput, frame, input, now, true);
    return true;
}

bool RollbackNetSession::consume_remote_input(uint32_t frame, std::vector<uint8_t>& output) {
    if (!m_impl) return false;
    auto it = m_impl->remote_inputs.find(frame);
    if (it == m_impl->remote_inputs.end()) return false;
    output = std::move(it->second);
    m_impl->remote_inputs.erase(it);
    m_impl->consumed_frame = std::max(m_impl->consumed_frame, frame);
    return true;
}

bool RollbackNetSession::consume_remote_input_up_to(uint32_t frame, uint32_t max_behind_frames,
                                                    std::vector<uint8_t>& output, uint32_t* source_frame) {
    if (!m_impl) return false;

    const uint32_t min_frame = (frame > max_behind_frames) ? (frame - max_behind_frames) : 0;
    auto best = m_impl->remote_inputs.end();
    for (auto it = m_impl->remote_inputs.begin(); it != m_impl->remote_inputs.end(); ++it) {
        const uint32_t candidate = it->first;
        if (candidate > frame || candidate < min_frame) continue;
        if (best == m_impl->remote_inputs.end() || candidate > best->first) best = it;
    }
    if (best == m_impl->remote_inputs.end()) return false;

    if (source_frame) *source_frame = best->first;
    output = std::move(best->second);
    const uint32_t consumed_frame = best->first;
    m_impl->consumed_frame = std::max(m_impl->consumed_frame, consumed_frame);
    m_impl->remote_inputs.erase(best);

    for (auto it = m_impl->remote_inputs.begin(); it != m_impl->remote_inputs.end();) {
        if (it->first <= consumed_frame) {
            it = m_impl->remote_inputs.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RollbackNetSession::consume_latest_remote_input(std::vector<uint8_t>& output, uint32_t* source_frame) {
    if (!m_impl || m_impl->remote_inputs.empty()) return false;
    auto best = m_impl->remote_inputs.begin();
    for (auto it = m_impl->remote_inputs.begin(); it != m_impl->remote_inputs.end(); ++it) {
        if (sequence_newer(it->first, best->first)) best = it;
    }
    if (source_frame) *source_frame = best->first;
    output = std::move(best->second);
    const uint32_t consumed_frame = best->first;
    m_impl->consumed_frame = std::max(m_impl->consumed_frame, consumed_frame);
    m_impl->remote_inputs.erase(best);

    for (auto it = m_impl->remote_inputs.begin(); it != m_impl->remote_inputs.end();) {
        if (!sequence_newer(it->first, consumed_frame) || it->first == consumed_frame) {
            it = m_impl->remote_inputs.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

uint8_t RollbackNetSession::recommended_input_delay_frames() const {
    if (!m_impl) return 1;
    return static_cast<uint8_t>(std::clamp<uint32_t>(m_impl->stats.local_input_delay_frames, 1, 8));
}

const RollbackNetStats& RollbackNetSession::stats() const {
    static RollbackNetStats fallback {};
    if (!m_impl) return fallback;
    return m_impl->stats;
}

} // namespace cafe::net
