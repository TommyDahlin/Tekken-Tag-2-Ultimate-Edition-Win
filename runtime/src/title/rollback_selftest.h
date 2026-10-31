#pragma once

// A self-test of what a rollback needs: that running K battle frames, putting
// back what they changed and running the same K frames again ends in the same
// memory.
//
// TTT2_ROLLBACK_SELFTEST=1 runs it when the battle state reaches each frame of
// TTT2_ROLLBACK_SELFTEST_FRAME (a list, "180,400,900"; default 180); each run
// ends by logging the pages any run's frames changed so far:
//   1. the registers and the memory of the ranges below are saved, and again
//      25 ms later: the words that differ are what the other threads change
//      meanwhile (counters, background buffers), and are left out below;
//   2. K frames (TTT2_ROLLBACK_SELFTEST_K, default 2) of the battle state run,
//      back to back, while the buttons each fighter pushes into its input
//      queue (sub_02EF7DC8) are recorded;
//   3. the memory after them is saved; the words the frames changed (the
//      others' left out) are logged: what a rollback has to save;
//   4. those words are put back, the same K frames run again with the
//      recorded buttons pushed in place of the pad's, and the memory is
//      compared with step 3's (the others' pages left out);
//   5. that is done once more, compared with the first replay's.
// The game goes on from the replayed state, K frames on. Nothing happens with
// the variable unset. The OS's synchronisation objects (mutexes, queues, ...,
// found by their magic words) are never put back either: their owner and
// count belong to the threads using them, and rewinding a lock another thread
// holds corrupts it (a crash inside OSLockMutex of the game's allocator).
// Only the words the frames wrote are put back: the
// ranges also hold the OS's locks and queues and other threads' data, which
// must not change under them (putting back whole pages froze the game, and
// leaving out pages the other threads also write crashed a replay).
//
// Ranges: TTT2_ROLLBACK_SELFTEST_RANGES="begin-end,begin-end" (hex), by
// default 0x10000000-0x16000000 (the static data and the heaps, with the
// game's allocator's arena and the chunks it hands out: saving only part of
// them corrupts its free lists) and 0x30000000-0x32000000, which leaves out
// what the GPU writes (the skinned vertices at 0x2A000000 and on, MEM1).
// Every thread's OSThread and stack are left out of the restore as well.

#include "cafe/ppc_context.h"

#include <cstdint>

namespace ttt2::title {

// Runs a frame of the battle state through `frame`; false when the self-test
// is off or has run (the caller runs the frame itself).
bool rollback_selftest_frame(cafe::PPCContext& ctx, uint8_t* base, cafe::PPCFunc* frame);

// True on the thread running the self-test's frames while it does (for the
// fault handler to say whose crash it is).
bool rollback_selftest_running_here();

// Called with the registers of sub_02EF7DC8 (r4: the side, r5: the buttons)
// before it runs; may change r5.
void rollback_selftest_push(cafe::PPCContext& ctx);

} // namespace ttt2::title
