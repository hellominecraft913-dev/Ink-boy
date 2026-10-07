#include "GameBoyActivity.h"

#if CROSSINK_GAMEBOY

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

#include "components/TouchRegistry.h"
#include "fontIds.h"

namespace {
constexpr char TAG[] = "GAMEBOY";
constexpr size_t ROM_HEADER_SIZE = 0x150;
constexpr size_t PEANUT_ROM_BANK_BYTES = 0x4000;
constexpr int TOUCH_TARGET_GAP = 4;

bool isBatteryBackedCartType(const uint8_t type) {
  switch (type) {
    case 0x03:  // MBC1 + RAM + BATTERY
    case 0x06:  // MBC2 + BATTERY
    case 0x09:  // ROM + RAM + BATTERY
    case 0x0F:  // MBC3 + TIMER + BATTERY
    case 0x10:  // MBC3 + TIMER + RAM + BATTERY
    case 0x13:  // MBC3 + RAM + BATTERY
    case 0x1B:  // MBC5 + RAM + BATTERY
    case 0x1E:  // MBC7 + RAM + BATTERY (rejected by Peanut-GB)
      return true;
    default:
      return false;
  }
}

bool hitRect(const int x, const int y, const int rx, const int ry, const int rw, const int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

}  // namespace

GameBoyActivity::GameBoyActivity(GfxRenderer& rendererIn, MappedInputManager& mappedInputIn, std::string romPathIn)
    : Activity("GameBoy", rendererIn, mappedInputIn), romPath(std::move(romPathIn)) {}

GameBoyActivity::~GameBoyActivity() { cleanup(); }

void GameBoyActivity::onEnter() {
  Activity::onEnter();
  cleanup();

  exitRequested = false;
  saveWritten = false;
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  activeTouch = TouchControl::None;
  nextFrameUs = 0;
  nextDisplayUs = 0;
  displayDue.store(true, std::memory_order_release);
  error[0] = '\0';
  title[0] = '\0';

  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  setLayout();
  renderer.clearScreen(0xFF);

  if (romPath.empty()) {
    showError("No Game Boy ROM selected");
    return;
  }

  if (!loadRom() || !buildSavePaths() || !initEmulator() || !loadSave()) {
    return;
  }

  // Peanut-GB keeps the RTC registers internally. Seed them from the wall
  // clock on every launch; the emulator advances them during frame execution.
  time_t rawTime = 0;
  time(&rawTime);
  struct tm timeInfo {};
  localtime_r(&rawTime, &timeInfo);
  gb_set_rtc(gb, &timeInfo);

  char gameTitle[17] = {};
  gb_get_rom_name(gb, gameTitle);
  std::strncpy(title, gameTitle, sizeof(title) - 1);
  if (title[0] == '\0') std::strncpy(title, "GAME BOY", sizeof(title) - 1);

  initialized.store(true, std::memory_order_release);
  applyJoypad();
  requestUpdate(true);

  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  nextFrameUs = nowUs;
  nextDisplayUs = nowUs + DISPLAY_PERIOD_US;
  LOG_INF(TAG, "Started %s (%u bytes, save=%u bytes)", title, static_cast<unsigned>(romSize),
          static_cast<unsigned>(cartRamSize));
}

void GameBoyActivity::onExit() {
  if (initialized.load(std::memory_order_acquire)) {
    if (!saveCartRam()) LOG_ERR(TAG, "Save failed during Game Boy exit: %s", savePath.c_str());
  }
  TouchRegistry::getInstance().clear();
  cleanup();
  renderer.setOrientation(GfxRenderer::Portrait);
  Activity::onExit();
}

void GameBoyActivity::loop() {
  handleInput();
  if (exitRequested) {
    mappedInput.suppressCurrentTouchContact();
    finish();
    return;
  }

  if (!initialized.load(std::memory_order_acquire)) {
    // Global shortcuts are intentionally blocked while the Game Boy activity
    // owns input, so provide a physical Back escape from the error screen too.
    using Button = MappedInputManager::Button;
    if (mappedInput.wasPressed(Button::Back)) exitRequested = true;
#if CROSSINK_APP_CAP_TOUCH
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTouchDown(tx, ty)) exitRequested = true;
#endif
    if (exitRequested) {
      mappedInput.suppressCurrentTouchContact();
      finish();
    }
    return;
  }

  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  runEmulation(nowUs);

