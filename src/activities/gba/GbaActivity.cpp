#include "GbaActivity.h"

#if CROSSINK_GBA

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <GpspGBA.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "components/TouchRegistry.h"

namespace {
constexpr char TAG[] = "GBA";

uint64_t nowMicros() { return static_cast<uint64_t>(esp_timer_get_time()); }

bool hitRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Convert RGB565 to one of four stable grayscale levels before 1-bit
// threshold dithering. The game core remains color internally.
uint8_t rgb565ToShade(uint16_t pixel) {
  const uint32_t r5 = (pixel >> 11) & 0x1Fu;
  const uint32_t g6 = (pixel >> 5) & 0x3Fu;
  const uint32_t b5 = pixel & 0x1Fu;
  const uint32_t r = (r5 * 255u + 15u) / 31u;
  const uint32_t g = (g6 * 255u + 31u) / 63u;
  const uint32_t b = (b5 * 255u + 15u) / 31u;
  const uint32_t luminance = (r * 299u + g * 587u + b * 114u) / 1000u;
  if (luminance >= 192u) return 0;  // white
  if (luminance >= 128u) return 1;  // light gray
  if (luminance >= 64u) return 2;   // dark gray
  return 3;                         // black
}

bool shadeIsBlack(uint8_t shade, int x, int y) {
  switch (shade) {
    case 3: return true;
    case 2: return ((x + y) & 1) == 0;
    case 1: return (x & 1) == 0 && (y & 1) == 0;
    default: return false;
  }
}

uint64_t hashBytes(const uint8_t* data, size_t size) {
  if (!data || size == 0) return 0;
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}
}  // namespace

GbaActivity::GbaActivity(GfxRenderer& rendererIn, MappedInputManager& mappedInputIn, std::string romPathIn)
    : Activity("GBA", rendererIn, mappedInputIn), romPath(std::move(romPathIn)) {}

GbaActivity::~GbaActivity() { cleanup(); }

void GbaActivity::onEnter() {
  Activity::onEnter();
  stopEmuTask();
  cleanup();

  exitRequested = false;
  backWasPressed = false;
  backPressedAtMs = 0;
  rotateRequested = false;
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  joypadMask.store(0, std::memory_order_release);
  activeTouch = TouchControl::None;
  nextFrameUs = 0;
  nextDisplayUs = 0;
  nextCleanUs = 0;
  nextSaveCheckUs = 0;
  refreshPreset = 0;
  lastSaveUs = 0;
  lastSavedHash = lastObservedHash = 0;
  lastSavedSize = lastObservedSize = 0;
  saveDirty = false;
  displayDue.store(true, std::memory_order_release);
  fullRefreshDue.store(false, std::memory_order_release);

  portrait = true;
  renderer.setOrientation(GfxRenderer::Portrait);
  setLayout();
  renderer.clearScreen(0xFF);

  frameBuffers[0] = static_cast<uint8_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frameBuffers[1] = static_cast<uint8_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  lastFrame = static_cast<uint8_t*>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frameBuffers[0] || !frameBuffers[1] || !lastFrame) {
    showError("Not enough PSRAM for GBA frames");
    cleanup();
    return;
  }
  // Shade 0 is white, so the first picture is a clean white frame.
  std::memset(frameBuffers[0], 0, FRAME_BYTES);
  std::memset(frameBuffers[1], 0, FRAME_BYTES);
  std::memset(lastFrame, 0, FRAME_BYTES);

  if (romPath.empty() || !buildSavePaths()) {
    showError("No GBA ROM path");
    return;
  }
  if (!gpsp::initialize(romPath.c_str(), &GbaActivity::videoRefresh, this)) {
    showError(gpsp::lastError());
    return;
  }
  coreStarted = true;
  if (!loadSave()) LOG_ERR(TAG, "Couldn't load cartridge save: %s", savePath.c_str());

  initialized.store(true, std::memory_order_release);
  const uint64_t now = nowMicros();
  nextFrameUs = now;
  nextDisplayUs = now + STOCK_DISPLAY_PERIOD_US;
  nextCleanUs = now + CLEAN_PERIOD_US;
  nextSaveCheckUs = now + SAVE_CHECK_PERIOD_US;
  lastSaveUs = now;
  if (fastRefreshValidated()) {
    refreshPreset = 1;
    applyRefreshPreset();
    nextDisplayUs = now + FAST_DISPLAY_PERIOD_US;
  }
  startEmuTask();
  requestUpdate(true);
  LOG_INF(TAG, "Started GBA ROM %s", romPath.c_str());
}

