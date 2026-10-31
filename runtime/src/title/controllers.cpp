#include "title/controllers.h"

#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "cafe/vfs.h"
#include "gpu/replacements.h"
#include "host/window.h"
#include "title/image.h"
#include "title/pictures.h"
#include "title/presentation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cafe::title {
namespace {

// ------------------------------------------------------------------ the modes

// VS Battle, Team Battle and Pair Play start on their player entry screen,
// and the players keep their controllers until the menus are back. Every
// offline mode loads the screen's layouts (data007 794FFCD9 and FA2AD1D0)
// as it starts, but only those show it: VS Battle inside its mode (the
// mode manager's kind 9, as practice.cpp finds the manager), Team Battle
// and Pair Play before theirs (14 and 0) start, with no mode meanwhile.
// The others start theirs at once (Arcade 0, Survival 10, Time Attack 11,
// Ghost Battle 12; Practice 4 and 5), and no one has joined: a mode that
// starts with no one joined ends it. The menus are kind 6.
constexpr uint32_t kEntryLayouts[] = {0x794FFCD9, 0xFA2AD1D0};
constexpr uint32_t kMatchObject = 0x10872CB8; // *(*(*it + 228) + 228) = the mode manager
constexpr uint32_t kModeKind = 444, kMenus = 6, kVsBattle = 9;

std::atomic<bool> g_menus_seen{false}; // (the layouts are read at start-up too)
std::atomic<bool> g_entry_opened{false};

// The mode manager's kind; none between modes.
std::optional<uint32_t> mode_kind() {
    const uint8_t* base = guest_base();
    const uint32_t object = ppc::ld32(base, kMatchObject);
    const uint32_t match = object ? ppc::ld32(base, object + 228) : 0;
    const uint32_t mode = match ? ppc::ld32(base, match + 228) : 0;
    std::optional<uint32_t> kind;
    if (mode) kind = ppc::ld32(base, mode + kModeKind);
    static const bool trace = std::getenv("TTT2_TRACE_PLAYERS") != nullptr;
    static std::optional<uint32_t> traced = 0xFFFFFFFF;
    if (trace && kind != traced) {
        traced = kind;
        if (kind) std::fprintf(stderr, "ttt2: players: mode kind %u\n", *kind);
        else std::fprintf(stderr, "ttt2: players: no mode\n");
    }
    return kind;
}

// On the window's thread.
bool players_mode(bool joined) {
    enum class Session { kNone, kOpening, kOn }; // kOpening: the menus' mode may not have gone yet
    static Session session = Session::kNone;
    if (g_entry_opened.exchange(false)) session = Session::kOpening;
    const std::optional<uint32_t> kind = mode_kind();
    if (kind == kMenus) {
        g_menus_seen = true;
        if (session == Session::kOn) session = Session::kNone;
        return session != Session::kNone;
    }
    if (session == Session::kOpening) session = Session::kOn;
    if (session == Session::kOn && kind && *kind != kVsBattle && !joined) session = Session::kNone;
    return session != Session::kNone;
}

// ------------------------------------------------------------------ the pictures

// The entry screen's layouts (data007 794FFCD9, FA2AD1D0: alike, but for
// some pages' texts) keep their pictures in pages of their texture sets.
constexpr const char* kLayoutArchive = "content/hdd/data007";
constexpr uint32_t kTextureSets[] = {0xFB833313, 0x82DCEE9A};

enum Variant { kWhite, kRed, kCyan, kSmall };
constexpr int kVariants = 4;

// Where a controller's pictures are: page, and the top left corner.
struct Cell {
    uint32_t page, x, y;
};
constexpr Cell kGamePadCells[kVariants] = {{12, 0, 292}, {15, 0, 220}, {17, 0, 220}, {15, 320, 220}};
constexpr Cell kProCells[kVariants] = {{13, 0, 292}, {16, 0, 0}, {18, 0, 0}, {15, 320, 324}};

uint32_t cell_width(int variant) { return variant == kSmall ? pictures::kSmallWidth : pictures::kWidth; }
uint32_t cell_height(int variant) { return variant == kSmall ? pictures::kSmallHeight : pictures::kHeight; }

// What a controller's pictures show: a device drawn here, the title's Pro
// Controller, or the title's own pictures.
enum class Shown { kOwn, kKeyboard, kPlayStation, kXbox, kPro };

Shown shown(const std::optional<host::Device>& device, bool pro) {
    if (!device) return Shown::kOwn;
    switch (*device) {
    case host::Device::kKeyboard: return Shown::kKeyboard;
    case host::Device::kPlayStation: return Shown::kPlayStation;
    case host::Device::kXbox: return Shown::kXbox;
    case host::Device::kNintendo: break;
    }
    return pro ? Shown::kOwn : Shown::kPro;
}

// The drawn devices' pictures, by variant (made beside the title; ready
// once g_drawn_ready).
std::array<std::array<Rgba, kVariants>, 3> g_drawn;
std::atomic<bool> g_drawn_ready{false};

const Rgba* drawn(Shown s, int variant) {
    const int k = s == Shown::kKeyboard ? 0 : s == Shown::kPlayStation ? 1 : 2;
    return &g_drawn[size_t(k)][size_t(variant)];
}

// A page of the texture sets, as the title has it, and the pages with the
// pictures changed, by what they show (kept: the renderer keeps pointers).
struct Page {
    uint32_t number;
    Rgba original;
    std::map<std::pair<Shown, Shown>, std::vector<uint8_t>> made;
};
std::deque<Page> g_pages;
std::vector<Rgba> g_pro_originals; // the title's Pro Controller pictures, by variant
std::mutex g_pages_mutex;

const std::vector<uint8_t>* page_now(Page& page) {
    if (!g_drawn_ready) return nullptr;
    const std::array<std::optional<host::Device>, host::kControllers> devices = host::controller_devices();
    std::optional<host::Device> pro;
    for (size_t c = 1; c < devices.size() && !pro; ++c) pro = devices[c];
    const std::pair key{shown(devices[0], false), shown(pro, true)};
    bool changes = false;
    for (int v = 0; v < kVariants; ++v) {
        changes = changes || (kGamePadCells[v].page == page.number && key.first != Shown::kOwn) ||
                  (kProCells[v].page == page.number && key.second != Shown::kOwn);
    }
    if (!changes) return nullptr;
    std::lock_guard lock(g_pages_mutex);
    if (const auto made = page.made.find(key); made != page.made.end()) return &made->second;
    Rgba with = page.original;
    for (int v = 0; v < kVariants; ++v) {
        for (const auto& [cell, s] : {std::pair{kGamePadCells[v], key.first}, std::pair{kProCells[v], key.second}}) {
            if (cell.page != page.number || s == Shown::kOwn) continue;
            const Rgba* picture = s == Shown::kPro ? &g_pro_originals[size_t(v)] : drawn(s, v);
            put(with, blank(cell_width(v), cell_height(v)), cell.x, cell.y);
            put(with, *picture, cell.x, cell.y);
        }
    }
    return &page.made.emplace(key, std::move(with.pixels)).first->second;
}

bool holds_cells(uint32_t page) {
    for (int v = 0; v < kVariants; ++v) {
        if (kGamePadCells[v].page == page || kProCells[v].page == page) return true;
    }
    return false;
}

void prepare_pictures() {
    std::vector<std::vector<uint8_t>> seen; // pages the sets share are replaced once
    for (const uint32_t set : kTextureSets) {
        const auto file = game_file(kLayoutArchive, set);
        const auto textures = file ? ntp3::parse(*file) : std::nullopt;
        if (!textures || textures->size() <= kProCells[kCyan].page) {
            std::fprintf(stderr, "ttt2: warning: the VS screens' pictures are not where expected; they show Wii U controllers\n");
            return;
        }
        for (uint32_t n = 0; n < textures->size(); ++n) {
            const ntp3::Texture& t = (*textures)[n];
            if (!holds_cells(n) || std::find(seen.begin(), seen.end(), t.data) != seen.end()) continue;
            auto decoded = ntp3::decode(t);
            if (!decoded || decoded->width != 512 || decoded->height != 512) continue;
            seen.push_back(t.data);
            if (g_pro_originals.empty()) g_pro_originals.resize(kVariants);
            for (int v = 0; v < kVariants; ++v) {
                if (kProCells[v].page == n && g_pro_originals[size_t(v)].pixels.empty()) {
                    g_pro_originals[size_t(v)] = crop(*decoded, kProCells[v].x, kProCells[v].y, cell_width(v), cell_height(v));
                }
            }
            g_pages.push_back({n, std::move(*decoded), {}});
            Page* page = &g_pages.back();
            replace_texture(t, [page] { return page_now(*page); });
        }
    }
    host::on_controller_devices_change([] { gpu::texture_replacements_changed(); });
    // Drawing takes some tens of milliseconds: beside the title's start.
    std::thread([] {
        for (int k = 0; k < 3; ++k) {
            const Rgba white = pictures::white(static_cast<pictures::Device>(k));
            g_drawn[size_t(k)] = {white, pictures::lit(white, pictures::kRedGlow, pictures::kRedTint),
                                  pictures::lit(white, pictures::kCyanGlow, pictures::kCyanTint), pictures::small(white)};
        }
        g_drawn_ready = true;
        gpu::texture_replacements_changed();
    }).detach();
}

} // namespace

void prepare_controllers() {
    prepare_pictures();
    host::on_own_controllers(players_mode);
    vfs::watch_entry_reads([](uint32_t hash) {
        if (g_menus_seen && std::find(std::begin(kEntryLayouts), std::end(kEntryLayouts), hash) != std::end(kEntryLayouts)) {
            g_entry_opened = true;
        }
    });
}

} // namespace cafe::title
