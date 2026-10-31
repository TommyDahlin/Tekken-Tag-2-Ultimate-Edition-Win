// Controllers: the Wii U GamePad (vpad) and up to four Pro Controllers
// (padscore's KPAD, channels 0-3); their states come from the host
// (set_gamepad_state, set_pro_controller_states, fed by the window layer).
// No Wii Remotes are connected. The title reads the other controllers
// through KPAD only (its calls of WPAD's own functions are in a library of
// the title's that nothing runs).

#include "kernel.h"

#include "cafe/export.h"
#include "cafe/input.h"
#include "cafe/vfs.h"
#include "net/lockstep.h"
#include "net/rollback_net.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cafe::os {
namespace {

std::mutex g_input_mutex;
GamepadState g_gamepad{};
uint32_t g_previous_hold = 0;
std::array<std::optional<GamepadState>, kProControllers> g_pro{};

// What the netplay makes of the other player's pad: while it is active, it is
// the Pro Controller on channel 0 (player 2), whatever the local pads say.
bool g_net_pro_active = false;
GamepadState g_net_pro{};

// A Pro Controller's reads. Once connected, it shows no buttons for
// kSettleReads reads, so that the title has seen it connect before it sees
// a press (a player joins with the press of + that connected the
// controller); what is pressed meanwhile is held for kHeldReads after.
constexpr int kSettleReads = 3;
struct ProReads {
    int connected = -1; // reads since it connected; -1: not connected
    uint32_t previous = 0, latched = 0;
    int latched_reads = 0;
};
std::array<ProReads, kProControllers> g_pro_reads{};

// set_input_step: reads to the next one the game sees change, buttons
// pressed meanwhile.
// It lasts a while unless set again (the mode sets it every frame; when its
// frames stop, the pad goes back to usual).
uint32_t g_step_every = 1, g_step_reads = 0, g_step_pressed = 0;
bool g_step_delivered = true;
std::chrono::steady_clock::time_point g_step_until{};
constexpr auto kStepLasts = std::chrono::milliseconds(250);

struct RollbackRuntime {
    std::once_flag init_once;
    bool enabled = false;
    bool use_remote_input = false;
    // The remote player drives the Pro Controller channel (player 2) instead of
    // being OR-ed into the GamePad. The server's pad is player 1 on both machines
    // and the client's is player 2, so the two games see the same assignment.
    bool remote_as_p2 = false;
    bool is_server = false;
    // Lockstep (net/lockstep.h): every poll of the pad waits for the other
    // player's input for it, so the two games always get the same two inputs.
    // Off after the other player has been silent too long.
    std::unique_ptr<net::Lockstep> lockstep;
    bool lockstep_off = false;
    bool lockstep_said_ready = false;
    uint32_t frame = 0;
    uint32_t last_remote_buttons = 0;
    uint32_t last_remote_frame = 0;
    // Local frame at which the last remote input arrived. The peer's frame counter
    // starts at its own boot, so its numbers can't be compared to ours for age.
    uint32_t last_remote_rx_frame = 0;
    uint32_t trace_counter = 0;
    std::optional<uint32_t> delay_override_frames {};
    net::RollbackNetSession session {};
};

RollbackRuntime& rollback_runtime() {
    static RollbackRuntime runtime;
    return runtime;
}

bool env_is_true(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return false;
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "on") == 0;
}