void GbaActivity::onExit() {
  stopEmuTask();
  if (coreStarted && !saveCartRam(true)) LOG_ERR(TAG, "Final GBA save failed: %s", savePath.c_str());
  TouchRegistry::getInstance().clear();
  if (refreshPreset != 0) {
    refreshPreset = 0;
#ifndef SIMULATOR
    display.finishOverlapped();
    display.setFastRefreshTuning(0, 0);
    display.setFastRefreshDuScale(0);
    display.setFastRefreshDiffWindow(false);
    display.setDisplaySpiHz(0);
#endif
  }
  cleanup();
  renderer.setOrientation(GfxRenderer::Portrait);
  Activity::onExit();
}

void GbaActivity::loop() {
  handleInput();
  if (rotateRequested) {
    rotateRequested = false;
    rotate();
  }
  if (exitRequested) {
    const bool backHeld = backWasPressed;
    mappedInput.suppressCurrentTouchContact();
    if (backHeld) finishAfterBackPress();
    else finish();
    return;
  }

  const uint64_t now = nowMicros();
  if (now >= nextDisplayUs) {
    nextDisplayUs = now + (refreshPreset != 0 ? FAST_DISPLAY_PERIOD_US : STOCK_DISPLAY_PERIOD_US);
    displayDue.store(true, std::memory_order_release);
    requestUpdate();
  }
  if (refreshPreset != 0 && now >= nextCleanUs) {
    nextCleanUs = now + CLEAN_PERIOD_US;
    fullRefreshDue.store(true, std::memory_order_release);
    displayDue.store(true, std::memory_order_release);
    requestUpdate();
  }
  delay(2);
}

