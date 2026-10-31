// See rollback_selftest.h.

#include "title/rollback_selftest.h"

#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "os/kernel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ttt2::title {
namespace {

using namespace cafe;

constexpr uint32_t kPage = 0x1000;

struct Range {
    uint32_t begin;
    uint32_t end;
};

// A copy of the pages of the ranges, in order (a page that was not committed
// is zeros, with `present` 0).
struct Image {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> present;
};

// One flag per page of the ranges.
using Mask = std::vector<uint8_t>;

enum class Mode : int { Off, Record, Replay };

struct Push {
    uint32_t side;
    uint32_t buttons;
};

struct SelfTest {
    bool enabled = false;
    std::vector<uint32_t> starts{180}; // battle frames to run the test at, ascending
    size_t next_start = 0;
    Mask changed; // the pages any run's frames changed
    uint32_t frames = 2;
    uint32_t calls = 0;
    std::vector<Range> ranges;
    std::vector<uint32_t> pages; // the address of each page of the ranges
    std::atomic<Mode> mode{Mode::Off};
    std::mutex mutex;
    std::vector<Push> pushes;
    size_t next_push = 0;
    uint32_t mismatches = 0;
    std::thread::id thread; // the one running the frames
};

uint32_t parse_hex(const std::string& text) { return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16)); }

std::vector<Range> parse_ranges(const char* text) {
    std::vector<Range> out;
    std::string rest(text);
    size_t pos = 0;
    while (pos < rest.size()) {
        const size_t comma = std::min(rest.find(',', pos), rest.size());
        const std::string item = rest.substr(pos, comma - pos);
        const size_t dash = item.find('-');
        if (dash != std::string::npos) out.push_back({parse_hex(item.substr(0, dash)), parse_hex(item.substr(dash + 1))});
        pos = comma + 1;
    }
    return out;
}

uint32_t env_u32(const char* name, uint32_t otherwise) {
    const char* value = std::getenv(name);
    if (!value || !*value) return otherwise;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed == 0 ? otherwise : static_cast<uint32_t>(parsed);
}

SelfTest& state() {
    static SelfTest test;
    static const bool configured = [] {
        const char* on = std::getenv("TTT2_ROLLBACK_SELFTEST");
        test.enabled = on && std::strcmp(on, "1") == 0;
        if (const char* list = std::getenv("TTT2_ROLLBACK_SELFTEST_FRAME"); list && *list) {
            test.starts.clear();
            const char* at = list;
            while (*at) {
                char* end = nullptr;
                const unsigned long frame = std::strtoul(at, &end, 10);
                if (end == at) break;
                if (frame != 0) test.starts.push_back(static_cast<uint32_t>(frame));
                if (*end != ',') break;
                at = end + 1;
            }
            std::sort(test.starts.begin(), test.starts.end());
        }
        test.frames = env_u32("TTT2_ROLLBACK_SELFTEST_K", 2);
        const char* ranges = std::getenv("TTT2_ROLLBACK_SELFTEST_RANGES");
        test.ranges =
            parse_ranges(ranges && *ranges ? ranges : "10000000-16000000,30000000-32000000");
        for (const Range& range : test.ranges) {
            for (uint32_t address = range.begin; address < range.end; address += kPage) test.pages.push_back(address);
        }
        test.changed.assign(test.pages.size(), 0);
        return true;
    }();
    (void)configured;
    return test;
}

Image save(const std::vector<uint32_t>& pages) {
    Image image;
    image.bytes.assign(pages.size() * kPage, 0);
    image.present.assign(pages.size(), 0);
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!guest_memory_committed(pages[i], kPage)) continue;
        image.present[i] = 1;
        std::memcpy(image.bytes.data() + i * kPage, guest_pointer(pages[i]), kPage);
    }
    return image;
}

// The pages that differ between two images.
Mask differing(const Image& a, const Image& b) {
    Mask mask(a.present.size(), 0);
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = a.present[i] && b.present[i] &&
                  std::memcmp(a.bytes.data() + i * kPage, b.bytes.data() + i * kPage, kPage) != 0;
    }
    return mask;
}

size_t count(const Mask& mask) { return static_cast<size_t>(std::count(mask.begin(), mask.end(), 1)); }

