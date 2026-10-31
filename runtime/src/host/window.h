#pragma once

// The game window: SDL3 on the process's main thread, which owns the event
// loop; keyboard (keys from keyboard.ini, host/keymap.h) and gamepads feed
// the Wii U GamePad state. The renderer presents into it from the GPU
// thread through a Vulkan surface. Whether it starts fullscreen, its size
// and the renderer's scale and frame rate come from display.ini
// (host/display_settings.h), read when it opens.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace cafe::host {

// Opens the window; false if there is no display (the port then runs
// headless). Sets display::active() for the run first: the renderer reads
// it. Main thread only.
bool open_window(const char* title);
bool window_open();
// The window's icon, RGBA rows top first. Main thread only.
void set_window_icon(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

// The kind of controller the player uses: the last one pressed (gamepads
// by their maker's button names; the keyboard counts as kOther).
// TTT2_CONTROLLER=playstation, nintendo or xbox sets it for good.
enum class Controller { kOther, kPlayStation, kNintendo };
Controller active_controller();
// Called on the event loop's thread when the kind changes.
void on_controller_change(std::function<void(Controller)> callback);

// What a player holds, for pictures of it: the keyboard, or a gamepad by
// its maker (kXbox: any other gamepad).
enum class Device { kKeyboard, kXbox, kPlayStation, kNintendo };

// The title's controllers: the GamePad (0) and Pro Controllers 1-4.
constexpr size_t kControllers = 5;

// Players with controllers of their own, in the title's modes for several
// players: `check` says whether the title is in one, told whether any
// device has taken a controller (asked on the event loop's thread).
// Outside them every device drives the GamePad, as one player's. In them
// the first device to press + takes the GamePad, and every other one that
// presses + a Pro Controller of its own; devices that have not pressed +
// drive the GamePad until it is taken, then nothing. Leaving the mode
// gives them all back to the GamePad.
void on_own_controllers(std::function<bool(bool joined)> check);

// The device on each of the title's controllers; none where no device is.
// While every device drives the GamePad, its device is the one used last.
// TTT2_DEVICES=keyboard,playstation,... sets them for good (testing).
std::array<std::optional<Device>, kControllers> controller_devices();
// Called on the event loop's thread when they change.
void on_controller_devices_change(std::function<void()> callback);

// Runs the event loop until `finished` returns true or the window closes.
// Returns false if the user closed the window.
bool run_event_loop(const std::function<bool()>& finished);

// For the renderer (any thread).
std::vector<const char*> vulkan_instance_extensions();
VkSurfaceKHR create_vulkan_surface(VkInstance instance);
void drawable_size(uint32_t& width, uint32_t& height);
// The desktop resolution of the primary display, in pixels.
void display_size(uint32_t& width, uint32_t& height);
// The primary display's refresh rate, in Hz.
float display_refresh_rate();
// The refresh rate of the display the window is on, in Hz (it follows the
// window to other displays; the primary's before the window opens).
float window_refresh_rate();

} // namespace cafe::host
