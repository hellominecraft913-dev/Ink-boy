#pragma once

#ifndef CROSSINK_GBA
#define CROSSINK_GBA 0
#endif

#include "activities/Activity.h"

#if CROSSINK_GBA
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class GbaActivity final : public Activity {
 public:
  GbaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string romPath);
  ~GbaActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool skipLoopDelay() override { return true; }
  uint8_t inputPollDelayMs() const override { return 2; }
  bool preventAutoSleep() override { return true; }
  bool blocksGlobalInput() const override { return true; }
  bool handleTwoFingerRotation(bool clockwise) override;

 private:
  enum class TouchControl : uint8_t {
    None,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    L,
    R,
    Select,
    Start,
    Exit,
    Rotate,
  };

  struct ControlButton {
    TouchControl control;
    int x;
    int y;
    int w;
    int h;
  };

  static constexpr int GBA_WIDTH = 240;
  static constexpr int GBA_HEIGHT = 160;
  static constexpr int MAX_SCALE = 2;
  static constexpr uint64_t FRAME_PERIOD_US = 16743ULL;
  static constexpr uint64_t STOCK_DISPLAY_PERIOD_US = 250000ULL;
  static constexpr uint64_t FAST_DISPLAY_PERIOD_US = 50000ULL;
  static constexpr uint64_t CLEAN_PERIOD_US = 60000000ULL;
  static constexpr uint64_t SAVE_CHECK_PERIOD_US = 2000000ULL;
  static constexpr uint64_t AUTOSAVE_PERIOD_US = 60000000ULL;
  static constexpr uint64_t BACK_HOLD_TO_EXIT_MS = 850ULL;
  static constexpr size_t FRAME_BYTES = static_cast<size_t>(GBA_WIDTH) * GBA_HEIGHT;
  static constexpr size_t MAX_CONTROLS = 12;

  // Bit positions match libretro's RETRO_DEVICE_ID_JOYPAD_* IDs.
  enum Joypad : uint16_t {
    JOY_B = 1u << 0,
    JOY_Y = 1u << 1,
    JOY_SELECT = 1u << 2,
    JOY_START = 1u << 3,
    JOY_UP = 1u << 4,
    JOY_DOWN = 1u << 5,
    JOY_LEFT = 1u << 6,
    JOY_RIGHT = 1u << 7,
    JOY_A = 1u << 8,
    JOY_X = 1u << 9,
    JOY_L = 1u << 10,
    JOY_R = 1u << 11,
  };

  std::string romPath;
  std::string savePath;
  std::string tempSavePath;
  std::string backupSavePath;

  uint8_t* frameBuffers[2] = {nullptr, nullptr};
  uint8_t* lastFrame = nullptr;
  uint8_t writeFrame = 0;
  std::atomic<uint8_t> readyFrame{0xFF};
  std::atomic<uint8_t> renderFrame{0xFF};

  uint16_t physicalJoypadMask = 0;
  uint16_t touchJoypadMask = 0;
  std::atomic<uint16_t> joypadMask{0};
  TouchControl activeTouch = TouchControl::None;
  ControlButton controls[MAX_CONTROLS] = {};
  size_t controlCount = 0;

  bool portrait = true;
  bool rotateRequested = false;
  bool exitRequested = false;
  bool backWasPressed = false;
  uint32_t backPressedAtMs = 0;
  int scale = MAX_SCALE;
  int gameX = 0;
  int gameY = 24;
  int gameW = GBA_WIDTH * MAX_SCALE;
  int gameH = GBA_HEIGHT * MAX_SCALE;

  std::atomic<bool> initialized{false};
  std::atomic<bool> displayDue{true};
  std::atomic<bool> fullRefreshDue{false};
  std::atomic<bool> emuRun{false};
  std::atomic<bool> emuStopped{true};
  TaskHandle_t emuTask = nullptr;
  bool coreStarted = false;
  bool saveDirty = false;
  uint8_t refreshPreset = 0;  // 0=stock refresh; 1=validated X4 Pro fast refresh
  uint64_t nextFrameUs = 0;
  uint64_t nextDisplayUs = 0;
  uint64_t nextCleanUs = 0;
  uint64_t nextSaveCheckUs = 0;
  uint64_t lastSaveUs = 0;
  uint64_t lastSavedHash = 0;
  uint64_t lastObservedHash = 0;
  size_t lastSavedSize = 0;
  size_t lastObservedSize = 0;

  void setLayout();
  void addControl(TouchControl control, int x, int y, int w, int h);
  void rotate();
  bool buildSavePaths();
  bool loadSave();
  bool saveCartRam(bool force);
  void checkSaveData(uint64_t nowUs);
  uint64_t hashSaveData(size_t& size) const;
  void cleanup();
  void showError(const char* message);

  void handleInput();
  void updateTouch(int x, int y);
  TouchControl hitTestTouch(int x, int y) const;
  static uint16_t joypadBitForTouch(TouchControl control);
  static uint16_t joypadBitForButton(MappedInputManager::Button button);
  void applyJoypad();

  void startEmuTask();
  void stopEmuTask();
  static void emuTaskEntry(void* arg);
  void emuTaskLoop();
  static void videoRefresh(void* user, const uint16_t* pixels, unsigned width,
                           unsigned height, size_t pitchBytes);
  void onVideoFrame(const uint16_t* pixels, unsigned width, unsigned height, size_t pitchBytes);
  void publishCompletedFrame();
  void releaseRenderedFrame();
  void drawGameFrame();
  bool drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer);
  bool drawGameFramePortrait(const uint8_t* source, uint8_t* framebuffer);
  void drawControls();
  void drawControlIcon(const ControlButton& c, bool foregroundBlack);
  void registerTouchTargets();
  void applyRefreshPreset();
  static bool fastRefreshValidated();
};
#endif  // CROSSINK_GBA