std::optional<uint32_t> decode_buttons(std::span<const uint8_t> payload) {
    if (payload.size() != sizeof(uint32_t)) return std::nullopt;
    const uint32_t value = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
                           (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
    return value;
}

// $XDG_CONFIG_HOME/ttt2/netplay.ini: "key = value" lines, # starts a comment.
// Keys: enabled, peer, port, local_port, role (host or join), player2, dtls,
// psk_id, psk_secret. The TTT2_NET_* variables override the file.
const std::map<std::string, std::string>& netplay_file() {
    static const std::map<std::string, std::string> settings = [] {
        std::map<std::string, std::string> out;
        std::filesystem::path file;
#ifdef _WIN32
        if (const char* data = std::getenv("LOCALAPPDATA"); data && *data)
            file = std::filesystem::path(data) / "TTT2Ultimate/config" / "netplay.ini";
#endif
        if (file.empty()) {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            const std::filesystem::path base = (xdg && *xdg == '/') ? std::filesystem::path(xdg)
                                               : std::filesystem::path(home && *home ? home : "/") / ".config";
            file = base / "ttt2" / "netplay.ini";
        }
        std::ifstream in(file);
        std::string line;
        auto trim = [](std::string s) {
            const char* space = " \t\r";
            s.erase(0, s.find_first_not_of(space));
            s.erase(s.find_last_not_of(space) + 1);
            return s;
        };
        while (std::getline(in, line)) {
            line = trim(line.substr(0, line.find('#')));
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return out;
    }();
    return settings;
}

std::optional<std::string> net_setting(const char* env, const char* key) {
    if (const char* value = std::getenv(env); value && *value) return std::string(value);
    const auto& file = netplay_file();
    if (const auto it = file.find(key); it != file.end() && !it->second.empty()) return it->second;
    return std::nullopt;
}

bool is_truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

bool net_flag(const char* env, const char* key, bool otherwise) {
    const auto value = net_setting(env, key);
    return value ? is_truthy(*value) : otherwise;
}

std::optional<uint16_t> net_port(const char* env, const char* key) {
    const auto value = net_setting(env, key);
    if (!value) return std::nullopt;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || *end != '\0' || parsed == 0 || parsed > 65535u) return std::nullopt;
    return static_cast<uint16_t>(parsed);
}

void initialize_rollback_runtime(RollbackRuntime& rt) {
    if (!net_flag("TTT2_NET_ROLLBACK", "enabled", false)) return;

    constexpr uint16_t kDefaultPort = 30000;
    const std::string peer = net_setting("TTT2_NET_ROLLBACK_PEER", "peer").value_or("");
    if (peer.empty()) {
        std::fprintf(stderr, "ttt2: rollback net: disabled (set peer in netplay.ini or TTT2_NET_ROLLBACK_PEER)\n");
        return;
    }
    // Both players use the same port by default, so each side knows the other's
    // (which a router's port forwarding or UDP hole punching needs).
    const uint16_t remote_port = net_port("TTT2_NET_ROLLBACK_PORT", "port").value_or(kDefaultPort);
    const uint16_t local_port = net_port("TTT2_NET_ROLLBACK_LOCAL_PORT", "local_port").value_or(remote_port);

    net::RollbackNetConfig config {};
    const char* profile = std::getenv("TTT2_NET_ROLLBACK_PROFILE");
    if (profile && (std::strcmp(profile, "enet") == 0 || std::strcmp(profile, "ENET") == 0)) {
        config.profile = net::TransportProfile::EnetDtls;
    }
    config.enable_dtls = env_is_true("TTT2_NET_ROLLBACK_NO_DTLS") ? false : net_flag("TTT2_NET_DTLS", "dtls", true);

    const bool is_server = env_is_true("TTT2_NET_ROLLBACK_SERVER") ||
                           net_setting("TTT2_NET_ROLE", "role").value_or("") == "host";
    const std::string identity = net_setting("TTT2_NET_ROLLBACK_PSK_ID", "psk_id").value_or("ttt2");
    const std::string secret = net_setting("TTT2_NET_ROLLBACK_PSK_SECRET", "psk_secret").value_or("ttt2-rollback");

    if (!rt.session.start(config, peer, remote_port, local_port, identity, secret, is_server)) {
        std::fprintf(stderr, "ttt2: rollback net: failed to start (%s:%u)\n", peer.c_str(),
                     static_cast<uint32_t>(remote_port));
        return;
    }

    rt.enabled = true;
    rt.remote_as_p2 = net_flag("TTT2_NET_REMOTE_AS_P2", "player2", false);
    rt.use_remote_input = rt.remote_as_p2 || env_is_true("TTT2_NET_USE_REMOTE_INPUT");
    rt.is_server = is_server;
    if (net_flag("TTT2_NET_LOCKSTEP", "lockstep", false)) {
        net::LockstepConfig lockstep_config;
        lockstep_config.is_host = is_server;
        if (const auto delay = net_setting("TTT2_NET_DELAY", "delay")) {
            const unsigned long parsed = std::strtoul(delay->c_str(), nullptr, 10);
            if (parsed >= 1 && parsed <= 30) lockstep_config.delay_frames = static_cast<uint32_t>(parsed);
        }
        rt.lockstep = std::make_unique<net::Lockstep>(rt.session, lockstep_config);
        // The host is player 1 and the guest player 2, on both machines.
        rt.remote_as_p2 = true;
        rt.use_remote_input = true;
        std::fprintf(stderr, "ttt2: lockstep: %s, input delay %u frames; the game waits for the other player's input\n",
                     is_server ? "host (player 1)" : "guest (player 2)", lockstep_config.delay_frames);
    }
    std::fprintf(stderr, "ttt2: rollback net: started (%s:%u, dtls %s, profile %s)%s%s\n", peer.c_str(),
                 static_cast<uint32_t>(remote_port), config.enable_dtls ? "on" : "off",
                 config.profile == net::TransportProfile::EnetDtls ? "enet" : "kcp",
                 rt.use_remote_input ? ", remote input applied" : "",
                 rt.remote_as_p2 ? (is_server ? " as player 2 (local is player 1)" : " as player 1 (local is player 2)")
                                 : "");
}

// A stick as the lockstep sends it (-127..127), so both machines get the same.
struct Sticks {
    int8_t lx = 0, ly = 0, rx = 0, ry = 0;
};

int8_t quantize_stick(float v) { return static_cast<int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }
float stick_value(int8_t v) { return static_cast<float>(v) / 127.0f; }

// What the two pads read this frame. `pro` is meaningful only when `pro_active`;
// the sticks only when `sticks_valid` (else the host's own pad is read).
struct RollbackPads {
    uint32_t vpad;
    uint32_t pro;
    bool pro_active;
    Sticks vpad_sticks = {};
    Sticks pro_sticks = {};
    bool sticks_valid = false;
};

bool skip_intro_finished(); // below

// `physical_hold` is the buttons of the real pad alone (no script, no skip-intro taps).
RollbackPads apply_rollback_input(uint32_t local_hold, const Sticks& local_sticks, uint32_t physical_hold) {
    RollbackRuntime& rt = rollback_runtime();
    std::call_once(rt.init_once, [&] { initialize_rollback_runtime(rt); });
    if (!rt.enabled) return {local_hold, 0, false};

    if (rt.lockstep && !rt.lockstep_off) {
        if (!rt.lockstep->started()) {
            // Until both games are at the sync point each runs on its own pad, with
            // the P2 pad already plugged in (so it does not appear in the menu). A
            // game is at the sync point when its player presses START (which goes on to
            // the game as well: the press takes effect on both machines together, once
            // both have pressed it) or when the skip-intro time is over.
            static const bool sync_now = net_setting("TTT2_NET_SYNC", "sync").value_or("menu") == "now";
            const bool ready = sync_now || skip_intro_finished() || (physical_hold & vpad::kPlus) != 0;
            const uint32_t shown = local_hold;
            if (ready && !rt.lockstep_said_ready) {
                rt.lockstep_said_ready = true;
                std::fprintf(stderr, "ttt2: lockstep: this game is at the sync point\n");
            }
            if (!rt.lockstep->sync_point(ready)) return {shown, 0, true};
            std::fprintf(stderr, "ttt2: lockstep: both players are at the sync point; lockstep starts now\n");
        }
        ++rt.frame;
        const net::PadSample mine{local_hold, local_sticks.lx, local_sticks.ly, local_sticks.rx, local_sticks.ry};
        const auto frame = rt.lockstep->step(mine);
        if (!frame) {
            std::fprintf(stderr, "ttt2: lockstep: the other player has been silent too long; playing on alone\n");
            rt.lockstep_off = true;
            return {local_hold, 0, false};
        }
        rt.delay_override_frames = std::nullopt;
        static const bool trace = env_is_true("TTT2_NET_TRACE_INPUT");
        if (trace && rt.lockstep->poll() % 600 == 0) {
            std::fprintf(stderr, "ttt2: lockstep: poll %u, %u polls waited for the other player (longest %u ms)\n",
                         rt.lockstep->poll(), rt.lockstep->stalls(), rt.lockstep->longest_wait_ms());
        }
        const auto sticks_of = [](const net::PadSample& s) { return Sticks{s.left_x, s.left_y, s.right_x, s.right_y}; };
        return {frame->host.buttons, frame->guest.buttons, true, sticks_of(frame->host), sticks_of(frame->guest), true};
    }

    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    rt.session.update(now_ms);
    const net::RollbackNetStats& net_stats = rt.session.stats();

    ++rt.frame;
    const std::array<uint8_t, 4> payload{
        static_cast<uint8_t>(local_hold & 0xFF),
        static_cast<uint8_t>((local_hold >> 8) & 0xFF),
        static_cast<uint8_t>((local_hold >> 16) & 0xFF),
        static_cast<uint8_t>((local_hold >> 24) & 0xFF),
    };
    (void)rt.session.submit_local_input(rt.frame, payload);

    constexpr uint32_t kRemoteHoldFrames = 4;

    std::vector<uint8_t> remote;
    uint32_t source_frame = 0;
    bool got_remote = false;
    if (rt.session.consume_latest_remote_input(remote, &source_frame)) {
        if (const auto decoded = decode_buttons(remote)) {
            rt.last_remote_buttons = *decoded;
            rt.last_remote_frame = source_frame;
            rt.last_remote_rx_frame = rt.frame;
            got_remote = true;
        }
    }

    if (rt.session.is_connected()) {
        rt.delay_override_frames = static_cast<uint32_t>(rt.session.recommended_input_delay_frames());
    } else {
        rt.delay_override_frames = std::nullopt;
    }

    static const bool trace_input = env_is_true("TTT2_NET_TRACE_INPUT");
    ++rt.trace_counter;
    if (trace_input && (rt.trace_counter % 120 == 0 || got_remote)) {
        std::fprintf(stderr,
                     "ttt2: rollback trace: frame=%u local=0x%08X got_remote=%s remote_last=0x%08X "
                     "remote_frame=%u tx=%u rx=%u lost=%u resent=%u txerr=%u rxerr=%u decerr=%u serr=%u rerr=%u ping=%ums jitter=%ums\n",
                     rt.frame, local_hold, got_remote ? "yes" : "no", rt.last_remote_buttons, rt.last_remote_frame,
                     net_stats.packets_sent, net_stats.packets_received, net_stats.packets_lost,
                     net_stats.packets_resent, net_stats.send_errors, net_stats.receive_errors,
                     net_stats.decode_errors, net_stats.last_send_error, net_stats.last_receive_error,
                     net_stats.ping_ms, net_stats.rtt_jitter_ms);
    }

    if (!rt.use_remote_input) return {local_hold, 0, false};
    const uint32_t remote_age = rt.frame - rt.last_remote_rx_frame;
    const bool fresh = rt.last_remote_rx_frame != 0 && remote_age <= kRemoteHoldFrames;
    const uint32_t remote_buttons = fresh ? rt.last_remote_buttons : 0;

    if (rt.remote_as_p2) {
        if (trace_input && (got_remote || local_hold != 0 || remote_buttons != 0)) {
            std::fprintf(stderr, "ttt2: rollback input: frame=%u local=0x%08X remote=0x%08X age=%u%s\n", rt.frame,
                         local_hold, remote_buttons, remote_age, got_remote ? " (fresh)" : "");
        }
        return rt.is_server ? RollbackPads{local_hold, remote_buttons, true}
                            : RollbackPads{remote_buttons, local_hold, true};
    }

    if (!fresh) return {local_hold, 0, false};
    const uint32_t merged = local_hold | remote_buttons;
    if (trace_input && (got_remote || local_hold != 0 || remote_buttons != 0)) {
        std::fprintf(stderr, "ttt2: rollback input: frame=%u local=0x%08X remote=0x%08X age=%u merged=0x%08X%s\n",
                     rt.frame, local_hold, remote_buttons, remote_age, merged, got_remote ? " (fresh)" : "");
    }
    return {merged, 0, false};
}

// TTT2_INPUT_SCRIPT="15:plus,20.5:a,22:down" presses each button at the given
// second after start-up, for unattended runs; "30:plus#2" presses it on the
// second controller (2-5: Pro Controllers 1-4, connected for the whole run
// once the script names them; 1 is the GamePad): for a fifth of a second and at
// least kHeldReads reads of the pad, then released for kReleasedReads reads
// before the same button is pressed again, so a press is seen even when a frame
// stalls (loading). An item "@3dbb1771" waits until the title reads the archive
// entry with that name hash (the character select screen's layout, say): the
// times after it count from then, and presses before it that have not come yet
// are dropped, so a script does not drift with loading times.
constexpr int kHeldReads = 4;
constexpr int kReleasedReads = 4;

struct ScriptedPress {
    double at;
    uint32_t button;
    size_t segment; // the anchors before it
    int port;       // 0: the GamePad, 1-4: a Pro Controller's channel + 1
    int reads = -1; // reads of the pad while held; -1: not pressed yet
};

struct Script {
    std::vector<ScriptedPress> presses;
    std::vector<uint32_t> anchors;
    std::mutex mutex;
    std::vector<double> starts; // each segment's start, after start-up; < 0 while waiting
    std::vector<std::pair<uint64_t, int>> released; // port << 32 | button -> reads since its last scripted release
    std::array<bool, kProControllers> pro{};          // Pro Controllers the script uses
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

Script& load_script() {
    static Script script;
    const char* text_env = std::getenv("TTT2_INPUT_SCRIPT");
    if (text_env == nullptr) return script;
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"a", vpad::kA}, {"b", vpad::kB}, {"x", vpad::kX}, {"y", vpad::kY}, {"left", vpad::kLeft},
        {"right", vpad::kRight}, {"up", vpad::kUp}, {"down", vpad::kDown}, {"zl", vpad::kZL}, {"zr", vpad::kZR},
        {"l", vpad::kL}, {"r", vpad::kR}, {"plus", vpad::kPlus}, {"minus", vpad::kMinus}, {"home", vpad::kHome},
    };
    std::string text(text_env);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find(',', pos) == std::string::npos ? text.size() : text.find(',', pos);
        const std::string item = text.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (!item.empty() && item[0] == '@') {
            script.anchors.push_back(static_cast<uint32_t>(std::strtoul(item.c_str() + 1, nullptr, 16)));
        } else if (colon != std::string::npos) {
            std::string name = item.substr(colon + 1);
            int port = 0;
            if (const size_t hash = name.find('#'); hash != std::string::npos) {
                port = std::clamp(std::atoi(name.c_str() + hash + 1), 1, kProControllers + 1) - 1;
                name.resize(hash);
            }
            if (port > 0) script.pro[port - 1] = true;
            for (const auto& [n, bit] : kNames) {
                if (name == n) {
                    script.presses.push_back(
                        {std::atof(item.substr(0, colon).c_str()), bit, script.anchors.size(), port});
                }
            }
        }
        pos = end + 1;
    }
    script.starts.assign(script.anchors.size() + 1, -1.0);
    script.starts[0] = 0.0;
    if (!script.anchors.empty()) {
        vfs::watch_entry_reads([&s = script](uint32_t hash) {
            std::lock_guard lock(s.mutex);
            for (size_t k = 0; k < s.anchors.size(); ++k) {
                if (s.starts[k + 1] >= 0.0) continue;
                if (s.starts[k] >= 0.0 && s.anchors[k] == hash) s.starts[k + 1] = seconds_since(s.start);
                break;
            }
        });
    }
    return script;
}