void GbaActivity::render(RenderLock&&) {
  if (!displayDue.exchange(false, std::memory_order_acq_rel)) return;
  const bool cleaning = fullRefreshDue.exchange(false, std::memory_order_acq_rel);

  // No title, counters, alerts, or button letters are drawn. The gameplay
  // framebuffer is just the dithered game image; all controls live outside it.
  renderer.clearScreen(0xFF);
  if (initialized.load(std::memory_order_acquire)) drawGameFrame();
  drawControls();
  registerTouchTargets();
#ifdef SIMULATOR
  const bool overlap = false;
#else
  const bool overlap = !cleaning && refreshPreset != 0 && renderer.supportsAsyncRefresh();
#endif
  if (overlap) {
#ifndef SIMULATOR
    display.displayBufferOverlapped(HalDisplay::FAST_REFRESH);
#endif
  } else {
    renderer.displayBuffer(cleaning ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  }
}

bool GbaActivity::handleTwoFingerRotation(bool) {
  rotateRequested = true;
  return true;
}

void GbaActivity::setLayout() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  controlCount = 0;
  scale = std::max(1, std::min(MAX_SCALE, screenW / GBA_WIDTH));
  gameW = GBA_WIDTH * scale;
  gameH = GBA_HEIGHT * scale;
  gameX = (screenW - gameW) / 2;

  if (portrait) {
    // The image takes the top 3:2 area (480 x 320 at scale 2). All touch
    // targets are deliberately below it, never on top of gameplay pixels.
    gameY = 24;
    const int buttonSize = 54;
    const int padX = 18;
    const int padY = gameY + gameH + 76;
    addControl(TouchControl::Up, padX + buttonSize, padY, buttonSize, buttonSize);
    addControl(TouchControl::Left, padX, padY + buttonSize, buttonSize, buttonSize);
    addControl(TouchControl::Right, padX + 2 * buttonSize, padY + buttonSize, buttonSize, buttonSize);
    addControl(TouchControl::Down, padX + buttonSize, padY + 2 * buttonSize, buttonSize, buttonSize);

    addControl(TouchControl::L, 14, gameY + gameH + 10, 76, 34);
    addControl(TouchControl::R, screenW - 90, gameY + gameH + 10, 76, 34);
    addControl(TouchControl::B, screenW - 198, padY + 76, 68, 68);
    addControl(TouchControl::A, screenW - 104, padY + 14, 68, 68);

    const int rowY = screenH - 172;
    addControl(TouchControl::Select, screenW / 2 - 92, rowY, 80, 44);
    addControl(TouchControl::Start, screenW / 2 + 12, rowY, 80, 44);
    addControl(TouchControl::Exit, screenW / 2 - 92, screenH - 78, 80, 48);
    addControl(TouchControl::Rotate, screenW / 2 + 12, screenH - 78, 80, 48);
    return;
  }

  // Landscape reserves the two 160-pixel side columns for controls. The 480 x
  // 320 gameplay rectangle stays centered and no control's hitbox overlaps it.
  gameY = std::max(0, (screenH - gameH) / 2);
  const int leftCenter = gameX / 2;
  const int padSize = 34;
  const int padY = gameY + 94;
  const int padX = leftCenter - (3 * padSize) / 2;
  addControl(TouchControl::Up, padX + padSize, padY, padSize, padSize);
  addControl(TouchControl::Left, padX, padY + padSize, padSize, padSize);
  addControl(TouchControl::Right, padX + 2 * padSize, padY + padSize, padSize, padSize);
  addControl(TouchControl::Down, padX + padSize, padY + 2 * padSize, padSize, padSize);

  addControl(TouchControl::L, 8, 22, 64, 36);
  addControl(TouchControl::Select, 8, screenH - 54, 68, 42);
  addControl(TouchControl::Rotate, 84, screenH - 54, 68, 42);
  addControl(TouchControl::R, screenW - 72, 22, 64, 36);
  addControl(TouchControl::B, gameX + gameW + 10, gameY + 88, 62, 62);
  addControl(TouchControl::A, gameX + gameW + 82, gameY + 144, 62, 62);
  addControl(TouchControl::Start, gameX + gameW + 8, screenH - 54, 68, 42);
  addControl(TouchControl::Exit, screenW - 72, screenH - 54, 64, 42);
}

void GbaActivity::addControl(TouchControl control, int x, int y, int w, int h) {
  if (controlCount >= MAX_CONTROLS) return;
  controls[controlCount++] = {control, x, y, w, h};
}

