#include "NESActivity.h"

#if CROSSINK_AGNES

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "components/TouchRegistry.h"
#include "fontIds.h"

namespace {
constexpr char TAG[] = "NES";
constexpr size_t INES_HEADER_SIZE = 16;
constexpr uint64_t BACK_HOLD_TO_EXIT_MS = 850;
constexpr int TOUCH_GAP = 4;
constexpr int MAX_GAME_ROW_BYTES = 64;

bool hitRect(const int x, const int y, const int rx, const int ry, const int rw, const int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

uint64_t nowMicros() { return static_cast<uint64_t>(esp_timer_get_time()); }

// Agnes exposes the NES palette as RGB. Convert it to one of four luminance
// levels before e-ink dithering; no RGB value is written to the panel buffer.
uint8_t rgbToGrayShade(const agnes_color_t color) {
  const uint32_t luminance = (static_cast<uint32_t>(color.r) * 299u +
                               static_cast<uint32_t>(color.g) * 587u +
                               static_cast<uint32_t>(color.b) * 114u) / 1000u;
  if (luminance >= 192u) return 0;  // white
  if (luminance >= 128u) return 1;  // light grey
  if (luminance >= 64u) return 2;   // dark grey
  return 3;                         // black
}

// 0=white, 1=light grey (about 1/4 black), 2=dark grey (about 3/4 black),
// 3=black. Match the stable four-shade dithering used by Ink-boy's GB renderer.
bool shadeIsBlack(const uint8_t shade, const int x, const int y) {
  switch (shade) {
    case 3: return true;
    case 2: return ((x ^ y) & 1) == 0 || (x & 1) == 0;
    case 1: return (x & 1) == 0 && (y & 1) == 0;
    default: return false;
  }
}

void buildGameRow(uint8_t* out, const uint8_t* source, const int step, const int count, const int scale,
                  const uint8_t (&patterns)[4][2]) {
  uint32_t acc = 0;
  int bits = 0;
  for (int x = 0; x < count; ++x) {
    acc = (acc << scale) | patterns[source[x * step] & 0x03][(x * scale) & 1];
    bits += scale;
    if (bits >= 8) {
      bits -= 8;
      *out++ = static_cast<uint8_t>(~(acc >> bits));  // panel framebuffer: 1 is white
      acc &= (1u << bits) - 1u;
    }
  }
}

void gamePatterns(uint8_t (&patterns)[2][4][2], const int scale) {
  for (int fixed = 0; fixed < 2; ++fixed) {
    for (int shade = 0; shade < 4; ++shade) {
      for (int along0 = 0; along0 < 2; ++along0) {
        uint8_t pattern = 0;
        for (int offset = 0; offset < scale; ++offset) {
          pattern = static_cast<uint8_t>((pattern << 1) |
              (shadeIsBlack(static_cast<uint8_t>(shade), along0 + offset, fixed) ? 1 : 0));
        }
        patterns[fixed][shade][along0] = pattern;
      }
    }
  }
}
}  // namespace

NESActivity::NESActivity(GfxRenderer& rendererIn, MappedInputManager& mappedInputIn, std::string romPathIn)
    : Activity("NES", rendererIn, mappedInputIn), romPath(std::move(romPathIn)) {}

NESActivity::~NESActivity() { cleanup(); }

void NESActivity::onEnter() {
  Activity::onEnter();
  stopEmuTask();
  cleanup();
  exitRequested = false;
  backWasPressed = false;
  backPressedAtMs = 0;
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  activeTouch = TouchControl::None;
  nextFrameUs = 0;
  nextDisplayUs = 0;
  nextCleanUs = 0;
  displayDue.store(true, std::memory_order_release);
  fullRefreshDue.store(false, std::memory_order_release);
  error[0] = '\0';

  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  setLayout();
  renderer.clearScreen(0xFF);
  if (romPath.empty()) {
    showError("No NES ROM selected");
    return;
  }
  if (!loadRom() || !initEmulator()) return;

  initialized.store(true, std::memory_order_release);
  applyJoypad();
  requestUpdate(true);
  const uint64_t nowUs = nowMicros();
  nextFrameUs = nowUs;
  nextDisplayUs = nowUs + STOCK_DISPLAY_PERIOD_US;
  if (fastRefreshValidated()) {
    refreshPreset = 1;
    applyRefreshPreset();
    nextDisplayUs = nowUs + FAST_DISPLAY_PERIOD_US;
  }
  startEmuTask();
  LOG_INF(TAG, "Started %s (%u bytes)", romPath.c_str(), static_cast<unsigned>(romSize));
}

void NESActivity::onExit() {
  stopEmuTask();
  TouchRegistry::getInstance().clear();
  if (refreshPreset != 0) {
    display.finishOverlapped();
    display.setFastRefreshTuning(0, 0);
    display.setFastRefreshDuScale(0);
    display.setFastRefreshDiffWindow(false);
    display.setDisplaySpiHz(0);
    refreshPreset = 0;
  }
  cleanup();
  renderer.setOrientation(GfxRenderer::Portrait);
  Activity::onExit();
}

void NESActivity::loop() {
  handleInput();
  if (exitRequested) {
    mappedInput.suppressCurrentTouchContact();
    finish();
    return;
  }
  if (!initialized.load(std::memory_order_acquire)) {
    // Keep the error screen escapable even though there are no game touch targets.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) exitRequested = true;
#if CROSSINK_APP_CAP_TOUCH
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTouchDown(touchX, touchY)) exitRequested = true;
#endif
    if (exitRequested) {
      mappedInput.suppressCurrentTouchContact();
      finish();
    }
    return;
  }
  const uint64_t nowUs = nowMicros();
  if (nowUs >= nextDisplayUs) {
    displayDue.store(true, std::memory_order_release);
    nextDisplayUs = nowUs + (refreshPreset != 0 ? FAST_DISPLAY_PERIOD_US : STOCK_DISPLAY_PERIOD_US);
    requestUpdate();
  }
  delay(2);
}