uint32_t word(const Image& image, size_t page, uint32_t offset) {
    uint32_t value;
    std::memcpy(&value, image.bytes.data() + page * kPage + offset, sizeof(value));
    return value;
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// The OS's synchronisation objects in the saved memory (mutexes, conditions,
// semaphores, events, message queues: each starts with a magic word, sync.cpp).
// Their state (an owner, a count, who waits) belongs to the threads using
// them, not to the fight: putting it back while another thread holds a lock
// corrupts the lock, so these words are never restored or compared.
struct Zones {
    std::vector<std::pair<uint32_t, uint32_t>> spans; // [begin, end), sorted

    bool contains(uint32_t address) const {
        auto it = std::upper_bound(spans.begin(), spans.end(), std::make_pair(address, UINT32_MAX));
        if (it == spans.begin()) return false;
        --it;
        return address < it->second;
    }
};

Zones find_zones(const std::vector<uint32_t>& pages, const Image& image) {
    struct Kind {
        uint32_t magic;
        uint32_t size;
    };
    static const Kind kKinds[] = {{0x6D557458, 0x2C},  // 'mUtX' OSMutex
                                  {0x634E6456, 0x1C},  // 'cNdV' OSCond
                                  {0x73506852, 0x20},  // 'sPhR' OSSemaphore
                                  {0x65566E54, 0x24},  // 'eVnT' OSEvent
                                  {0x6D536751, 0x3C}}; // 'mSgQ' OSMessageQueue
    Zones zones;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!image.present[i]) continue;
        for (uint32_t w = 0; w < kPage; w += 4) {
            const uint32_t value = be32(image.bytes.data() + i * kPage + w);
            for (const Kind& kind : kKinds) {
                if (value == kind.magic) zones.spans.push_back({pages[i] + w, pages[i] + w + kind.size});
            }
        }
    }
    // Every thread's OSThread and stack too: a thread that sat idle while the
    // other threads were measured but ran during the frames would otherwise
    // look like the frames' own work, and have its stack rewound.
    for (const auto& span : cafe::os::guest_thread_spans()) zones.spans.push_back(span);
    std::sort(zones.spans.begin(), zones.spans.end());
    // Merge overlaps (a mutex on a stack), so `contains` need only look at one.
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& span : zones.spans) {
        if (!merged.empty() && span.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, span.second);
        } else {
            merged.push_back(span);
        }
    }
    zones.spans = std::move(merged);
    return zones;
}

// The words the frames changed (`first` to `after`), word by word, but not
// those another thread changed too while this one waited (`first` to `idle`):
// they are the fight's own, and the rest is not ours to put back.
struct Words {
    Mask ours;     // per page: it has words of the frames
    Mask shared;   // per page: it has words the frames and other threads changed
    size_t ours_words = 0;
    size_t shared_words = 0;
    size_t zone_words = 0; // words of synchronisation objects the frames changed (left alone)
};

Words classify(const std::vector<uint32_t>& pages, const Image& first, const Image& idle, const Image& after,
               const Zones& zones) {
    Words out;
    out.ours.assign(first.present.size(), 0);
    out.shared.assign(first.present.size(), 0);
    for (size_t i = 0; i < first.present.size(); ++i) {
        if (!first.present[i] || !after.present[i] || !idle.present[i]) continue;
        if (std::memcmp(first.bytes.data() + i * kPage, after.bytes.data() + i * kPage, kPage) == 0) continue;
        for (uint32_t w = 0; w < kPage; w += 4) {
            if (word(first, i, w) == word(after, i, w)) continue;
            if (zones.contains(pages[i] + w)) {
                ++out.zone_words;
                continue;
            }
            if (word(first, i, w) == word(idle, i, w)) {
                out.ours[i] = 1;
                ++out.ours_words;
            } else {
                out.shared[i] = 1;
                ++out.shared_words;
            }
        }
    }
    return out;
}

