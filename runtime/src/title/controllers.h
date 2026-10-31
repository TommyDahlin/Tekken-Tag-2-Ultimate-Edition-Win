#pragma once

// The players' controllers on a computer:
// - The title's modes for several players (VS Battle, Team Battle, Pair
//   Play) give each player a controller of their own: the devices join
//   as the title's controllers by pressing + on its entry screen
//   (host::on_own_controllers); elsewhere every device is the GamePad.
// - That screen's pictures of the controllers show the players' devices:
//   the GamePad's the device on the GamePad, the Pro Controller's the one
//   on the first Pro Controller. A keyboard, a PlayStation-style and an
//   Xbox-style controller are drawn in the style of the title's pictures
//   (title/pictures.h); a Nintendo controller shows as the Pro Controller.

namespace cafe::title {

// After vfs::open_game and the image load, before the title runs.
void prepare_controllers();

} // namespace cafe::title