void NESActivity::render(RenderLock&&) {
  if (!displayDue.exchange(false, std::memory_order_acq_rel)) return;
  if (!initialized.load(std::memory_order_acquire)) {
    renderer.clearScreen(0xFF);
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 30, "NES", true,
                              EpdFontFamily::BOLD);
    if (error[0]) renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 8, error, true);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 50, "Back to exit", true);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const bool cleaning = fullRefreshDue.exchange(false, std::memory_order_acq_rel);
  renderer.clearScreen(0xFF);
  drawGameFrame();
  releaseRenderedFrame();  // frame is now copied into the panel framebuffer
  drawControls();
  registerTouchTargets();

  // On validated UC8279-0x68 X4 Pro panels, the driver holds a private copy of
  // the differential frame, so it is safe to draw the next frame while the
  // current fast waveform is running. Other panels use normal blocking refresh.
  const bool overlap = !cleaning && refreshPreset != 0 && renderer.supportsAsyncRefresh();
  if (overlap) {
    display.displayBufferOverlapped(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(cleaning ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  }
}

bool NESActivity::loadRom() {
  HalFile file;
  if (!Storage.openFileForRead(TAG, romPath, file)) {
    showError("Unable to open NES ROM");
    return false;
  }
  const uint64_t fileSize = file.fileSize64();
  constexpr uint64_t MAX_ROM_BYTES = 4ULL * 1024ULL * 1024ULL;
  if (fileSize < INES_HEADER_SIZE || fileSize > MAX_ROM_BYTES || fileSize > static_cast<uint64_t>(SIZE_MAX)) {
    file.close();
    showError("NES ROM size is invalid (max 4 MiB)");
    return false;
  }
  uint8_t header[INES_HEADER_SIZE] = {};
  if (!file.seekSet(0) || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    file.close();
    showError("NES header could not be read");
    return false;
  }
  if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
    file.close();
    showError("Not an iNES .nes ROM");
    return false;
  }

  rom = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(fileSize), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!rom) {
    file.close();
    showError("Not enough PSRAM for NES ROM");
    return false;
  }
  if (!file.seekSet(0)) {
    file.close();
    showError("NES ROM seek failed");
    return false;
  }
  size_t offset = 0;
  while (offset < static_cast<size_t>(fileSize)) {
    const size_t wanted = std::min<size_t>(16384, static_cast<size_t>(fileSize) - offset);
    const int received = file.read(rom + offset, wanted);
    if (received <= 0) {
      file.close();
      showError("NES ROM read failed");
      return false;
    }
    offset += static_cast<size_t>(received);
  }
  file.close();
  romSize = static_cast<size_t>(fileSize);
  return true;
}