Script& script() {
    static Script& script = load_script();
    return script;
}

// The buttons the script holds on a port (0: the GamePad, 1-4: a Pro
// Controller's channel + 1), for a read of it.
uint32_t scripted_buttons(int port) {
    Script& script = cafe::os::script();
    if (script.presses.empty()) return 0;
    const double t = seconds_since(script.start);
    std::lock_guard lock(script.mutex);
    auto released_reads = [&](uint32_t button) -> int& {
        const uint64_t key = uint64_t(port) << 32 | button;
        for (auto& [b, n] : script.released) {
            if (b == key) return n;
        }
        script.released.push_back({key, kReleasedReads});
        return script.released.back().second;
    };
    uint32_t buttons = 0;
    for (ScriptedPress& p : script.presses) {
        if (p.port != port || p.reads >= kHeldReads) continue; // another pad's, or done
        const double start = script.starts[p.segment];
        const bool next_reached = p.segment + 1 < script.starts.size() && script.starts[p.segment + 1] >= 0.0;
        if (p.reads < 0) {
            if (start < 0.0 || next_reached || t < start + p.at) continue;
            if (released_reads(p.button) < kReleasedReads || (buttons & p.button)) continue; // wait for a release
            p.reads = 0;
        }
        buttons |= p.button;
        if (++p.reads >= kHeldReads && t < start + p.at + 0.2) p.reads = kHeldReads - 1; // hold the fifth second
        if (p.reads >= kHeldReads) released_reads(p.button) = -1;
    }
    for (auto& [b, n] : script.released) {
        if (b >> 32 == uint64_t(port) && !(buttons & uint32_t(b)) && n < kReleasedReads) ++n;
    }
    return buttons;
}