  if (nowUs >= nextDisplayUs) {
    displayDue.store(true, std::memory_order_release);
    // Do not accumulate an unbounded display backlog when an e-ink refresh or
    // a long emulation stretch takes longer than one target period.
    nextDisplayUs = nowUs + DISPLAY_PERIOD_US;
    requestUpdate();
  }
}

void GameBoyActivity::render(RenderLock&&) {
  if (!displayDue.load(std::memory_order_acquire)) return;

  renderer.clearScreen(0xFF);
  if (!initialized.load(std::memory_order_acquire)) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 32, "GAME BOY", true, EpdFontFamily::BOLD);
    if (error[0]) renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 8, error, true);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 70, "Touch screen to exit", true);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    displayDue.store(false, std::memory_order_release);
    return;
  }

  drawGameFrame();
  const uint8_t claimedFrame = renderFrame.load(std::memory_order_acquire);
  drawControls();
  registerTouchTargets();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  if (claimedFrame <= 1) {
    uint8_t expected = claimedFrame;
    readyFrame.compare_exchange_strong(expected, static_cast<uint8_t>(0xFF),
                                       std::memory_order_acq_rel, std::memory_order_acquire);
    renderFrame.store(0xFF, std::memory_order_release);
  }
  displayDue.store(false, std::memory_order_release);
}

void GameBoyActivity::setLayout() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int availableW = std::max(GAMEBOY_WIDTH, screenW);
  const int availableH = std::max(GAMEBOY_HEIGHT, screenH - 48);
  scale = std::min({MAX_GAME_SCALE, availableW / GAMEBOY_WIDTH, availableH / GAMEBOY_HEIGHT});
  scale = std::max(1, scale);

  gameW = GAMEBOY_WIDTH * scale;
  gameH = GAMEBOY_HEIGHT * scale;
  gameX = (screenW - gameW) / 2;
  gameY = (screenH - gameH) / 2;
  if (gameY < 0) gameY = 0;
}

size_t GameBoyActivity::romBankCountFromCode(const uint8_t code) {
  // Peanut-GB v1.3.0's internal ROM-size table covers codes 0..8.
  if (code > 8) return 0;
  return static_cast<size_t>(2) << code;
}

bool GameBoyActivity::isBatteryBackedCart(const uint8_t type) { return isBatteryBackedCartType(type); }