bool NESActivity::initEmulator() {
  emulator = agnes_make();
  if (!emulator) {
    showError("Not enough memory for NES core");
    return false;
  }
  if (!agnes_load_ines_data(emulator, rom, romSize)) {
    showError("Unsupported NES ROM or mapper");
    return false;
  }
  for (auto& frame : frameBuffers) {
    frame = static_cast<uint8_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame) {
      showError("Not enough PSRAM for NES video");
      return false;
    }
    std::memset(frame, 0, FRAME_BYTES);
  }
  writeFrame = 0;
  readyFrame.store(0xFF, std::memory_order_release);
  renderFrame.store(0xFF, std::memory_order_release);
  return true;
}

void NESActivity::cleanup() {
  initialized.store(false, std::memory_order_release);
  if (emulator) {
    agnes_destroy(emulator);
    emulator = nullptr;
  }
  if (rom) {
    heap_caps_free(rom);
    rom = nullptr;
  }
  for (auto*& frame : frameBuffers) {
    if (frame) heap_caps_free(frame);
    frame = nullptr;
  }
  romSize = 0;
  writeFrame = 0;
  readyFrame.store(0xFF, std::memory_order_release);
  renderFrame.store(0xFF, std::memory_order_release);
  joypadMask.store(0, std::memory_order_release);
  physicalJoypadMask = touchJoypadMask = 0;
  activeTouch = TouchControl::None;
}

void NESActivity::showError(const char* message) {
  std::strncpy(error, message ? message : "NES error", sizeof(error) - 1);
  error[sizeof(error) - 1] = '\0';
  initialized.store(false, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

void NESActivity::setLayout() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  // Reserve side columns for the virtual D-pad and A/B/SELECT/START controls.
  scale = std::min({MAX_SCALE, std::max(1, (screenW - 240) / NES_WIDTH), screenH / NES_HEIGHT});
  scale = std::max(1, scale);
  gameW = NES_WIDTH * scale;
  gameH = NES_HEIGHT * scale;
  gameX = (screenW - gameW) / 2;
  gameY = (screenH - gameH) / 2;
}

uint8_t NESActivity::joypadBitForTouch(const TouchControl control) {
  switch (control) {
    case TouchControl::Up: return JOY_UP;
    case TouchControl::Down: return JOY_DOWN;
    case TouchControl::Left: return JOY_LEFT;
    case TouchControl::Right: return JOY_RIGHT;
    case TouchControl::A: return JOY_A;
    case TouchControl::B: return JOY_B;
    case TouchControl::Select: return JOY_SELECT;
    case TouchControl::Start: return JOY_START;
    default: return 0;
  }
}

void NESActivity::applyJoypad() {
  joypadMask.store(static_cast<uint8_t>(physicalJoypadMask | touchJoypadMask), std::memory_order_release);
}

void NESActivity::handleInput() {
  using Button = MappedInputManager::Button;
  uint8_t nextPhysical = 0;
  if (mappedInput.isPressed(Button::Up)) nextPhysical |= JOY_UP;
  if (mappedInput.isPressed(Button::Down)) nextPhysical |= JOY_DOWN;
  if (mappedInput.isPressed(Button::Left)) nextPhysical |= JOY_LEFT;
  if (mappedInput.isPressed(Button::Right)) nextPhysical |= JOY_RIGHT;
  if (mappedInput.isPressed(Button::Confirm)) nextPhysical |= JOY_A;
  if (mappedInput.isPressed(Button::Power)) nextPhysical |= JOY_B;

  if (mappedInput.isPressed(Button::Back)) {
    nextPhysical |= JOY_SELECT;
    if (!backWasPressed) {
      backWasPressed = true;
      backPressedAtMs = millis();
    } else if (millis() - backPressedAtMs >= BACK_HOLD_TO_EXIT_MS) {
      exitRequested = true;
    }
  } else {
    backWasPressed = false;
    backPressedAtMs = 0;
  }
  if (nextPhysical != physicalJoypadMask) {
    physicalJoypadMask = nextPhysical;
    applyJoypad();
  }

#if CROSSINK_APP_CAP_TOUCH
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) updateTouch(x, y);
  if (mappedInput.isScreenTouchHeld(x, y)) {
    updateTouch(x, y);
  } else if (mappedInput.wasScreenTouchReleased()) {
    activeTouch = TouchControl::None;
    touchJoypadMask = 0;
    applyJoypad();
  }
#endif
}

