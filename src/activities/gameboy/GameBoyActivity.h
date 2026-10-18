#pragma once

// The Game Boy screen (X4 Pro builds with CROSSINK_GAMEBOY): runs a DMG ROM
// opened from the file browser on the Peanut-GB core (lib/PeanutGB) at the
// Game Boy's own pace, showing a frame on the e-ink panel four times a
// second, with touch controls beside the picture. Started from the Ink-boy
// fork of CrossInk (https://github.com/hellominecraft913-dev/Ink-boy, MIT).

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"

#if CROSSINK_GAMEBOY
extern "C" {
// The same configuration as src/peanut_gb_impl.c, so both see one gb_s layout.
#define PEANUT_GB_IS_LITTLE_ENDIAN 1
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PEANUT_GB_HEADER_ONLY
#include <peanut_gb.h>
#undef PEANUT_GB_HEADER_ONLY
}

class GameBoyActivity final : public Activity {
 public:
  GameBoyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string romPath);
  ~GameBoyActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool skipLoopDelay() override { return true; }
  uint8_t inputPollDelayMs() const override { return 2; }
  bool preventAutoSleep() override { return true; }
  bool blocksGlobalInput() const override { return true; }

 private:
  enum class TouchControl : uint8_t {
    None,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    Select,
    Start,
    Exit,
    Rotate,
  };

  // One on-screen button, in logical coordinates for the current orientation.
  struct ControlButton {
    TouchControl control;
    int x, y, w, h;
    const char* label;
  };

  static constexpr int GAMEBOY_WIDTH = 160;
  static constexpr int GAMEBOY_HEIGHT = 144;
  static constexpr int MAX_GAME_SCALE = 3;
  static constexpr uint64_t EMULATION_FRAME_PERIOD_US = 16743ULL;
  // At most four screen refreshes a second on the stock refresh. The faster
  // one asks for the next picture as soon as the last is on screen (the short
  // period only paces the redraws of a picture that did not change, which the
  // driver skips). The emulator itself continues to run at the Game Boy's
  // ~59.73 Hz frame cadence.
  static constexpr uint64_t DISPLAY_PERIOD_US = 250000ULL;
  static constexpr uint64_t FAST_DISPLAY_PERIOD_US = 50000ULL;
  static constexpr uint8_t MAX_CATCH_UP_FRAMES = 4;
  static constexpr size_t EMULATOR_FRAME_BYTES = GAMEBOY_WIDTH * GAMEBOY_HEIGHT;
  static constexpr size_t MAX_CONTROLS = 10;
  // Battery-backed cartridge RAM is written out this often while it changes,
  // not only on exit, so a flat battery or a crash loses at most this much.
  static constexpr uint64_t AUTOSAVE_PERIOD_US = 60000000ULL;

  std::string romPath;
  std::string savePath;
  std::string tempSavePath;
  std::string backupSavePath;

  gb_s* gb = nullptr;
  uint8_t* rom = nullptr;
  size_t romSize = 0;
  uint8_t* cartRam = nullptr;
  size_t cartRamSize = 0;

  // CrossInk renders on a separate task. Peanut-GB writes one completed
  // Game Boy frame while the render task consumes the previous completed frame.
  uint8_t* frameBuffers[2] = {nullptr, nullptr};
  uint8_t writeFrame = 0;
  // Frame ownership protocol:
  //   readyFrame = newest completed frame not yet consumed (0xFF = none)
  //   renderFrame = frame currently being copied into the CrossInk framebuffer
  // This prevents the emulation task from overwriting either buffer while the
  // separate render task is consuming it.
  std::atomic<uint8_t> readyFrame{0xFF};
  std::atomic<uint8_t> renderFrame{0xFF};

  uint8_t physicalJoypadMask = 0;
  uint8_t touchJoypadMask = 0;

  int scale = 3;
  int gameX = 160;
  int gameY = 24;
  int gameW = 480;
  int gameH = 432;

  // Portrait (game on top, controls below like the handheld) is the default;
  // ROTATE switches to landscape and back.
  bool portrait = true;
  ControlButton controls[MAX_CONTROLS] = {};
  size_t controlCount = 0;
  int titleX = 0;
  int titleY = 0;

  TouchControl activeTouch = TouchControl::None;

  std::atomic<bool> initialized{false};
  std::atomic<bool> displayDue{true};
  bool exitRequested = false;

  // The emulator runs in its own task on CPU 0: sharing CPU 1 with the render
  // task (and the main loop) slowed the e-ink SPI streaming roughly threefold.
  // It owns the emulator state, its speed counters, autosave and preset
  // switches; the main loop keeps input, the layout and display scheduling.
  TaskHandle_t emuTask = nullptr;
  std::atomic<bool> emuRun{false};
  std::atomic<bool> emuStopped{true};
  static void emuTaskEntry(void* arg);
  void emuTaskLoop();
  void startEmuTask();
  void stopEmuTask();
  bool rotateRequested = false;
  // The next refresh is a full one, clearing what the old layout left behind.
  std::atomic<bool> fullRefreshDue{false};
  bool saveDirty = false;   // cartridge RAM changed since the last save
  uint64_t nextFrameUs = 0;
  uint64_t nextDisplayUs = 0;
  uint64_t nextAutosaveUs = 0;

  char title[32] = {};
  char error[160] = {};

  // Performance counters, logged every 2 s and appended to
  // /.crosspoint/gameboy-perf.txt on exit: the emulator's speed against real
  // time, the time one Game Boy frame takes, how often it waited for a buffer
  // the display held, and how long drawing and the e-ink refresh take. The
  // emulator task owns its counters; the render task adds to the atomics.
  static constexpr uint64_t PERF_WINDOW_US = 2000000ULL;
  uint64_t perfWindowStartUs = 0;
  uint64_t perfSessionStartUs = 0;
  uint32_t perfFrames = 0, perfWaits = 0, perfEmuUs = 0, perfEmuMaxUs = 0;
  uint64_t perfTotalFrames = 0, perfTotalWaits = 0, perfTotalEmuUs = 0;
  uint32_t perfTotalEmuMaxUs = 0;
  std::atomic<uint32_t> perfDisplays{0}, perfDrawUs{0}, perfRefreshUs{0}, perfRefreshMaxUs{0};
  uint64_t perfTotalDisplays = 0, perfTotalDrawUs = 0, perfTotalRefreshUs = 0;
  uint32_t perfTotalRefreshMaxUs = 0;
  // How often loop() runs and its longest gap: a gap as long as a refresh
  // means the display starved the emulator of CPU.
  uint64_t perfLastLoopUs = 0, perfTotalLoops = 0;
  uint32_t perfLoops = 0, perfLoopMaxGapUs = 0, perfTotalLoopMaxGapUs = 0;
  // Cleaning (Half) refreshes are counted apart, so the averages are the Fast
  // refreshes alone; perfPhaseUs adds up the driver's phases of each Fast one
  // (copy, new plane, setup, waveform, old plane - HalDisplay::lastRefreshTimings)
  // and, last, the bytes sent per plane. Pictures the driver skipped because
  // nothing changed count apart too (perfUnchanged).
  std::atomic<uint32_t> perfCleanRefreshes{0}, perfUnchanged{0};
  std::atomic<uint32_t> perfPhaseUs[6] = {};
  uint64_t perfTotalCleanRefreshes = 0, perfTotalUnchanged = 0;
  // The driver's running counts of completed and skipped refreshes as last
  // seen (render task only): with an overlapped refresh, a call completes the
  // previous refresh and starts (or skips) the next.
  uint32_t perfSeenRefreshes = 0, perfSeenSkipped = 0;
  uint64_t perfTotalPhaseUs[6] = {};

  // The screen refresh the game uses: 0 = stock, 1 = the faster one on readers
  // it was validated on (see kRefreshPresets). The faster one gets a cleaning
  // refresh every CLEAN_PERIOD_US against ghosting.
  static constexpr uint64_t CLEAN_PERIOD_US = 60000000ULL;
  uint8_t refreshPreset = 0;
  uint64_t nextCleanUs = 0;

  void setLayout();
  void addControl(TouchControl control, int x, int y, int w, int h, const char* label);
  void rotate();
  void applyRefreshPreset();
  bool loadRom();
  bool buildSavePaths();
  bool initEmulator();
  bool loadSave();
  bool saveCartRam();
  void cleanup();
  void showError(const char* message);

  void handleInput();
  void updateTouch(int x, int y);
  TouchControl hitTestTouch(int x, int y) const;
  static uint8_t joypadBitForTouch(TouchControl control);
  static uint8_t joypadBitForButton(MappedInputManager::Button button);
  void applyJoypad();

  void runEmulation(uint64_t nowUs);
  void updatePerf(uint64_t nowUs);
  void writePerfLog();
  void resetPerfTotals(uint64_t nowUs);
  void releaseRenderedFrame();
  void publishCompletedFrame();
  void drawGameFrame();
  bool drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer);
  bool drawGameFramePortrait(const uint8_t* source, uint8_t* framebuffer);
  void drawControls();
  // What the title and buttons were last drawn with (joypad mask, active touch
  // control, orientation); the framebuffer keeps them between pictures, so
  // they are redrawn only when this changes. CHROME_NONE forces a redraw.
  static constexpr uint32_t CHROME_NONE = 0xFFFFFFFFu;
  uint32_t chromeKey = CHROME_NONE;
  void registerTouchTargets();

  static uint8_t romRead(gb_s* gb, uint_fast32_t addr);
  static uint8_t cartRamRead(gb_s* gb, uint_fast32_t addr);
  static void cartRamWrite(gb_s* gb, uint_fast32_t addr, uint8_t value);
  static void lcdDrawLine(gb_s* gb, const uint8_t* pixels, uint_fast8_t line);
  static void fatalError(gb_s* gb, enum gb_error_e error, uint16_t addr);
  void writeGameBoyLine(const uint8_t* pixels, uint_fast8_t line);

  static bool isBatteryBackedCart(uint8_t type);
  static size_t romBankCountFromCode(uint8_t code);
  static const char* initErrorString(int error);
};

#endif  // CROSSINK_GAMEBOY