// TTT2_SKIP_INTRO=<seconds> taps A and + (Start), alternately, for that many
// seconds after start-up, which skips the logos and the intro movie and gets
// through the title screen's "press start". It does not know where the intro
// ends: a time that runs on into the menus presses whatever is selected there.
double skip_intro_seconds() {
    static const double seconds = [] {
        const char* value = std::getenv("TTT2_SKIP_INTRO");
        return value && *value ? std::atof(value) : 0.0;
    }();
    return seconds;
}

// Seconds since the first poll of the pad, from which the skip counts.
double since_first_poll() {
    static const auto start = std::chrono::steady_clock::now();
    return seconds_since(start);
}

bool skip_intro_finished() {
    const double seconds = skip_intro_seconds();
    return seconds > 0.0 && since_first_poll() > seconds + 0.3;
}

uint32_t skip_intro_buttons() {
    const double seconds = skip_intro_seconds();
    if (seconds <= 0.0) return 0;
    const double t = since_first_poll();
    if (t > seconds) return 0;
    const double phase = std::fmod(t, 0.5);
    if (phase < 0.12) return vpad::kA;
    if (phase >= 0.25 && phase < 0.37) return vpad::kPlus;
    return 0;
}

// ------------------------------------------------------------ the - chord
// See arm_select_chord. A press of - waits kChordFrames armings (the mode's
// frames, so slow frames wait longer): the mode sees a button two or three
// frames after the pad has it, and the other button may come a little after
// -. A press of - let go meanwhile still reaches the game, late, for
// kHeldReads reads.
using Clock = std::chrono::steady_clock;
constexpr uint32_t kChordFrames = 8;                         // how many armings a press of - waits for the partner
constexpr auto kChordArmed = std::chrono::milliseconds(250); // how long an arming lasts (frames may be slow)
constexpr auto kChordTaken = std::chrono::milliseconds(500); // how long a chord waits to be taken