void NESActivity::updateTouch(const int x, const int y) {
  const TouchControl control = hitTestTouch(x, y);
  if (control == TouchControl::Exit) {
    exitRequested = true;
    return;
  }
  if (control == activeTouch) return;
  activeTouch = control;
  touchJoypadMask = joypadBitForTouch(control);
  applyJoypad();
}

NESActivity::TouchControl NESActivity::hitTestTouch(const int x, const int y) const {
  const int leftW = gameX;
  const int centerX = leftW / 2;
  if (leftW >= 120) {
    if (hitRect(x, y, centerX - 35, 8, 70, 58)) return TouchControl::Up;
    if (hitRect(x, y, centerX - 35, renderer.getScreenHeight() - 66, 70, 58)) return TouchControl::Down;
    if (hitRect(x, y, 4, renderer.getScreenHeight() / 2 - 40, 60, 80)) return TouchControl::Left;
    if (hitRect(x, y, leftW - 64, renderer.getScreenHeight() / 2 - 40, 60, 80)) return TouchControl::Right;
  }
  const int rightX = gameX + gameW;
  const int rightW = renderer.getScreenWidth() - rightX;
  if (rightW >= 120) {
    const int buttonW = std::min(68, (rightW - TOUCH_GAP - 8) / 2);
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_GAP;
    if (hitRect(x, y, firstX, 72, buttonW, 72)) return TouchControl::B;
    if (hitRect(x, y, secondX, 72, buttonW, 72)) return TouchControl::A;
    if (hitRect(x, y, firstX, 174, buttonW, 52)) return TouchControl::Select;
    if (hitRect(x, y, secondX, 174, buttonW, 52)) return TouchControl::Start;
    if (hitRect(x, y, rightX + 4, renderer.getScreenHeight() - 62, rightW - 8, 54))
      return TouchControl::Exit;
  }
  return TouchControl::None;
}

void NESActivity::startEmuTask() {
  emuRun.store(true, std::memory_order_release);
  emuStopped.store(false, std::memory_order_release);
  if (xTaskCreatePinnedToCore(&NESActivity::emuTaskEntry, "NESEmu", 12288, this, 1, &emuTask, 0) != pdPASS) {
    LOG_ERR(TAG, "Could not start NES emulator task");
    emuTask = nullptr;
    emuRun.store(false, std::memory_order_release);
    emuStopped.store(true, std::memory_order_release);
    showError("Could not start NES emulator");
  }
}

void NESActivity::stopEmuTask() {
  if (!emuTask) return;
  emuRun.store(false, std::memory_order_release);
  const unsigned long started = millis();
  while (!emuStopped.load(std::memory_order_acquire) && millis() - started < 1000) delay(1);
  if (!emuStopped.load(std::memory_order_acquire)) LOG_ERR(TAG, "NES emulator task did not stop in time");
#ifdef SIMULATOR
  vTaskDelete(emuTask);
#endif
  emuTask = nullptr;
}

void NESActivity::emuTaskEntry(void* arg) {
  auto* self = static_cast<NESActivity*>(arg);
  self->emuTaskLoop();
  self->emuStopped.store(true, std::memory_order_release);
#ifndef SIMULATOR
  vTaskDelete(nullptr);
#endif
}

void NESActivity::emuTaskLoop() {
  while (emuRun.load(std::memory_order_acquire)) {
    const uint64_t nowUs = nowMicros();
    runEmulation(nowUs);
    if (refreshPreset != 0 && nowUs >= nextCleanUs) {
      nextCleanUs = nowUs + CLEAN_PERIOD_US;
      fullRefreshDue.store(true, std::memory_order_release);
      displayDue.store(true, std::memory_order_release);
      requestUpdate();
    }
    delay(1);
  }
}