// Puts the frames' words of `first` back; returns how many.
size_t restore(const std::vector<uint32_t>& pages, const Image& first, const Image& idle, const Image& after,
               const Words& words, const Zones& zones) {
    size_t restored = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!words.ours[i]) continue;
        for (uint32_t w = 0; w < kPage; w += 4) {
            if (word(first, i, w) == word(after, i, w) || word(first, i, w) != word(idle, i, w)) continue;
            if (zones.contains(pages[i] + w)) continue;
            const uint32_t value = word(first, i, w);
            std::memcpy(guest_pointer(pages[i] + w), &value, sizeof(value));
            ++restored;
        }
    }
    return restored;
}

// Logs the flagged pages as runs of contiguous ones.
void print_runs(const char* indent, const std::vector<uint32_t>& pages, const Mask& mask, size_t limit) {
    size_t shown = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!mask[i]) continue;
        size_t j = i;
        while (j + 1 < pages.size() && mask[j + 1] && pages[j + 1] == pages[j] + kPage) ++j;
        if (shown++ < limit) {
            std::fprintf(stderr, "ttt2: rollback selftest: %s%08X-%08X (%zu pages)\n", indent, pages[i],
                         pages[j] + kPage, j - i + 1);
        }
        i = j;
    }
    if (shown > limit) std::fprintf(stderr, "ttt2: rollback selftest: %s... %zu more runs\n", indent, shown - limit);
}

// Logs the words where the guest's memory differs from `expected`, but those
// the other threads change (`first` to `idle`) and the synchronisation objects'.
void compare(const char* what, const std::vector<uint32_t>& pages, const Image& expected, const Image& first,
             const Image& idle, const Zones& zones) {
    const Image now = save(pages);
    Mask differs(pages.size(), 0);
    size_t words = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!expected.present[i] || !now.present[i] || !idle.present[i]) continue;
        const uint8_t* want = expected.bytes.data() + i * kPage;
        const uint8_t* have = now.bytes.data() + i * kPage;
        if (std::memcmp(want, have, kPage) == 0) continue;
        for (uint32_t w = 0; w < kPage; w += 4) {
            if (std::memcmp(want + w, have + w, 4) == 0 || word(first, i, w) != word(idle, i, w)) continue;
            if (zones.contains(pages[i] + w)) continue;
            differs[i] = 1;
            ++words;
        }
    }
    if (count(differs) == 0) {
        std::fprintf(stderr, "ttt2: rollback selftest: %s: MATCHES (every word but the other threads' is the same)\n",
                     what);
        return;
    }
    std::fprintf(stderr, "ttt2: rollback selftest: %s: DIFFERS in %zu pages (%zu words)\n", what, count(differs), words);
    print_runs("  ", pages, differs, 30);

    size_t shown = 0;
    for (size_t i = 0; i < differs.size() && shown < 6; ++i) {
        if (!differs[i]) continue;
        ++shown;
        const uint8_t* want = expected.bytes.data() + i * kPage;
        const uint8_t* have = now.bytes.data() + i * kPage;
        size_t listed = 0;
        for (uint32_t w = 0; w < kPage && listed < 6; w += 4) {
            if (std::memcmp(want + w, have + w, 4) == 0 || word(first, i, w) != word(idle, i, w)) continue;
            if (zones.contains(pages[i] + w)) continue;
            std::fprintf(stderr, "ttt2: rollback selftest:     %08X expected %08X, got %08X\n", pages[i] + w,
                         be32(want + w), be32(have + w));
            ++listed;
        }
    }
}

} // namespace

bool rollback_selftest_running_here() {
    SelfTest& t = state();
    return t.mode.load(std::memory_order_relaxed) != Mode::Off && std::this_thread::get_id() == t.thread;
}

void rollback_selftest_push(PPCContext& ctx) {
    SelfTest& t = state();
    const Mode mode = t.mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return;
    std::lock_guard lock(t.mutex);
    if (mode == Mode::Record) {
        t.pushes.push_back({ctx.r[4], ctx.r[5]});
    } else if (t.next_push < t.pushes.size() && t.pushes[t.next_push].side == ctx.r[4]) {
        ctx.r[5] = t.pushes[t.next_push++].buttons;
    } else {
        ++t.mismatches;
    }
}

