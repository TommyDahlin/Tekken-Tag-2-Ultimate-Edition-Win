#pragma once

// Pictures of the players' devices in the style of the title's pictures of
// Wii U controllers (its VS screens'): white, with grey lines, seen from
// above; a player's side lights them with a glow of its colour, and small
// ones stand on a shadow. Drawn here, from shapes.

#include "title/image.h"

#include <cstdint>

namespace cafe::title::pictures {

enum class Device { kKeyboard, kPlayStation, kXbox };

// The size of the title's pictures, and of its small ones.
constexpr uint32_t kWidth = 320, kHeight = 220;
constexpr uint32_t kSmallWidth = 168, kSmallHeight = 104;

// The device, white, kWidth x kHeight.
Rgba white(Device device);

// A picture lit by a side's colour, as the title lights its own: a glow
// outside it of `outer` (RGB), and inside a tint toward `inner` that fades
// from its edge.
Rgba lit(const Rgba& picture, uint32_t outer, uint32_t inner);
constexpr uint32_t kRedGlow = 0xFC0000, kRedTint = 0xFF0000;   // 1P
constexpr uint32_t kCyanGlow = 0x00D9F9, kCyanTint = 0x00FFFF; // 2P

// A picture made small (kSmallWidth x kSmallHeight), on its shadow.
Rgba small(const Rgba& picture);

} // namespace cafe::title::pictures