void NESActivity::runEmulation(const uint64_t nowUs) {
  if (!emulator || nextFrameUs == 0) nextFrameUs = nowUs;
  uint8_t ran = 0;
  while (nowUs >= nextFrameUs && ran < MAX_CATCH_UP_FRAMES) {
    const uint8_t pending = readyFrame.load(std::memory_order_acquire);
    const uint8_t rendering = renderFrame.load(std::memory_order_acquire);
    if (writeFrame == pending || writeFrame == rendering) break;

    const uint8_t mask = joypadMask.load(std::memory_order_acquire);
    agnes_input_t input{};
    input.up = (mask & JOY_UP) != 0;
    input.down = (mask & JOY_DOWN) != 0;
    input.left = (mask & JOY_LEFT) != 0;
    input.right = (mask & JOY_RIGHT) != 0;
    input.a = (mask & JOY_A) != 0;
    input.b = (mask & JOY_B) != 0;
    input.select = (mask & JOY_SELECT) != 0;
    input.start = (mask & JOY_START) != 0;
    agnes_set_input(emulator, &input, nullptr);
    agnes_next_frame(emulator);

    uint8_t* target = frameBuffers[writeFrame];
    if (!target) break;
    for (int y = 0; y < NES_HEIGHT; ++y) {
      for (int x = 0; x < NES_WIDTH; ++x) {
        target[static_cast<size_t>(y) * NES_WIDTH + x] = rgbToGrayShade(agnes_get_screen_pixel(emulator, x, y));
      }
    }
    publishCompletedFrame();
    nextFrameUs += EMULATION_FRAME_PERIOD_US;
    ++ran;
  }
  if (ran == MAX_CATCH_UP_FRAMES && nowUs >= nextFrameUs) nextFrameUs = nowUs + EMULATION_FRAME_PERIOD_US;
}

void NESActivity::publishCompletedFrame() {
  readyFrame.store(writeFrame, std::memory_order_release);
  writeFrame ^= 1u;
}

void NESActivity::releaseRenderedFrame() {
  const uint8_t claimed = renderFrame.load(std::memory_order_acquire);
  if (claimed > 1) return;
  uint8_t expected = claimed;
  readyFrame.compare_exchange_strong(expected, static_cast<uint8_t>(0xFF), std::memory_order_acq_rel,
                                     std::memory_order_acquire);
  renderFrame.store(0xFF, std::memory_order_release);
}

void NESActivity::drawGameFrame() {
  const uint8_t frameIndex = readyFrame.load(std::memory_order_acquire);
  if (frameIndex > 1) return;
  const uint8_t* source = frameBuffers[frameIndex];
  uint8_t* framebuffer = renderer.getFrameBuffer();
  if (!source || !framebuffer) return;
  renderFrame.store(frameIndex, std::memory_order_release);
  if (!drawGameFrameLandscape(source, framebuffer)) renderFrame.store(0xFF, std::memory_order_release);
}

bool NESActivity::drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer) {
  const int panelBytes = renderer.getDisplayWidthBytes();
  const int panelWidth = renderer.getDisplayWidth();
  const int rowBytes = gameW / 8;
  if (panelBytes <= 0 || panelWidth <= 0 || gameX < 0 || gameX % 8 != 0 || gameX + gameW > panelWidth ||
      gameY < 0 || gameY + gameH > renderer.getDisplayHeight() || rowBytes > MAX_GAME_ROW_BYTES) {
    return false;
  }

  uint8_t patterns[2][4][2];
  gamePatterns(patterns, scale);
  uint8_t rows[2][MAX_GAME_ROW_BYTES];
  for (int gameY0 = 0; gameY0 < NES_HEIGHT; ++gameY0) {
    const uint8_t* line = source + static_cast<size_t>(gameY0) * NES_WIDTH;
    for (int parity = 0; parity < 2; ++parity) {
      buildGameRow(rows[parity], line, 1, NES_WIDTH, scale, patterns[parity]);
    }
    for (int sy = 0; sy < scale; ++sy) {
      const int panelY = gameY + gameY0 * scale + sy;
      uint8_t* out = framebuffer + static_cast<size_t>(panelY) * panelBytes + gameX / 8;
      std::memcpy(out, rows[panelY & 1], static_cast<size_t>(rowBytes));
    }
  }
  return true;
}