struct SelectChord {
    enum Gate { kOpen, kWaiting, kChord } gate = kOpen;
    Clock::time_point armed_until{}, chord_at{};
    uint32_t armings = 0, pressed_armings = 0; // armings so far, and at the press of -
    bool partner = false;
    bool raw = false;     // - held, as the host has it
    bool chord = false;   // a chord not taken yet
    int late_reads = 0;
};
SelectChord g_chord;

uint32_t gate_select(uint32_t hold) {
    SelectChord& c = g_chord;
    const Clock::time_point now = Clock::now();
    const bool raw = hold & vpad::kMinus, pressed = raw && !c.raw, armed = now < c.armed_until;
    c.raw = raw;
    auto chord = [&] {
        c.gate = SelectChord::kChord;
        c.chord = true;
        c.chord_at = now;
    };
    if (pressed) {
        if (!armed) c.gate = SelectChord::kOpen;
        else if (c.partner) chord();
        else {
            c.gate = SelectChord::kWaiting;
            c.pressed_armings = c.armings;
        }
    } else if (c.gate == SelectChord::kWaiting) {
        if (armed && c.partner) {
            chord();
        } else if (!armed || c.armings - c.pressed_armings >= kChordFrames) {
            c.gate = SelectChord::kOpen;
            if (!raw) c.late_reads = kHeldReads;
        }
    }
    switch (c.gate) {
    case SelectChord::kWaiting: return hold & ~vpad::kMinus;
    case SelectChord::kChord:
        if (!raw) c.gate = SelectChord::kOpen;
        return hold & ~vpad::kMinus;
    case SelectChord::kOpen: break;
    }
    if (c.late_reads > 0) {
        --c.late_reads;
        return hold | vpad::kMinus;
    }
    return hold;
}

// ------------------------------------------------------------------- vpad
constexpr int32_t kVpadOk = 0;
constexpr int32_t kVpadNoController = -2;