bool GameBoyActivity::loadRom() {
  HalFile file;
  if (!Storage.openFileForRead(TAG, romPath, file)) {
    showError("Unable to open Game Boy ROM");
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize < ROM_HEADER_SIZE || fileSize > static_cast<uint64_t>(SIZE_MAX)) {
    file.close();
    showError("ROM size is invalid");
    return false;
  }

  uint8_t romSizeCode = 0xFF;
  if (!file.seekSet(0x148) || file.read(&romSizeCode, 1) != 1) {
    file.close();
    showError("ROM header could not be read");
    return false;
  }

  const size_t bankCount = romBankCountFromCode(romSizeCode);
  if (bankCount == 0) {
    file.close();
    showError("Unsupported ROM size");
    return false;
  }

  const size_t expectedSize = bankCount * PEANUT_ROM_BANK_BYTES;
  if (fileSize < expectedSize) {
    file.close();
    showError("ROM file is incomplete");
    return false;
  }

  // Allocate the cartridge size declared by the ROM header, not any trailing
  // padding some dump tools append to the file. This keeps PSRAM usage bounded
  // by the actual Game Boy cartridge image.
  rom = static_cast<uint8_t*>(heap_caps_malloc(expectedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!rom) {
    file.close();
    showError("Not enough PSRAM for ROM");
    return false;
  }

  if (!file.seekSet(0)) {
    file.close();
    showError("ROM seek failed");
    return false;
  }

  size_t offset = 0;
  const size_t totalSize = expectedSize;
  while (offset < totalSize) {
    const size_t chunk = std::min<size_t>(16384, totalSize - offset);
    const int got = file.read(rom + offset, chunk);
    if (got <= 0) {
      file.close();
      showError("ROM read failed");
      return false;
    }
    offset += static_cast<size_t>(got);
  }
  file.close();
  romSize = totalSize;

  if (rom[0x149] > 5) {
    showError("Unsupported cartridge RAM");
    return false;
  }

  return true;
}

bool GameBoyActivity::buildSavePaths() {
  savePath = romPath;
  const size_t slash = savePath.find_last_of('/');
  const size_t dot = savePath.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    savePath += ".sav";
  else
    savePath.replace(dot, std::string::npos, ".sav");

  tempSavePath = savePath + ".tmp";
  backupSavePath = savePath + ".bak";
  return savePath.size() < 512 && tempSavePath.size() < 512 && backupSavePath.size() < 512;
}

bool GameBoyActivity::initEmulator() {
  gb = static_cast<gb_s*>(calloc(1, sizeof(gb_s)));
  if (!gb) {
    showError("Not enough memory for emulator");
    return false;
  }

  const enum gb_init_error_e initResult = gb_init(gb, &GameBoyActivity::romRead, &GameBoyActivity::cartRamRead,
                                                   &GameBoyActivity::cartRamWrite, &GameBoyActivity::fatalError, this);
  if (initResult != GB_INIT_NO_ERROR) {
    showError(initErrorString(initResult));
    return false;
  }

  size_t saveSize = 0;
  if (gb_get_save_size_s(gb, &saveSize) < 0) {
    showError("Unknown cartridge RAM size");
    return false;
  }

  if (saveSize > 0) {
    cartRam = static_cast<uint8_t*>(heap_caps_malloc(saveSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!cartRam) {
      showError("Not enough PSRAM for save RAM");
      return false;
    }
    std::memset(cartRam, 0, saveSize);
    cartRamSize = saveSize;
  }

  frameBuffers[0] = static_cast<uint8_t*>(heap_caps_malloc(EMULATOR_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frameBuffers[1] = static_cast<uint8_t*>(heap_caps_malloc(EMULATOR_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frameBuffers[0] || !frameBuffers[1]) {
    showError("Not enough PSRAM for Game Boy video");
    return false;
  }
  std::memset(frameBuffers[0], 3, EMULATOR_FRAME_BYTES);
  std::memset(frameBuffers[1], 3, EMULATOR_FRAME_BYTES);
  writeFrame = 0;
  readyFrame.store(0xFF, std::memory_order_release);
  renderFrame.store(0xFF, std::memory_order_release);

  gb_init_lcd(gb, &GameBoyActivity::lcdDrawLine);
  return true;
}

bool GameBoyActivity::loadSave() {
  if (cartRamSize == 0 || !isBatteryBackedCart(rom[0x147])) return true;

  std::memset(cartRam, 0, cartRamSize);
  HalFile file;
  if (!Storage.openFileForRead(TAG, savePath, file)) return true;  // New save; normal case.

  size_t offset = 0;
  while (offset < cartRamSize) {
    const size_t want = cartRamSize - offset;
    const int got = file.read(cartRam + offset, want);
    if (got <= 0) break;
    offset += static_cast<size_t>(got);
  }
  file.close();
  LOG_INF(TAG, "Loaded save: %u/%u bytes", static_cast<unsigned>(offset), static_cast<unsigned>(cartRamSize));
  return true;
}

bool GameBoyActivity::saveCartRam() {
  if (saveWritten || cartRamSize == 0 || !cartRam || !rom || !isBatteryBackedCart(rom[0x147])) return true;

  HalFile file;
  if (!Storage.openFileForWrite(TAG, tempSavePath, file)) {
    LOG_ERR(TAG, "Unable to open save temp file: %s", tempSavePath.c_str());
    return false;
  }

  const size_t written = file.write(cartRam, cartRamSize);
  const bool synced = written == cartRamSize && file.sync();
  file.close();
  if (!synced) {
    Storage.remove(tempSavePath.c_str());
    LOG_ERR(TAG, "Save write failed: wrote=%u expected=%u", static_cast<unsigned>(written),
            static_cast<unsigned>(cartRamSize));
    return false;
  }

  const bool hadExisting = Storage.exists(savePath.c_str());
  if (hadExisting) {
    Storage.remove(backupSavePath.c_str());
    if (!Storage.rename(savePath.c_str(), backupSavePath.c_str())) {
      Storage.remove(tempSavePath.c_str());
      LOG_ERR(TAG, "Could not stage previous save before commit");
      return false;
    }
  }

  if (!Storage.rename(tempSavePath.c_str(), savePath.c_str())) {
    if (hadExisting) Storage.rename(backupSavePath.c_str(), savePath.c_str());
    Storage.remove(tempSavePath.c_str());
    LOG_ERR(TAG, "Could not commit save: %s", savePath.c_str());
    return false;
  }

  if (hadExisting) Storage.remove(backupSavePath.c_str());
  saveWritten = true;
  LOG_INF(TAG, "Saved: %s", savePath.c_str());
  return true;
}

void GameBoyActivity::cleanup() {
  initialized.store(false, std::memory_order_release);
  if (gb) {
    free(gb);
    gb = nullptr;
  }
  if (rom) {
    heap_caps_free(rom);
    rom = nullptr;
  }
  if (cartRam) {
    heap_caps_free(cartRam);
    cartRam = nullptr;
  }
  for (auto*& buffer : frameBuffers) {
    if (buffer) heap_caps_free(buffer);
    buffer = nullptr;
  }
  writeFrame = 0;
  readyFrame.store(0xFF, std::memory_order_release);
  renderFrame.store(0xFF, std::memory_order_release);
  romSize = 0;
  cartRamSize = 0;
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  activeTouch = TouchControl::None;
  saveWritten = false;
}

void GameBoyActivity::showError(const char* message) {
  std::strncpy(error, message ? message : "Game Boy error", sizeof(error) - 1);
  error[sizeof(error) - 1] = '\0';
  initialized.store(false, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

void GameBoyActivity::handleInput() {
  using Button = MappedInputManager::Button;
  const Button buttons[] = {Button::Up, Button::Down, Button::Left, Button::Right, Button::Confirm, Button::Back,
                            Button::Power};
  uint8_t nextPhysical = 0;
  for (const Button button : buttons) {
    if (mappedInput.isPressed(button)) nextPhysical |= joypadBitForButton(button);
  }
  if (nextPhysical != physicalJoypadMask) {
    physicalJoypadMask = nextPhysical;
    applyJoypad();
  }

#if CROSSINK_APP_CAP_TOUCH
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    updateTouch(x, y);
  }

  if (mappedInput.isScreenTouchHeld(x, y)) {
    updateTouch(x, y);
  } else if (mappedInput.wasScreenTouchReleased()) {
    activeTouch = TouchControl::None;
    touchJoypadMask = 0;
    applyJoypad();
  }
#endif
}

void GameBoyActivity::updateTouch(const int x, const int y) {
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

GameBoyActivity::TouchControl GameBoyActivity::hitTestTouch(const int x, const int y) const {
  // Left side D-pad occupies the panel to the left of the game image.
  const int leftW = std::max(0, gameX);
  const int panelY = gameY + 120;
  const int centerX = leftW / 2;
  if (leftW >= 110) {
    if (hitRect(x, y, centerX - 35, panelY, 70, 58)) return TouchControl::Up;
    if (hitRect(x, y, centerX - 35, panelY + 140, 70, 58)) return TouchControl::Down;
    if (hitRect(x, y, 10, panelY + 58, 60, 82)) return TouchControl::Left;
    if (hitRect(x, y, leftW - 70, panelY + 58, 60, 82)) return TouchControl::Right;
  }

  const int rightX = gameX + gameW;
  const int rightW = std::max(0, renderer.getScreenWidth() - rightX);
  if (rightW >= TOUCH_EXIT_MIN_WIDTH) {
    const int buttonW = std::min(70, std::max(48, (rightW - TOUCH_TARGET_GAP) / 2));
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_TARGET_GAP;
    if (hitRect(x, y, firstX, panelY, buttonW, 70)) return TouchControl::B;
    if (hitRect(x, y, secondX, panelY, buttonW, 70)) return TouchControl::A;
    if (hitRect(x, y, firstX, gameY + 275, buttonW, 52)) return TouchControl::Select;
    if (hitRect(x, y, secondX, gameY + 275, buttonW, 52)) return TouchControl::Start;
    if (hitRect(x, y, rightX + 4, gameY + gameH - 52, rightW - 8, 48)) return TouchControl::Exit;
  }
  return TouchControl::None;
}

uint8_t GameBoyActivity::joypadBitForTouch(const TouchControl control) {
  switch (control) {
    case TouchControl::Up: return JOYPAD_UP;
    case TouchControl::Down: return JOYPAD_DOWN;
    case TouchControl::Left: return JOYPAD_LEFT;
    case TouchControl::Right: return JOYPAD_RIGHT;
    case TouchControl::A: return JOYPAD_A;
    case TouchControl::B: return JOYPAD_B;
    case TouchControl::Select: return JOYPAD_SELECT;
    case TouchControl::Start: return JOYPAD_START;
    default: return 0;
  }
}

uint8_t GameBoyActivity::joypadBitForButton(const MappedInputManager::Button button) {
  switch (button) {
    case MappedInputManager::Button::Up: return JOYPAD_UP;
    case MappedInputManager::Button::Down: return JOYPAD_DOWN;
    case MappedInputManager::Button::Left: return JOYPAD_LEFT;
    case MappedInputManager::Button::Right: return JOYPAD_RIGHT;
    case MappedInputManager::Button::Confirm: return JOYPAD_START;
    case MappedInputManager::Button::Back: return JOYPAD_SELECT;
    case MappedInputManager::Button::Power: return JOYPAD_A;
    default: return 0;
  }
}

void GameBoyActivity::applyJoypad() {
  if (gb) gb->direct.joypad = static_cast<uint8_t>(~(physicalJoypadMask | touchJoypadMask));
}

void GameBoyActivity::runEmulation(const uint64_t nowUs) {
  if (!gb || nextFrameUs == 0) nextFrameUs = nowUs;

  uint8_t ran = 0;
  while (nowUs >= nextFrameUs && ran < MAX_CATCH_UP_FRAMES) {
    // Do not start a frame in either buffer while the display task owns it,
    // and do not overwrite the most recently completed frame before it has
    // been consumed. With two buffers this gives a simple, lock-free handoff.
    const uint8_t pending = readyFrame.load(std::memory_order_acquire);
    const uint8_t rendering = renderFrame.load(std::memory_order_acquire);
    if (writeFrame == pending || writeFrame == rendering) break;

    gb_run_frame(gb);
    publishCompletedFrame();
    nextFrameUs += EMULATION_FRAME_PERIOD_US;
    ++ran;
  }

  if (ran == MAX_CATCH_UP_FRAMES && nowUs >= nextFrameUs) nextFrameUs = nowUs + EMULATION_FRAME_PERIOD_US;
}

void GameBoyActivity::publishCompletedFrame() {
  readyFrame.store(writeFrame, std::memory_order_release);
  writeFrame ^= 1u;
}

void GameBoyActivity::drawGameFrame() {
  const uint8_t frameIndex = readyFrame.load(std::memory_order_acquire);
  if (frameIndex > 1) return;

  const uint8_t* source = frameBuffers[frameIndex];
  uint8_t* framebuffer = renderer.getFrameBuffer();
  if (!source || !framebuffer) return;

  // Claim the exact source buffer before reading it. The emulation task checks
  // this ownership marker before starting its next frame. Keep the claim until
  // the display refresh has consumed the CrossInk framebuffer.
  renderFrame.store(frameIndex, std::memory_order_release);

  const int panelBytes = renderer.getDisplayWidthBytes();
  const int screenW = renderer.getDisplayWidth();
  if (panelBytes <= 0 || screenW <= 0) {
    renderFrame.store(0xFF, std::memory_order_release);
    return;
  }

  // LandscapeCounterClockwise is the native X4 Pro framebuffer orientation,
  // so logical (x,y) maps directly to physical (x,y). Convert Peanut-GB's
  // 4-shade line buffer to CrossInk BW: shades 0/1 are black, 2/3 are white.
  const int startByte = gameX / 8;
  if (gameX < 0 || startByte < 0 || gameX + gameW > screenW) {
    renderFrame.store(0xFF, std::memory_order_release);
    return;
  }

  for (int gy = 0; gy < GAMEBOY_HEIGHT; ++gy) {
    for (int sy = 0; sy < scale; ++sy) {
      uint8_t* out = framebuffer + static_cast<size_t>(gameY + gy * scale + sy) * panelBytes + startByte;
      const int outByteCount = gameW / 8;
      std::memset(out, 0xFF, static_cast<size_t>(outByteCount));

      for (int gx = 0; gx < GAMEBOY_WIDTH; ++gx) {
        const uint8_t shade = source[gy * GAMEBOY_WIDTH + gx] & 0x03;
        if (shade >= 2) continue;
        const int baseX = gx * scale;
        for (int sx = 0; sx < scale; ++sx) {
          const int dstX = baseX + sx;
          out[dstX >> 3] &= static_cast<uint8_t>(~(1u << (7 - (dstX & 7))));
        }
      }
    }
  }
}

void GameBoyActivity::drawControls() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int leftW = std::max(0, gameX);
  const int rightX = gameX + gameW;
  const int rightW = std::max(0, screenW - rightX);
  const uint8_t currentMask = static_cast<uint8_t>(physicalJoypadMask | touchJoypadMask);

  renderer.drawRect(gameX - 1, gameY - 1, gameW + 2, gameH + 2, true);
  if (leftW >= 110) {
    renderer.drawText(UI_10_FONT_ID, 6, 18, title[0] ? title : "GAME BOY", true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, 8, screenH - 42, "HOLD = KEEP PRESSED", true);

    auto dpadButton = [&](int x, int y, int w, int h, const uint8_t bit, const char* label) {
      const bool active = (currentMask & bit) != 0;
      renderer.fillRect(x + 1, y + 1, w - 2, h - 2, active);
      renderer.drawRect(x, y, w, h, true);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
      renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, y + h / 2 - 7, label, !active, EpdFontFamily::BOLD);
    };

    const int panelY = gameY + 120;
    const int centerX = leftW / 2;
    dpadButton(centerX - 35, panelY, 70, 58, JOYPAD_UP, "U");
    dpadButton(centerX - 35, panelY + 140, 70, 58, JOYPAD_DOWN, "D");
    dpadButton(10, panelY + 58, 60, 82, JOYPAD_LEFT, "L");
    dpadButton(leftW - 70, panelY + 58, 60, 82, JOYPAD_RIGHT, "R");
  }

  if (rightW >= TOUCH_EXIT_MIN_WIDTH) {
    const int buttonW = std::min(70, std::max(48, (rightW - TOUCH_TARGET_GAP) / 2));
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_TARGET_GAP;
    const int panelY = gameY + 120;

    auto actionButton = [&](int x, int y, int w, int h, const uint8_t bit, const char* label) {
      const bool active = bit != 0 && (currentMask & bit) != 0;
      renderer.fillRect(x + 1, y + 1, w - 2, h - 2, active);
      renderer.drawRect(x, y, w, h, true);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, label);
      renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, y + h / 2 - 7, label, !active, EpdFontFamily::BOLD);
    };

    actionButton(firstX, panelY, buttonW, 70, JOYPAD_B, "B");
    actionButton(secondX, panelY, buttonW, 70, JOYPAD_A, "A");
    actionButton(firstX, gameY + 275, buttonW, 52, JOYPAD_SELECT, "SEL");
    actionButton(secondX, gameY + 275, buttonW, 52, JOYPAD_START, "START");

    const bool exitActive = activeTouch == TouchControl::Exit;
    renderer.fillRect(rightX + 5, gameY + gameH - 51, rightW - 10, 46, exitActive);
    renderer.drawRect(rightX + 4, gameY + gameH - 52, rightW - 8, 48, true);
    renderer.drawText(UI_10_FONT_ID, rightX + rightW / 2 - renderer.getTextWidth(UI_10_FONT_ID, "EXIT") / 2,
                      gameY + gameH - 38, "EXIT", !exitActive, EpdFontFamily::BOLD);
  }
}

void GameBoyActivity::registerTouchTargets() {
#if CROSSINK_APP_CAP_TOUCH
  auto& registry = TouchRegistry::getInstance();
  const int leftW = std::max(0, gameX);
  const int panelY = gameY + 120;
  if (leftW >= 110) {
    const int centerX = leftW / 2;
    registry.add(Rect(centerX - 35, panelY, 70, 58), static_cast<int>(TouchControl::Up), TouchRegistry::Kind::Button);
    registry.add(Rect(centerX - 35, panelY + 140, 70, 58), static_cast<int>(TouchControl::Down), TouchRegistry::Kind::Button);
    registry.add(Rect(10, panelY + 58, 60, 82), static_cast<int>(TouchControl::Left), TouchRegistry::Kind::Button);
    registry.add(Rect(leftW - 70, panelY + 58, 60, 82), static_cast<int>(TouchControl::Right), TouchRegistry::Kind::Button);
  }

  const int rightX = gameX + gameW;
  const int rightW = std::max(0, renderer.getScreenWidth() - rightX);
  if (rightW >= TOUCH_EXIT_MIN_WIDTH) {
    const int buttonW = std::min(70, std::max(48, (rightW - TOUCH_TARGET_GAP) / 2));
    const int firstX = rightX + 4;
    const int secondX = firstX + buttonW + TOUCH_TARGET_GAP;
    registry.add(Rect(firstX, panelY, buttonW, 70), static_cast<int>(TouchControl::B), TouchRegistry::Kind::Button);
    registry.add(Rect(secondX, panelY, buttonW, 70), static_cast<int>(TouchControl::A), TouchRegistry::Kind::Button);
    registry.add(Rect(firstX, gameY + 275, buttonW, 52), static_cast<int>(TouchControl::Select), TouchRegistry::Kind::Button);
    registry.add(Rect(secondX, gameY + 275, buttonW, 52), static_cast<int>(TouchControl::Start), TouchRegistry::Kind::Button);
    registry.add(Rect(rightX + 4, gameY + gameH - 52, rightW - 8, 48), static_cast<int>(TouchControl::Exit),
                  TouchRegistry::Kind::Button);
  }
#endif
}

uint8_t GameBoyActivity::romRead(gb_s* state, const uint_fast32_t addr) {
  auto* self = state ? static_cast<GameBoyActivity*>(state->direct.priv) : nullptr;
  if (!self || !self->rom || addr >= self->romSize) return 0xFF;
  return self->rom[addr];
}

uint8_t GameBoyActivity::cartRamRead(gb_s* state, const uint_fast32_t addr) {
  auto* self = state ? static_cast<GameBoyActivity*>(state->direct.priv) : nullptr;
  if (!self || !self->cartRam || addr >= self->cartRamSize) return 0xFF;
  return self->cartRam[addr];
}

void GameBoyActivity::cartRamWrite(gb_s* state, const uint_fast32_t addr, const uint8_t value) {
  auto* self = state ? static_cast<GameBoyActivity*>(state->direct.priv) : nullptr;
  if (!self || !self->cartRam || addr >= self->cartRamSize) return;
  self->cartRam[addr] = value;
}

void GameBoyActivity::lcdDrawLine(gb_s* state, const uint8_t* pixels, const uint_fast8_t line) {
  auto* self = state ? static_cast<GameBoyActivity*>(state->direct.priv) : nullptr;
  if (self) self->writeGameBoyLine(pixels, line);
}

void GameBoyActivity::writeGameBoyLine(const uint8_t* pixels, const uint_fast8_t line) {
  if (!pixels || line >= GAMEBOY_HEIGHT || !frameBuffers[writeFrame]) return;
  std::memcpy(frameBuffers[writeFrame] + static_cast<size_t>(line) * GAMEBOY_WIDTH, pixels, GAMEBOY_WIDTH);
}

void GameBoyActivity::fatalError(gb_s* state, enum gb_error_e errorCode, const uint16_t addr) {
  auto* self = state ? static_cast<GameBoyActivity*>(state->direct.priv) : nullptr;
  if (!self) {
    ESP.restart();
    while (true) delay(1000);
  }

  LOG_ERR(TAG, "Peanut-GB fatal error=%d addr=0x%04X", static_cast<int>(errorCode), addr);
  if (!self->saveCartRam()) LOG_ERR(TAG, "Emergency save failed before restart: %s", self->savePath.c_str());

  ESP.restart();
  while (true) delay(1000);
}

const char* GameBoyActivity::initErrorString(const int errorCode) {
  switch (static_cast<gb_init_error_e>(errorCode)) {
    case GB_INIT_CARTRIDGE_UNSUPPORTED: return "Unsupported cartridge";
    case GB_INIT_INVALID_CHECKSUM: return "Invalid ROM checksum";
    case GB_INIT_NO_ERROR: return "";
    default: return "Unknown Game Boy ROM error";
  }
}

#endif  // CROSSINK_GAMEBOY