void NESActivity::drawControls() {
  renderer.drawRect(gameX - 1, gameY - 1, gameW + 2, gameH + 2, true);
  renderer.drawText(UI_10_FONT_ID, 8, 4, "NES", true, EpdFontFamily::BOLD);
  const int leftW = gameX;
  if (leftW >= 120) {
    auto draw = [this](int x, int y, int w, int h, const char* label, bool active) {
      renderer.fillRect(x + 1, y + 1, w - 2, h - 2, active);
      renderer.drawRect(x, y, w, h, true);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
      renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, y + h / 2 - 7, label, !active, EpdFontFamily::BOLD);
    };
    const int cx = leftW / 2;
    const int cy = renderer.getScreenHeight() / 2;
    draw(cx - 35, 8, 70, 58, "UP", activeTouch == TouchControl::Up);
    draw(cx - 35, renderer.getScreenHeight() - 66, 70, 58, "DOWN", activeTouch == TouchControl::Down);
    draw(4, cy - 40, 60, 80, "LEFT", activeTouch == TouchControl::Left);
    draw(leftW - 64, cy - 40, 60, 80, "RIGHT", activeTouch == TouchControl::Right);
  }
  const int rightX = gameX + gameW;
  const int rightW = renderer.getScreenWidth() - rightX;
  if (rightW >= 120) {
    const int buttonW = std::min(68, (rightW - TOUCH_GAP - 8) / 2);
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_GAP;
    auto draw = [this](int x, int y, int w, int h, const char* label, const TouchControl control) {
      const bool active = activeTouch == control;
      renderer.fillRect(x + 1, y + 1, w - 2, h - 2, active);
      renderer.drawRect(x, y, w, h, true);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
      renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, y + h / 2 - 7, label, !active, EpdFontFamily::BOLD);
    };
    draw(firstX, 72, buttonW, 72, "B", TouchControl::B);
    draw(secondX, 72, buttonW, 72, "A", TouchControl::A);
    draw(firstX, 174, buttonW, 52, "SELECT", TouchControl::Select);
    draw(secondX, 174, buttonW, 52, "START", TouchControl::Start);
    draw(rightX + 4, renderer.getScreenHeight() - 62, rightW - 8, 54, "EXIT", TouchControl::Exit);
  }
}

void NESActivity::registerTouchTargets() {
#if CROSSINK_APP_CAP_TOUCH
  auto& registry = TouchRegistry::getInstance();
  const int leftW = gameX;
  if (leftW >= 120) {
    const int cx = leftW / 2;
    const int cy = renderer.getScreenHeight() / 2;
    registry.add(Rect(cx - 35, 8, 70, 58), static_cast<int>(TouchControl::Up), TouchRegistry::Kind::Button);
    registry.add(Rect(cx - 35, renderer.getScreenHeight() - 66, 70, 58), static_cast<int>(TouchControl::Down), TouchRegistry::Kind::Button);
    registry.add(Rect(4, cy - 40, 60, 80), static_cast<int>(TouchControl::Left), TouchRegistry::Kind::Button);
    registry.add(Rect(leftW - 64, cy - 40, 60, 80), static_cast<int>(TouchControl::Right), TouchRegistry::Kind::Button);
  }
  const int rightX = gameX + gameW;
  const int rightW = renderer.getScreenWidth() - rightX;
  if (rightW >= 120) {
    const int buttonW = std::min(68, (rightW - TOUCH_GAP - 8) / 2);
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_GAP;
    registry.add(Rect(firstX, 72, buttonW, 72), static_cast<int>(TouchControl::B), TouchRegistry::Kind::Button);
    registry.add(Rect(secondX, 72, buttonW, 72), static_cast<int>(TouchControl::A), TouchRegistry::Kind::Button);
    registry.add(Rect(firstX, 174, buttonW, 52), static_cast<int>(TouchControl::Select), TouchRegistry::Kind::Button);
    registry.add(Rect(secondX, 174, buttonW, 52), static_cast<int>(TouchControl::Start), TouchRegistry::Kind::Button);
    registry.add(Rect(rightX + 4, renderer.getScreenHeight() - 62, rightW - 8, 54),
                 static_cast<int>(TouchControl::Exit), TouchRegistry::Kind::Button);
  }
#endif
}

bool NESActivity::fastRefreshValidated() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279 &&
         BoardConfig::ACTIVE.displayControllerVariant == 0x68;
#endif
}

void NESActivity::applyRefreshPreset() {
#ifndef SIMULATOR
  // Same validated profile as the latest Ink-boy Game Boy implementation:
  // PLL 0x0F, host-loaded DU waveform on 40% of frames, differential window,
  // and 20 MHz SPI. Other panel variants keep stock refresh.
  RenderLock lock(*this);
  display.finishOverlapped();
  display.setFastRefreshTuning(0, 0x0F);
  display.setFastRefreshDuScale(40);
  display.setFastRefreshDiffWindow(true);
  display.setDisplaySpiHz(20000000);
#endif
  nextCleanUs = nowMicros() + CLEAN_PERIOD_US;
  fullRefreshDue.store(true, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

#endif  // CROSSINK_AGNES