GamepadState movement_state(GamepadState state) {
    // Keep DP and LS movement consistent. Resolve opposite digital
    // directions to neutral on that axis; digital input takes precedence
    // over analog drift.
    const auto digital_axis = [&](uint32_t negative, uint32_t positive, float analog) {
        const bool neg = state.buttons & negative, pos = state.buttons & positive;
        if (!neg && !pos) return std::clamp(analog, -1.0f, 1.0f);
        if (neg && pos) state.buttons &= ~(negative | positive);
        return float(pos) - float(neg);
    };
    state.left_x = digital_axis(vpad::kLeft, vpad::kRight, state.left_x);
    state.left_y = digital_axis(vpad::kDown, vpad::kUp, state.left_y);
    state.right_x = std::clamp(state.right_x, -1.0f, 1.0f);
    state.right_y = std::clamp(state.right_y, -1.0f, 1.0f);

    // Wii U reports stick directions in hold/trigger/release too. Sending
    // only axes leaves LS-mode controllers without these digital inputs.
    constexpr uint32_t directions = vpad::kStickLLeft | vpad::kStickLRight | vpad::kStickLUp | vpad::kStickLDown |
                                    vpad::kStickRLeft | vpad::kStickRRight | vpad::kStickRUp | vpad::kStickRDown;
    state.buttons &= ~directions;
    const auto stick_axis = [&](float value, uint32_t negative, uint32_t positive) {
        if (value <= -0.5f) state.buttons |= negative;
        if (value >= 0.5f) state.buttons |= positive;
    };
    stick_axis(state.left_x, vpad::kStickLLeft, vpad::kStickLRight);
    stick_axis(state.left_y, vpad::kStickLDown, vpad::kStickLUp);
    stick_axis(state.right_x, vpad::kStickRLeft, vpad::kStickRRight);
    stick_axis(state.right_y, vpad::kStickRDown, vpad::kStickRUp);
    // Combat's sub_03F52F20 matches the D-pad masks, not the VPAD stick
    // emulation flags. An LS-mode leverless therefore needs these bits too.
    stick_axis(state.left_x, vpad::kLeft, vpad::kRight);
    stick_axis(state.left_y, vpad::kDown, vpad::kUp);
    return state;
}

struct Vec2 { be<float> x, y; };
struct Vec3 { be<float> x, y, z; };
struct TouchData { be<uint16_t> x, y, touched, validity; };
struct VPADStatus {
    be<uint32_t> hold, trigger, release;
    Vec2 left_stick, right_stick;
    Vec3 acceleration;
    be<float> acc_magnitude, acc_variation;
    Vec2 acc_xy;
    Vec3 gyro, angle;
    int8_t error;
    uint8_t pad1;
    TouchData touch, touch_processed1, touch_processed2;
    uint8_t pad2[2];
    Vec3 direction[3];
    uint8_t headphone;
    uint8_t pad3[3];
    Vec3 magnet;
    uint8_t volume, battery, mic, volume_ex;
    uint8_t pad4[8];
};
static_assert(sizeof(VPADStatus) == 0xAC);

void VPADInit() {}

int32_t VPADRead(int32_t channel, VPADStatus* status, uint32_t count, be<int32_t>* error) {
    if (channel != 0 || count == 0 || status == nullptr) {
        if (error) *error = kVpadNoController;
        return 0;
    }
    GamepadState state;
    uint32_t local_hold = 0;
    uint32_t physical_hold = 0;
    {
        std::lock_guard lock(g_input_mutex);
        state = g_gamepad;
        physical_hold = state.buttons;
        state.buttons |= scripted_buttons(0) | skip_intro_buttons();
        state = movement_state(state);
        local_hold = gate_select(state.buttons);
    }
    const Sticks local_sticks{quantize_stick(state.left_x), quantize_stick(state.left_y),
                              quantize_stick(state.right_x), quantize_stick(state.right_y)};
    // Not under the lock: a lockstep poll waits for the other player, and the
    // window thread feeds the pad (set_gamepad_state) under it meanwhile.
    const RollbackPads pads = apply_rollback_input(local_hold, local_sticks, physical_hold);
    std::lock_guard lock(g_input_mutex);
    std::memset(status, 0, sizeof(VPADStatus));
    g_net_pro_active = pads.pro_active;
    g_net_pro = {pads.pro, stick_value(pads.pro_sticks.lx), stick_value(pads.pro_sticks.ly),
                 stick_value(pads.pro_sticks.rx), stick_value(pads.pro_sticks.ry)};
    uint32_t hold = pads.vpad;
    if (g_step_every > 1 && std::chrono::steady_clock::now() > g_step_until) g_step_every = 1;
    g_step_delivered = g_step_every <= 1 || ++g_step_reads % g_step_every == 0;
    if (!g_step_delivered) {
        g_step_pressed |= hold;
        hold = g_previous_hold;
    } else {
        hold |= g_step_pressed;
        g_step_pressed = 0;
    }
    status->hold = hold;
    status->trigger = hold & ~g_previous_hold;
    status->release = g_previous_hold & ~hold;
    g_previous_hold = hold;
    if (pads.sticks_valid) {
        status->left_stick = {stick_value(pads.vpad_sticks.lx), stick_value(pads.vpad_sticks.ly)};
        status->right_stick = {stick_value(pads.vpad_sticks.rx), stick_value(pads.vpad_sticks.ry)};
    } else {
        status->left_stick = {state.left_x, state.left_y};
        status->right_stick = {state.right_x, state.right_y};
    }
    // A GamePad lying flat and still: gravity on -Z (in g), identity frame.
    status->acceleration = {0.0f, 0.0f, -1.0f};
    status->acc_magnitude = 1.0f;
    status->direction[0] = {1.0f, 0.0f, 0.0f};
    status->direction[1] = {0.0f, 1.0f, 0.0f};
    status->direction[2] = {0.0f, 0.0f, 1.0f};
    status->battery = 0xC0;
    status->volume = 0xFF;
    status->volume_ex = 0xFF;
    if (error) *error = kVpadOk;
    return 1;
}

