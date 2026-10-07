#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"

#if CROSSINK_GAMEBOY
extern "C" {
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
  };

  static constexpr int GAMEBOY_WIDTH = 160;
  static constexpr int GAMEBOY_HEIGHT = 144;
  static constexpr int MAX_GAME_SCALE = 3;
  static constexpr uint64_t EMULATION_FRAME_PERIOD_US = 16743ULL;
  // Four complete screen refreshes per second is the target. The emulator
  // itself continues to run at the Game Boy's ~59.73 Hz frame cadence.
  static constexpr uint64_t DISPLAY_PERIOD_US = 250000ULL;
  static constexpr uint8_t MAX_CATCH_UP_FRAMES = 4;
  static constexpr size_t EMULATOR_FRAME_BYTES = GAMEBOY_WIDTH * GAMEBOY_HEIGHT;
  static constexpr int TOUCH_EXIT_MIN_WIDTH = 120;

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

  TouchControl activeTouch = TouchControl::None;

  std::atomic<bool> initialized{false};
  std::atomic<bool> displayDue{true};
  bool exitRequested = false;
  bool saveWritten = false;
  uint64_t nextFrameUs = 0;
  uint64_t nextDisplayUs = 0;

  char title[32] = {};
  char error[160] = {};

  void setLayout();
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
  void publishCompletedFrame();
  void drawGameFrame();
  void drawControls();
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
