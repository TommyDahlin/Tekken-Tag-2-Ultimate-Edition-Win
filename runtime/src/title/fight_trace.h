#pragma once

// A read-only trace of the guest memory a fight changes, for working out what
// a rollback would have to save (and whether a fight replays the same).
//
// TTT2_TRACE_FIGHT_STATE=<file> writes, for each battle frame (the battle
// state's, GameMode::Impl::state_game_battle), the 4 KiB pages of the title's
// memory that changed since the frame before and a hash of each. Two runs of
// the same input script (TTT2_INPUT_SCRIPT) give files that
// tools/fight_state_report.py compares.
//   TTT2_TRACE_FIGHT_FRAMES=<n>   frames to trace (default 300)
//   TTT2_TRACE_FIGHT_STRIDE=<n>   scan every n-th frame (default 1)
// With the variable unset nothing is read or written.

namespace ttt2::title {

// Called after each frame of the battle state.
void trace_fight_frame();

} // namespace ttt2::title
