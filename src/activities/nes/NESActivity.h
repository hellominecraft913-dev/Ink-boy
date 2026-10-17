#pragma once

#ifndef CROSSINK_AGNES
#define CROSSINK_AGNES 0
#endif

#include "activities/Activity.h"

#if CROSSINK_AGNES
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/nes/vendor/agnes.h"

class NESActivity final : public Activity {
 public:
  NESActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string romPath);
  ~NESActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool skipLoopDelay() override { return true; }
  uint8_t inputPollDelayMs() const override { return 2; }
  bool preventAutoSleep() override { return true; }
  bool blocksGlobalInput() const override { return true; }

 private:
  enum class TouchControl : uint8_t { None, Up, Down, Left, Right, A, B, Select, Start, Exit };

  static constexpr int NES_WIDTH = AGNES_SCREEN_WIDTH;
  static constexpr int NES_HEIGHT = AGNES_SCREEN_HEIGHT;
  static constexpr int MAX_SCALE = 2;
  static constexpr size_t FRAME_BYTES = static_cast<size_t>(NES_WIDTH) * NES_HEIGHT;
  static constexpr uint64_t EMULATION_FRAME_PERIOD_US = 16667ULL;
  // Match the latest Ink-boy game loop: schedule the next picture quickly and
  // let the validated panel driver finish/skip unchanged overlapped refreshes.
  static constexpr uint64_t STOCK_DISPLAY_PERIOD_US = 250000ULL;
  static constexpr uint64_t FAST_DISPLAY_PERIOD_US = 50000ULL;
  static constexpr uint64_t CLEAN_PERIOD_US = 60000000ULL;
  static constexpr uint8_t MAX_CATCH_UP_FRAMES = 4;

  enum Joypad : uint8_t {
    JOY_UP = 1u << 0,
    JOY_DOWN = 1u << 1,
    JOY_LEFT = 1u << 2,
    JOY_RIGHT = 1u << 3,
    JOY_A = 1u << 4,
    JOY_B = 1u << 5,
    JOY_SELECT = 1u << 6,
    JOY_START = 1u << 7,
  };

  std::string romPath;
  uint8_t* rom = nullptr;
  size_t romSize = 0;
  agnes_t* emulator = nullptr;

  // The emulator task publishes shade-indexed frames to the rendering task.
  // A buffer is never reused while it is the newest published or being drawn.
  uint8_t* frameBuffers[2] = {nullptr, nullptr};
  uint8_t writeFrame = 0;
  std::atomic<uint8_t> readyFrame{0xFF};
  std::atomic<uint8_t> renderFrame{0xFF};
  std::atomic<uint8_t> joypadMask{0};

  uint8_t physicalJoypadMask = 0;
  uint8_t touchJoypadMask = 0;
  TouchControl activeTouch = TouchControl::None;
  std::atomic<bool> initialized{false};
  std::atomic<bool> displayDue{true};
  std::atomic<bool> fullRefreshDue{false};
  std::atomic<bool> emuRun{false};
  std::atomic<bool> emuStopped{true};
  bool exitRequested = false;
  bool backWasPressed = false;
  uint32_t backPressedAtMs = 0;
  uint64_t nextFrameUs = 0;
  uint64_t nextDisplayUs = 0;
  uint64_t nextCleanUs = 0;
  TaskHandle_t emuTask = nullptr;
  uint8_t refreshPreset = 0;  // 0=stock, 1=validated X4 Pro fast-window preset.

  int scale = 2;
  int gameX = 0;
  int gameY = 0;
  int gameW = NES_WIDTH * 2;
  int gameH = NES_HEIGHT * 2;
  char error[128] = {};

  bool loadRom();
  bool initEmulator();
  void cleanup();
  void showError(const char* message);
  void setLayout();
  void handleInput();
  void updateTouch(int x, int y);
  TouchControl hitTestTouch(int x, int y) const;
  static uint8_t joypadBitForTouch(TouchControl control);
  void applyJoypad();

  void startEmuTask();
  void stopEmuTask();
  static void emuTaskEntry(void* arg);
  void emuTaskLoop();
  void runEmulation(uint64_t nowUs);
  void publishCompletedFrame();
  void releaseRenderedFrame();
  void drawGameFrame();
  bool drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer);
  void drawControls();
  void registerTouchTargets();
  void applyRefreshPreset();
  static bool fastRefreshValidated();
};
#endif  // CROSSINK_AGNES
