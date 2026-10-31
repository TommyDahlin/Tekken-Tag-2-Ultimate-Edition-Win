// See fight_trace.h.
//
// File format (text, one record per line):
//   # ttt2 fight trace v1 stride=<n>
//   R <first page address> <pages>          a scanned range, once
//   F <battle frame> <changed pages>        a scan; the first one only sets the baseline
//   P <page address> <hash>                 a page that changed in that scan

#include "title/fight_trace.h"

#include "cafe/layout.h"
#include "cafe/runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The memory a title's objects live in: MEM2 (the heaps and the static data),
// the foreground bucket and MEM1.
constexpr Range kRanges[] = {
    {layout::kMem2Base, layout::kMem2End},
    {layout::kForegroundBucketBase, layout::kForegroundBucketBase + layout::kForegroundBucketSize},
    {layout::kMem1Base, layout::kMem1Base + layout::kMem1Size},
};

struct Tracer {
    std::FILE* file = nullptr;
    uint32_t frames_left = 0;
    uint32_t stride = 1;
    uint32_t calls = 0;
    uint32_t scans = 0;
    std::vector<uint32_t> pages;   // the address of each page scanned
    std::vector<uint64_t> before;  // its hash at the last scan (0: not committed)
    std::vector<uint64_t> now;
};

uint64_t hash_page(const uint8_t* p) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < kPage; i += 8) {
        uint64_t word;
        std::memcpy(&word, p + i, sizeof(word));
        h = (h ^ word) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h | 1; // never 0
}

uint32_t env_u32(const char* name, uint32_t otherwise) {
    const char* value = std::getenv(name);
    if (!value || !*value) return otherwise;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed == 0 ? otherwise : static_cast<uint32_t>(parsed);
}

Tracer* open_tracer() {
    const char* path = std::getenv("TTT2_TRACE_FIGHT_STATE");
    if (!path || !*path) return nullptr;
    static Tracer tracer;
    tracer.file = std::fopen(path, "w");
    if (!tracer.file) {
        std::fprintf(stderr, "ttt2: fight trace: cannot write %s\n", path);
        return nullptr;
    }
    tracer.frames_left = env_u32("TTT2_TRACE_FIGHT_FRAMES", 300);
    tracer.stride = env_u32("TTT2_TRACE_FIGHT_STRIDE", 1);
    std::fprintf(tracer.file, "# ttt2 fight trace v1 stride=%u\n", tracer.stride);
    for (const Range& range : kRanges) {
        std::fprintf(tracer.file, "R %08X %u\n", range.begin, (range.end - range.begin) / kPage);
        for (uint32_t address = range.begin; address < range.end; address += kPage) tracer.pages.push_back(address);
    }
    tracer.before.assign(tracer.pages.size(), 0);
    tracer.now.assign(tracer.pages.size(), 0);
    std::fprintf(stderr, "ttt2: fight trace: %s, %u frames, every %u, %zu pages\n", path, tracer.frames_left,
                 tracer.stride, tracer.pages.size());
    return &tracer;
}

void hash_all(Tracer& t) {
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    auto work = [&t](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            const uint32_t address = t.pages[i];
            t.now[i] = guest_memory_committed(address, kPage) ? hash_page(guest_pointer(address)) : 0;
        }
    };
    std::vector<std::thread> pool;
    const size_t chunk = (t.pages.size() + threads - 1) / threads;
    for (unsigned k = 0; k < threads; ++k) {
        const size_t from = k * chunk;
        if (from >= t.pages.size()) break;
        pool.emplace_back(work, from, std::min(t.pages.size(), from + chunk));
    }
    for (std::thread& thread : pool) thread.join();
}

} // namespace

void trace_fight_frame() {
    static Tracer* tracer = open_tracer();
    if (!tracer || tracer->frames_left == 0) return;
    if (tracer->calls++ % tracer->stride != 0) return;

    hash_all(*tracer);
    const bool baseline = tracer->scans++ == 0;
    size_t changed = 0;
    if (!baseline) {
        for (size_t i = 0; i < tracer->pages.size(); ++i) changed += tracer->now[i] != tracer->before[i];
    }
    std::fprintf(tracer->file, "F %u %zu\n", tracer->calls - 1, changed);
    if (!baseline) {
        for (size_t i = 0; i < tracer->pages.size(); ++i) {
            if (tracer->now[i] != tracer->before[i]) {
                std::fprintf(tracer->file, "P %08X %08X\n", tracer->pages[i],
                             static_cast<uint32_t>(tracer->now[i] >> 32));
            }
        }
    }
    tracer->before.swap(tracer->now);
    if (--tracer->frames_left == 0) {
        std::fclose(tracer->file);
        tracer->file = nullptr;
        std::fprintf(stderr, "ttt2: fight trace: done\n");
    }
}

} // namespace ttt2::title