void VPADSetBtnRepeat(int32_t, float, float) {}
void VPADSetAccParam(int32_t, float, float) {}
int32_t VPADControlMotor(int32_t, uint32_t, uint32_t) { return 0; }
void VPADStopMotor(int32_t) {}

// Touch coordinates come in raw; calibrated output is the 1280x720 screen.
void VPADGetTPCalibratedPoint(int32_t, TouchData* out, const TouchData* in) {
    if (out && in) *out = *in;
}

// --------------------------------------------------------------- padscore
constexpr int32_t kWpadNoController = -1;

uint32_t g_wpad_alloc = 0, g_wpad_free = 0;
uint32_t g_connect_callbacks[7]{};

void WPADRegisterAllocator(uint32_t alloc, uint32_t free) {
    g_wpad_alloc = alloc;
    g_wpad_free = free;
}
void KPADInit() {}
void KPADShutdown() {}
uint32_t KPADSetConnectCallback(uint32_t channel, uint32_t callback) {
    if (channel >= 7) return 0;
    const uint32_t previous = g_connect_callbacks[channel];
    g_connect_callbacks[channel] = callback;
    return previous;
}
// KPADStatus: the Wii Remote's own part (left zero: no Wii Remote), then
// the extension's; a Pro Controller's buttons by WPAD's bits for it.
struct KPADStatus {
    be<uint32_t> hold, trigger, release;
    uint8_t remote[0x50];
    uint8_t extension; // its kind: 0x1F a Pro Controller
    int8_t error;
    uint8_t pos_valid;
    uint8_t format; // WPAD's data format: 22 a Pro Controller's
    be<uint32_t> pro_hold, pro_trigger, pro_release;
    Vec2 left_stick, right_stick;
    be<int32_t> charging, wired;
    uint8_t rest[0x6C];
};
static_assert(sizeof(KPADStatus) == 0xF0 && offsetof(KPADStatus, extension) == 0x5C &&
              offsetof(KPADStatus, pro_hold) == 0x60 && offsetof(KPADStatus, left_stick) == 0x6C);
constexpr int32_t kKpadOk = 0;
constexpr int32_t kKpadNoController = -2;
constexpr uint8_t kExtensionPro = 0x1F;
constexpr uint8_t kFormatPro = 22;

// The GamePad's buttons as a Pro Controller's (WPAD_PRO_*).
uint32_t pro_buttons(uint32_t vpad_buttons) {
    static constexpr std::pair<uint32_t, uint32_t> kBits[] = {
        {vpad::kUp, 0x0001},     {vpad::kLeft, 0x0002},   {vpad::kZR, 0x0004},     {vpad::kX, 0x0008},
        {vpad::kA, 0x0010},      {vpad::kY, 0x0020},      {vpad::kB, 0x0040},      {vpad::kZL, 0x0080},
        {vpad::kR, 0x0200},      {vpad::kPlus, 0x0400},   {vpad::kHome, 0x0800},   {vpad::kMinus, 0x1000},
        {vpad::kL, 0x2000},      {vpad::kDown, 0x4000},   {vpad::kRight, 0x8000},  {vpad::kStickR, 0x10000},
        {vpad::kStickL, 0x20000},
    };
    uint32_t pro = 0;
    for (const auto& [from, to] : kBits) {
        if (vpad_buttons & from) pro |= to;
    }
    return pro;
}

int32_t KPADReadEx(int32_t channel, KPADStatus* status, uint32_t count, be<int32_t>* error) {
    std::lock_guard lock(g_input_mutex);
    const bool valid = channel >= 0 && channel < kProControllers;
    const bool scripted = valid && script().pro[channel];
    const bool net = channel == 0 && g_net_pro_active;
    if (!valid || !(g_pro[channel] || scripted || net) || status == nullptr || count == 0) {
        if (valid) g_pro_reads[channel] = {};
        if (error) *error = kKpadNoController;
        return 0;
    }
    // (Movement as on the GamePad: each player's D-pad and stick agree the same way.)
    GamepadState pad = net ? g_net_pro : g_pro[channel].value_or(GamepadState{});
    pad.buttons |= scripted_buttons(channel + 1);
    pad = movement_state(pad);
    ProReads& r = g_pro_reads[channel];
    uint32_t hold = pro_buttons(pad.buttons);
    const bool settled = r.connected >= kSettleReads;
    if (!settled) {
        r.latched |= hold;
        hold = 0;
        if (++r.connected == kSettleReads && r.latched) r.latched_reads = kHeldReads;
    } else if (r.latched_reads > 0) {
        hold |= r.latched;
        if (--r.latched_reads == 0) r.latched = 0;
    }
    std::memset(status, 0, sizeof(KPADStatus));
    status->extension = kExtensionPro;
    status->format = kFormatPro;
    status->pro_hold = hold;
    status->pro_trigger = hold & ~r.previous;
    status->pro_release = r.previous & ~hold;
    r.previous = hold;
    if (settled) {
        status->left_stick = {pad.left_x, pad.left_y};
        status->right_stick = {pad.right_x, pad.right_y};
    }
    status->wired = 1;
    if (error) *error = kKpadOk;
    return 1;
}