void GbaActivity::rotate() {
  mappedInput.suppressCurrentTouchContact();
  activeTouch = TouchControl::None;
  touchJoypadMask = 0;
  applyJoypad();
  {
    RenderLock lock(*this);
#ifndef SIMULATOR
    if (refreshPreset != 0) display.finishOverlapped();
#endif
    portrait = !portrait;
    renderer.setOrientation(portrait ? GfxRenderer::Portrait : GfxRenderer::LandscapeCounterClockwise);
    setLayout();
  }
  fullRefreshDue.store(true, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  nextDisplayUs = nowMicros();
  requestUpdate();
}

bool GbaActivity::buildSavePaths() {
  if (romPath.empty()) return false;
  const size_t slash = romPath.find_last_of('/');
  const size_t dot = romPath.find_last_of('.');
  const size_t stemEnd = (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                             ? dot
                             : romPath.size();
  savePath = romPath.substr(0, stemEnd) + ".sav";
  tempSavePath = savePath + ".tmp";
  backupSavePath = savePath + ".bak";
  return true;
}

bool GbaActivity::loadSave() {
  uint8_t* data = gpsp::batteryRamData();
  const size_t size = gpsp::batteryRamSize();
  if (!data || size == 0) {
    lastSavedSize = lastObservedSize = 0;
    lastSavedHash = lastObservedHash = 0;
    saveDirty = false;
    return true;
  }

  // Unwritten cartridge storage is erased/blank (0xFF), as on the cartridge.
  std::memset(data, 0xFF, size);
  HalFile file;
  if (Storage.openFileForRead(TAG, savePath, file)) {
    const uint64_t fileSize = file.fileSize64();
    const size_t limit = static_cast<size_t>(std::min<uint64_t>(fileSize, size));
    size_t offset = 0;
    while (offset < limit) {
      const int got = file.read(data + offset, limit - offset);
      if (got <= 0) break;
      offset += static_cast<size_t>(got);
    }
    file.close();
    // Match gpSP's own normalization of empty/all-zero save files.
    bool allZero = offset > 0;
    for (size_t i = 0; i < offset; ++i) {
      if (data[i] != 0) { allZero = false; break; }
    }
    if (allZero) std::memset(data, 0xFF, size);
    LOG_INF(TAG, "Loaded GBA save %u/%u bytes", static_cast<unsigned>(offset), static_cast<unsigned>(size));
  }

  lastSavedHash = lastObservedHash = hashBytes(data, size);
  lastSavedSize = lastObservedSize = size;
  saveDirty = false;
  return true;
}

uint64_t GbaActivity::hashSaveData(size_t& size) const {
  const uint8_t* data = gpsp::batteryRamData();
  size = gpsp::batteryRamSize();
  return hashBytes(data, size);
}

void GbaActivity::checkSaveData(uint64_t nowUs) {
  size_t size = 0;
  const uint64_t hash = hashSaveData(size);
  if (size != lastObservedSize || hash != lastObservedHash) {
    lastObservedSize = size;
    lastObservedHash = hash;
    saveDirty = size != 0;
  }
  if (saveDirty && nowUs - lastSaveUs >= AUTOSAVE_PERIOD_US) {
    if (!saveCartRam(false)) LOG_ERR(TAG, "GBA autosave failed: %s", savePath.c_str());
    lastSaveUs = nowUs;
  }
}

bool GbaActivity::saveCartRam(bool force) {
  if (!coreStarted) return true;
  uint8_t* data = gpsp::batteryRamData();
  const size_t size = gpsp::batteryRamSize();
  if (!data || size == 0) {
    saveDirty = false;
    return true;
  }
  const uint64_t hash = hashBytes(data, size);
  if (size == lastSavedSize && hash == lastSavedHash) {
    saveDirty = false;
    return true;
  }
  if (!force && !saveDirty) return true;

  HalFile file;
  if (!Storage.openFileForWrite(TAG, tempSavePath, file)) {
    LOG_ERR(TAG, "Unable to open GBA save temp: %s", tempSavePath.c_str());
    return false;
  }
  const size_t written = file.write(data, size);
  const bool synced = written == size && file.sync();
  file.close();
  if (!synced) {
    Storage.remove(tempSavePath.c_str());
    LOG_ERR(TAG, "GBA save write incomplete (%u/%u)", static_cast<unsigned>(written), static_cast<unsigned>(size));
    return false;
  }

  const bool hadExisting = Storage.exists(savePath.c_str());
  if (hadExisting) {
    Storage.remove(backupSavePath.c_str());
    if (!Storage.rename(savePath.c_str(), backupSavePath.c_str())) {
      Storage.remove(tempSavePath.c_str());
      return false;
    }
  }
  if (!Storage.rename(tempSavePath.c_str(), savePath.c_str())) {
    if (hadExisting) Storage.rename(backupSavePath.c_str(), savePath.c_str());
    Storage.remove(tempSavePath.c_str());
    return false;
  }
  if (hadExisting) Storage.remove(backupSavePath.c_str());

  lastSavedHash = lastObservedHash = hash;
  lastSavedSize = lastObservedSize = size;
  saveDirty = false;
  LOG_INF(TAG, "Saved GBA cartridge RAM: %s", savePath.c_str());
  return true;
}

void GbaActivity::cleanup() {
  initialized.store(false, std::memory_order_release);
  if (coreStarted) {
    gpsp::shutdown();
    coreStarted = false;
  }
  for (auto*& buffer : frameBuffers) {
    if (buffer) heap_caps_free(buffer);
    buffer = nullptr;
  }
  if (lastFrame) heap_caps_free(lastFrame);
  lastFrame = nullptr;
  writeFrame = 0;
  readyFrame.store(0xFF, std::memory_order_release);
  renderFrame.store(0xFF, std::memory_order_release);
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  joypadMask.store(0, std::memory_order_release);
  activeTouch = TouchControl::None;
  saveDirty = false;
}

void GbaActivity::showError(const char* message) {
  // Intentionally no on-screen diagnostic text: keep the game display free of
  // overlays. Details are available over serial logs while the screen stays
  // blank and the icon-only exit/rotate controls remain usable.
  LOG_ERR(TAG, "%s: %s", romPath.c_str(), message ? message : "GBA initialization failed");
  initialized.store(false, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

void GbaActivity::handleInput() {
  using Button = MappedInputManager::Button;
  uint16_t nextPhysical = 0;
  const Button buttons[] = {Button::Up, Button::Down, Button::Left, Button::Right, Button::Confirm,
                            Button::Back, Button::Power, Button::PageBack, Button::PageForward};
  for (const Button button : buttons) {
    if (mappedInput.isPressed(button)) nextPhysical |= joypadBitForButton(button);
  }

  if (mappedInput.isPressed(Button::Back)) {
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
    displayDue.store(true, std::memory_order_release);
    requestUpdate();
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
    displayDue.store(true, std::memory_order_release);
    requestUpdate();
  }
#endif
}

void GbaActivity::updateTouch(int x, int y) {
  const TouchControl control = hitTestTouch(x, y);
  if (control == activeTouch) return;
  activeTouch = control;
  if (control == TouchControl::Exit) {
    exitRequested = true;
    return;
  }
  if (control == TouchControl::Rotate) {
    rotateRequested = true;
    return;
  }
  touchJoypadMask = joypadBitForTouch(control);
  applyJoypad();
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

GbaActivity::TouchControl GbaActivity::hitTestTouch(int x, int y) const {
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& control = controls[i];
    if (hitRect(x, y, control.x, control.y, control.w, control.h)) return control.control;
  }
  return TouchControl::None;
}

uint16_t GbaActivity::joypadBitForTouch(TouchControl control) {
  switch (control) {
    case TouchControl::Up: return JOY_UP;
    case TouchControl::Down: return JOY_DOWN;
    case TouchControl::Left: return JOY_LEFT;
    case TouchControl::Right: return JOY_RIGHT;
    case TouchControl::A: return JOY_A;
    case TouchControl::B: return JOY_B;
    case TouchControl::L: return JOY_L;
    case TouchControl::R: return JOY_R;
    case TouchControl::Select: return JOY_SELECT;
    case TouchControl::Start: return JOY_START;
    default: return 0;
  }
}

uint16_t GbaActivity::joypadBitForButton(MappedInputManager::Button button) {
  switch (button) {
    case MappedInputManager::Button::Up: return JOY_UP;
    case MappedInputManager::Button::Down: return JOY_DOWN;
    case MappedInputManager::Button::Left: return JOY_LEFT;
    case MappedInputManager::Button::Right: return JOY_RIGHT;
    case MappedInputManager::Button::Confirm: return JOY_A;
    case MappedInputManager::Button::Back: return JOY_B;
    case MappedInputManager::Button::Power: return JOY_START;
    case MappedInputManager::Button::PageBack: return JOY_L;
    case MappedInputManager::Button::PageForward: return JOY_R;
    default: return 0;
  }
}

void GbaActivity::applyJoypad() {
  joypadMask.store(static_cast<uint16_t>(physicalJoypadMask | touchJoypadMask), std::memory_order_release);
}

void GbaActivity::startEmuTask() {
  emuRun.store(true, std::memory_order_release);
  emuStopped.store(false, std::memory_order_release);
  if (xTaskCreatePinnedToCore(&GbaActivity::emuTaskEntry, "GBAEmu", 16384, this, 1, &emuTask, 0) != pdPASS) {
    emuTask = nullptr;
    emuRun.store(false, std::memory_order_release);
    emuStopped.store(true, std::memory_order_release);
    showError("Could not start GBA emulator task");
  }
}

void GbaActivity::stopEmuTask() {
  if (!emuTask) return;
  emuRun.store(false, std::memory_order_release);
  const unsigned long start = millis();
  while (!emuStopped.load(std::memory_order_acquire) && millis() - start < 2000) delay(1);
  if (!emuStopped.load(std::memory_order_acquire)) {
    // Do not release gpSP memory while the core task may still be touching it.
    LOG_ERR(TAG, "GBA task did not stop; waiting for a safe shutdown");
    while (!emuStopped.load(std::memory_order_acquire)) delay(1);
  }
#ifdef SIMULATOR
  vTaskDelete(emuTask);
#endif
  emuTask = nullptr;
}

void GbaActivity::emuTaskEntry(void* arg) {
  auto* self = static_cast<GbaActivity*>(arg);
  self->emuTaskLoop();
  self->emuStopped.store(true, std::memory_order_release);
#ifndef SIMULATOR
  vTaskDelete(nullptr);
#endif
}

void GbaActivity::emuTaskLoop() {
  uint64_t now = nowMicros();
  nextFrameUs = now;
  nextSaveCheckUs = now + SAVE_CHECK_PERIOD_US;
  while (emuRun.load(std::memory_order_acquire)) {
    now = nowMicros();
    if (now >= nextFrameUs) {
      gpsp::runFrame(joypadMask.load(std::memory_order_acquire));
      nextFrameUs += FRAME_PERIOD_US;
      const uint64_t after = nowMicros();
      if (after > nextFrameUs + FRAME_PERIOD_US * 3) nextFrameUs = after + FRAME_PERIOD_US;
    }
    now = nowMicros();
    if (now >= nextSaveCheckUs) {
      nextSaveCheckUs = now + SAVE_CHECK_PERIOD_US;
      checkSaveData(now);
    }
    delay(1);
  }
}

void GbaActivity::videoRefresh(void* user, const uint16_t* pixels, unsigned width,
                               unsigned height, size_t pitchBytes) {
  auto* self = static_cast<GbaActivity*>(user);
  if (self) self->onVideoFrame(pixels, width, height, pitchBytes);
}

void GbaActivity::onVideoFrame(const uint16_t* pixels, unsigned width, unsigned height, size_t pitchBytes) {
  if (!pixels || width != GBA_WIDTH || height != GBA_HEIGHT || pitchBytes < GBA_WIDTH * sizeof(uint16_t)) return;
  const uint8_t pending = readyFrame.load(std::memory_order_acquire);
  const uint8_t rendering = renderFrame.load(std::memory_order_acquire);
  if (writeFrame == pending || writeFrame == rendering) return;  // Drop this video update, not an emulation frame.

  uint8_t* target = frameBuffers[writeFrame];
  if (!target) return;
  const auto* src = reinterpret_cast<const uint8_t*>(pixels);
  for (int y = 0; y < GBA_HEIGHT; ++y) {
    const auto* line = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y) * pitchBytes);
    uint8_t* out = target + static_cast<size_t>(y) * GBA_WIDTH;
    for (int x = 0; x < GBA_WIDTH; ++x) out[x] = rgb565ToShade(line[x]);
  }
  publishCompletedFrame();
}

void GbaActivity::publishCompletedFrame() {
  readyFrame.store(writeFrame, std::memory_order_release);
  writeFrame ^= 1u;
}

void GbaActivity::releaseRenderedFrame() {
  renderFrame.store(0xFF, std::memory_order_release);
}

void GbaActivity::drawGameFrame() {
  const uint8_t index = readyFrame.exchange(0xFF, std::memory_order_acq_rel);
  if (index < 2) {
    renderFrame.store(index, std::memory_order_release);
    std::memcpy(lastFrame, frameBuffers[index], FRAME_BYTES);
    releaseRenderedFrame();
  }
  if (!lastFrame) return;
  const bool ok = portrait ? drawGameFramePortrait(lastFrame, renderer.getFrameBuffer())
                           : drawGameFrameLandscape(lastFrame, renderer.getFrameBuffer());
  if (!ok) LOG_ERR(TAG, "GBA frame placement exceeds framebuffer bounds");
}

bool GbaActivity::drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer) {
  if (!source || !framebuffer) return false;
  const int stride = renderer.getDisplayWidthBytes();
  const int panelW = renderer.getDisplayWidth();
  const int panelH = renderer.getDisplayHeight();
  if (stride <= 0 || panelW <= 0 || panelH <= 0 || gameX < 0 || gameX % 8 != 0 ||
      gameX + gameW > panelW || gameY < 0 || gameY + gameH > panelH) return false;

  for (int gy = 0; gy < GBA_HEIGHT; ++gy) {
    for (int sy = 0; sy < scale; ++sy) {
      const int logicalY = gameY + gy * scale + sy;
      uint8_t* row = framebuffer + static_cast<size_t>(logicalY) * stride;
      for (int gx = 0; gx < GBA_WIDTH; ++gx) {
        const uint8_t shade = source[static_cast<size_t>(gy) * GBA_WIDTH + gx];
        for (int sx = 0; sx < scale; ++sx) {
          const int logicalX = gameX + gx * scale + sx;
          if (shadeIsBlack(shade, logicalX, logicalY))
            row[logicalX / 8] &= static_cast<uint8_t>(~(1u << (7 - (logicalX & 7))));
        }
      }
    }
  }
  return true;
}

bool GbaActivity::drawGameFramePortrait(const uint8_t* source, uint8_t* framebuffer) {
  if (!source || !framebuffer) return false;
  const int stride = renderer.getDisplayWidthBytes();
  const int panelW = renderer.getDisplayWidth();
  const int panelH = renderer.getDisplayHeight();
  if (stride <= 0 || panelW <= 0 || panelH <= 0 || gameY % 8 != 0 || gameH % 8 != 0 ||
      gameX < 0 || gameX + gameW > panelH || gameY < 0 || gameY + gameH > panelW) return false;

  // Logical portrait (x,y) maps to physical (y, panelHeight - 1 - x).
  // Game rows therefore occupy adjacent bits in each physical framebuffer row.
  for (int gx = 0; gx < GBA_WIDTH; ++gx) {
    for (int sx = 0; sx < scale; ++sx) {
      const int logicalX = gameX + gx * scale + sx;
      const int physicalY = panelH - 1 - logicalX;
      if (physicalY < 0 || physicalY >= panelH) return false;
      uint8_t* row = framebuffer + static_cast<size_t>(physicalY) * stride;
      for (int gy = 0; gy < GBA_HEIGHT; ++gy) {
        const uint8_t shade = source[static_cast<size_t>(gy) * GBA_WIDTH + gx];
        for (int sy = 0; sy < scale; ++sy) {
          const int logicalY = gameY + gy * scale + sy;
          if (!shadeIsBlack(shade, logicalX, logicalY)) continue;
          row[logicalY / 8] &= static_cast<uint8_t>(~(1u << (7 - (logicalY & 7))));
        }
      }
    }
  }
  return true;
}

void GbaActivity::drawControls() {
  // Border only; all controls are outside this rectangle in both layouts.
  renderer.drawRect(gameX, gameY, gameW, gameH, true);
  const uint16_t activeMask = static_cast<uint16_t>(physicalJoypadMask | touchJoypadMask);
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& control = controls[i];
    const uint16_t bit = joypadBitForTouch(control.control);
    const bool active = bit ? ((activeMask & bit) != 0) : (activeTouch == control.control);
    renderer.fillRect(control.x + 2, control.y + 2, control.w - 4, control.h - 4, active);
    renderer.drawRect(control.x, control.y, control.w, control.h, true);
    drawControlIcon(control, !active);
  }
}

