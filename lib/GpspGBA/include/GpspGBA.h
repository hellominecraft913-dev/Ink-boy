#pragma once

#include <cstddef>
#include <cstdint>

namespace gpsp {

// RGB565 frame callback. The frame is valid only for the duration of callback.
using VideoCallback = void (*)(void* user, const uint16_t* pixels, unsigned width,
                               unsigned height, size_t pitchBytes);

// Starts the upstream gpSP libretro core and opens the ROM directly from
// Ink-boy's existing Storage filesystem. No additional ROM directory is used.
bool initialize(const char* romPath, VideoCallback video, void* user);

// Run exactly one GBA frame with RETRO_DEVICE_ID_JOYPAD bit positions
// (B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R).
void runFrame(uint16_t joypadMask);

// Returns the battery-backed cartridge memory supported by the core's
// detected save type. The pointer remains owned by gpSP until shutdown().
uint8_t* batteryRamData();
size_t batteryRamSize();

const char* lastError();
bool isLoaded();
void shutdown();

}  // namespace gpsp