bool rollback_selftest_frame(PPCContext& ctx, uint8_t* base, PPCFunc* frame) {
    SelfTest& t = state();
    if (!t.enabled || t.next_start >= t.starts.size() || ++t.calls != t.starts[t.next_start]) return false;
    ++t.next_start;

    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    auto say = [](const char* text) { std::fprintf(stderr, "ttt2: rollback selftest: %s\n", text); };
    std::fprintf(stderr, "ttt2: rollback selftest: battle frame %u: %u frames, then the same again twice\n", t.calls,
                 t.frames);

    // What the other threads change while this one does nothing: those words
    // are theirs (counters, timers, buffers filled in the background), not the
    // fight's, and are neither restored nor compared.
    const PPCContext registers = ctx;
    const Image first = save(t.pages);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const Image idle = save(t.pages);
    const Zones zones = find_zones(t.pages, first);
    std::fprintf(stderr,
                 "ttt2: rollback selftest: saved %.1f MiB; other threads changed %zu pages in 25 ms (left out); %zu "
                 "synchronisation objects are never restored\n",
                 first.bytes.size() / 1048576.0, count(differing(first, idle)), zones.spans.size());

    {
        std::lock_guard lock(t.mutex);
        t.pushes.clear();
        t.next_push = 0;
        t.mismatches = 0;
        t.thread = std::this_thread::get_id();
    }
    t.mode = Mode::Record;
    for (uint32_t i = 0; i < t.frames; ++i) {
        ctx = registers;
        frame(ctx, base);
    }
    t.mode = Mode::Off;
    const Image after = save(t.pages);

    // What the frames changed: the memory a rollback has to put back, word by
    // word. Words the other threads changed too are left out, and counted.
    const Words words = classify(t.pages, first, idle, after, zones);
    for (size_t i = 0; i < words.ours.size(); ++i) t.changed[i] = t.changed[i] || words.ours[i];
    std::fprintf(stderr,
                 "ttt2: rollback selftest: the frames pushed %zu button words and changed %zu words in %zu pages (%.1f KiB "
                 "of words):\n",
                 t.pushes.size(), words.ours_words, count(words.ours), words.ours_words * 4 / 1024.0);
    print_runs("  ", t.pages, words.ours, 40);
    if (words.zone_words != 0) {
        std::fprintf(stderr, "ttt2: rollback selftest: %zu more words, in synchronisation objects, were left alone\n",
                     words.zone_words);
    }
    if (words.shared_words != 0) {
        std::fprintf(stderr,
                     "ttt2: rollback selftest: %zu more words, in %zu pages, were changed by the frames and by other "
                     "threads (not restored or compared):\n",
                     words.shared_words, count(words.shared));
        print_runs("  ", t.pages, words.shared, 20);
    }

    // The same frames again from the saved state, twice: the first replay is
    // compared with the live run, the second with the first.
    Image previous = after;
    for (int replay = 1; replay <= 2; ++replay) {
        restore(t.pages, first, idle, after, words, zones);
        {
            std::lock_guard lock(t.mutex);
            t.next_push = 0;
            t.mismatches = 0;
        }
        t.mode = Mode::Replay;
        for (uint32_t i = 0; i < t.frames; ++i) {
            ctx = registers;
            frame(ctx, base);
        }
        t.mode = Mode::Off;
        std::fprintf(stderr, "ttt2: rollback selftest: replay %d used %zu of the %zu pushes, %u did not match\n",
                     replay, t.next_push, t.pushes.size(), t.mismatches);
        compare(replay == 1 ? "replay 1 against the live run" : "replay 2 against replay 1", t.pages, previous, first,
                idle, zones);
        if (replay == 1) previous = save(t.pages);
    }
    std::fprintf(stderr, "ttt2: rollback selftest: over %zu of %zu runs the frames changed %zu different pages (%.2f MiB)\n",
                 t.next_start, t.starts.size(), count(t.changed), count(t.changed) * kPage / 1048576.0);
    say(("took " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()) +
         " ms")
            .c_str());
    return true;
}

} // namespace ttt2::title

// A fighter's buttons going into its side's input queue (r3: the queue object,
// r4: the side, r5: the buttons).
PPC_FUNC(sub_02EF7DC8_orig);
PPC_FUNC(sub_02EF7DC8) {
    ttt2::title::rollback_selftest_push(ctx);
    sub_02EF7DC8_orig(ctx, base);
}