void GbaActivity::drawControlIcon(const ControlButton& c, bool ink) {
  const int cx = c.x + c.w / 2;
  const int cy = c.y + c.h / 2;
  const int r = std::max(5, std::min(c.w, c.h) / 5);
  switch (c.control) {
    case TouchControl::Up:
      renderer.drawLine(cx, cy - r, cx - r, cy + r / 2, ink);
      renderer.drawLine(cx, cy - r, cx + r, cy + r / 2, ink);
      renderer.drawLine(cx - r, cy + r / 2, cx + r, cy + r / 2, ink);
      break;
    case TouchControl::Down:
      renderer.drawLine(cx, cy + r, cx - r, cy - r / 2, ink);
      renderer.drawLine(cx, cy + r, cx + r, cy - r / 2, ink);
      renderer.drawLine(cx - r, cy - r / 2, cx + r, cy - r / 2, ink);
      break;
    case TouchControl::Left:
      renderer.drawLine(cx - r, cy, cx + r / 2, cy - r, ink);
      renderer.drawLine(cx - r, cy, cx + r / 2, cy + r, ink);
      renderer.drawLine(cx + r / 2, cy - r, cx + r / 2, cy + r, ink);
      break;
    case TouchControl::Right:
      renderer.drawLine(cx + r, cy, cx - r / 2, cy - r, ink);
      renderer.drawLine(cx + r, cy, cx - r / 2, cy + r, ink);
      renderer.drawLine(cx - r / 2, cy - r, cx - r / 2, cy + r, ink);
      break;
    case TouchControl::A:
      renderer.drawLine(cx, cy - r, cx + r, cy, ink);
      renderer.drawLine(cx + r, cy, cx, cy + r, ink);
      renderer.drawLine(cx, cy + r, cx - r, cy, ink);
      renderer.drawLine(cx - r, cy, cx, cy - r, ink);
      break;
    case TouchControl::B:
      renderer.drawRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, ink);
      renderer.drawRect(cx - r / 2, cy - r / 2, r + 1, r + 1, ink);
      break;
    case TouchControl::L:
      renderer.drawLine(c.x + 10, cy, c.x + c.w - 10, cy, ink);
      break;
    case TouchControl::R:
      renderer.drawLine(c.x + 10, cy - 3, c.x + c.w - 10, cy - 3, ink);
      renderer.drawLine(c.x + 10, cy + 3, c.x + c.w - 10, cy + 3, ink);
      break;
    case TouchControl::Select:
      renderer.fillRect(cx - 9, cy - 2, 5, 5, ink);
      renderer.fillRect(cx + 4, cy - 2, 5, 5, ink);
      break;
    case TouchControl::Start:
      renderer.drawLine(cx - r, cy - r, cx + r, cy, ink);
      renderer.drawLine(cx + r, cy, cx - r, cy + r, ink);
      renderer.drawLine(cx - r, cy + r, cx - r, cy - r, ink);
      break;
    case TouchControl::Exit:
      renderer.drawLine(cx - r, cy - r, cx + r, cy + r, ink);
      renderer.drawLine(cx + r, cy - r, cx - r, cy + r, ink);
      break;
    case TouchControl::Rotate:
      renderer.drawLine(cx - r, cy - r + 2, cx + r / 2, cy - r + 2, ink);
      renderer.drawLine(cx + r / 2, cy - r + 2, cx + r / 2, cy - r / 2, ink);
      renderer.drawLine(cx + r / 2, cy - r / 2, cx + r, cy - r, ink);
      renderer.drawLine(cx + r, cy - r, cx + r, cy + r, ink);
      renderer.drawLine(cx + r, cy + r, cx - r / 2, cy + r, ink);
      renderer.drawLine(cx - r / 2, cy + r, cx - r / 2, cy + r / 2, ink);
      renderer.drawLine(cx - r / 2, cy + r / 2, cx - r, cy + r, ink);
      break;
    default: break;
  }
}

void GbaActivity::registerTouchTargets() {
#if CROSSINK_APP_CAP_TOUCH
  auto& registry = TouchRegistry::getInstance();
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& c = controls[i];
    registry.add(Rect(c.x, c.y, c.w, c.h), static_cast<int>(c.control), TouchRegistry::Kind::Button);
  }
#endif
}

bool GbaActivity::fastRefreshValidated() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279 &&
         BoardConfig::ACTIVE.displayControllerVariant == 0x68;
#endif
}

void GbaActivity::applyRefreshPreset() {
#ifndef SIMULATOR
  // This is the same validated X4 Pro profile used by the existing NES/GB
  // activities. Other panel/controller variants retain stock refresh.
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

#endif  // CROSSINK_GBA