int32_t WPADProbe(int32_t, be<uint32_t>* type) {
    if (type) *type = 0xFD; // no device
    return kWpadNoController;
}
void WPADRead(int32_t, uint8_t* status) {
    if (status) std::memset(status, 0, 0x2A);
}
int32_t WPADSetDataFormat(int32_t, uint32_t) { return kWpadNoController; }
uint32_t WPADGetLatestIndexInBuf(int32_t) { return 0; }
void WPADSetAutoSamplingBuf(int32_t, uint32_t, uint32_t) {}
void WPADControlMotor(int32_t, uint32_t) {}
bool WPADIsMotorEnabled() { return true; }
int32_t WPADEnableURCC(int32_t) { return 0; }
void WPADDisconnect(int32_t) {}
int32_t WPADControlDpd(int32_t, uint32_t, uint32_t) { return kWpadNoController; }
void KPADEnableDPD(int32_t) {}
void KPADDisableDPD(int32_t) {}
void WPADClampStick(int32_t, uint32_t, uint32_t) {}
void WPADClampTrigger(int32_t, uint32_t, uint32_t) {}

} // namespace

void set_gamepad_state(const GamepadState& state) {
    std::lock_guard lock(g_input_mutex);
    g_gamepad = state;
}

void set_pro_controller_states(const std::array<std::optional<GamepadState>, kProControllers>& states) {
    std::lock_guard lock(g_input_mutex);
    g_pro = states;
}

void arm_select_chord(bool partner_held) {
    std::lock_guard lock(g_input_mutex);
    g_chord.armed_until = Clock::now() + kChordArmed;
    g_chord.partner = partner_held;
    ++g_chord.armings;
}

void set_input_step(uint32_t every) {
    std::lock_guard lock(g_input_mutex);
    g_step_until = std::chrono::steady_clock::now() + kStepLasts;
    if (every == g_step_every) return;
    g_step_every = std::max(every, 1u);
    g_step_reads = 0;
    if (g_step_every == 1) g_step_delivered = true;
}

bool input_step_delivered() {
    std::lock_guard lock(g_input_mutex);
    return g_step_delivered;
}

bool take_select_chord() {
    std::lock_guard lock(g_input_mutex);
    const bool chord = g_chord.chord && Clock::now() - g_chord.chord_at < kChordTaken;
    g_chord.chord = false;
    return chord;
}

std::optional<uint32_t> rollback_input_delay_frames() {
    RollbackRuntime& rt = rollback_runtime();
    std::call_once(rt.init_once, [&] { initialize_rollback_runtime(rt); });
    return rt.delay_override_frames;
}

CAFE_EXPORT(vpad, VPADInit, VPADInit);
CAFE_EXPORT(vpad, VPADRead, VPADRead);
CAFE_EXPORT(vpad, VPADSetBtnRepeat, VPADSetBtnRepeat);
CAFE_EXPORT(vpad, VPADSetAccParam, VPADSetAccParam);
CAFE_EXPORT(vpad, VPADControlMotor, VPADControlMotor);
CAFE_EXPORT(vpad, VPADStopMotor, VPADStopMotor);
CAFE_EXPORT(vpad, VPADGetTPCalibratedPoint, VPADGetTPCalibratedPoint);

CAFE_EXPORT(padscore, WPADRegisterAllocator, WPADRegisterAllocator);
CAFE_EXPORT(padscore, KPADInit, KPADInit);
CAFE_EXPORT(padscore, KPADShutdown, KPADShutdown);
CAFE_EXPORT(padscore, KPADSetConnectCallback, KPADSetConnectCallback);
CAFE_EXPORT(padscore, KPADReadEx, KPADReadEx);
CAFE_EXPORT(padscore, WPADProbe, WPADProbe);
CAFE_EXPORT(padscore, WPADRead, WPADRead);
CAFE_EXPORT(padscore, WPADSetDataFormat, WPADSetDataFormat);
CAFE_EXPORT(padscore, WPADGetLatestIndexInBuf, WPADGetLatestIndexInBuf);
CAFE_EXPORT(padscore, WPADSetAutoSamplingBuf, WPADSetAutoSamplingBuf);
CAFE_EXPORT(padscore, WPADControlMotor, WPADControlMotor);
CAFE_EXPORT(padscore, WPADIsMotorEnabled, WPADIsMotorEnabled);
CAFE_EXPORT(padscore, WPADEnableURCC, WPADEnableURCC);
CAFE_EXPORT(padscore, WPADDisconnect, WPADDisconnect);
CAFE_EXPORT(padscore, WPADControlDpd, WPADControlDpd);
CAFE_EXPORT(padscore, KPADEnableDPD, KPADEnableDPD);
CAFE_EXPORT(padscore, KPADDisableDPD, KPADDisableDPD);
CAFE_EXPORT(padscore, WPADClampStick, WPADClampStick);
CAFE_EXPORT(padscore, WPADClampTrigger, WPADClampTrigger);

} // namespace cafe::os
