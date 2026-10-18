#include "GameBoyActivity.h"

#if CROSSINK_GAMEBOY

#include <AppVersion.h>
#include <Arduino.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#ifndef SIMULATOR
#include <BoardConfig.h>
#include <esp_system.h>
#include <esp_timer.h>
#endif

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

uint64_t nowMicros() {
#ifdef SIMULATOR
  return static_cast<uint64_t>(micros());
#else
  return static_cast<uint64_t>(esp_timer_get_time());
#endif
}

// The screen refresh a game uses (GameBoyActivity::refreshPreset). The panel's
// built-in Fast waveform takes ~680 ms on the X4 Pro's UC8279, about 1.3
// pictures a second. Measured on a UC8279 (variant 0x68) unit across the
// 1.8.1 betas, "fast" runs a host-loaded DU waveform (the stock X3 UC8279
// bank) at 40% of its frames, the PLL frame rate at 0x0F and the display SPI
// at 20 MHz (the controller's rating; 10 MHz otherwise): a refresh takes
// ~276 ms, about 3 pictures a second, with the darkness of stock and a little
// ghosting that the periodic cleaning refresh clears. Drive levels stay the
// bank's and voltages the panel's. Other controllers and variants keep stock.
// It also sends and drives only the rectangle that changed since the last
// picture, and skips a picture that did not change at all.
struct RefreshPreset {
  const char* label;
  uint8_t tsset;
  uint8_t pll;
  uint8_t duPct;
  uint32_t spiHz;   // display SPI clock, 0 = the driver's (10 MHz on the X4 Pro)
  bool diffWindow;  // only the changed rectangle (HalDisplay::setFastRefreshDiffWindow)
};
constexpr RefreshPreset kRefreshPresets[] = {
    {"stock", 0, 0, 0, 0, false},
    {"fast-window", 0, 0x0F, 40, 20000000, true},
};

// Whether this reader's screen is one the fast refresh was validated on.
bool fastRefreshValidated() {
#ifdef SIMULATOR
  return false;
#else
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279 &&
         BoardConfig::ACTIVE.displayControllerVariant == 0x68;
#endif
}

// The e-ink controller the reader detected at boot (X4 Pro batches differ:
// SSD1677, UC8179 or UC8279) and its variant byte, for the speed log:
// how fast a refresh can be depends on it.
void panelName(char* out, const size_t size) {
#ifdef SIMULATOR
  snprintf(out, size, "simulator");
#else
  const char* name = "other";
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677: name = "SSD1677"; break;
    case BoardConfig::DisplayController::UC8179: name = "UC8179"; break;
    case BoardConfig::DisplayController::UC8279: name = "UC8279"; break;
    case BoardConfig::DisplayController::UC8253: name = "UC8253"; break;
    default: break;
  }
  snprintf(out, size, "%s-%02X", name, BoardConfig::ACTIVE.displayControllerVariant);
#endif
}

// Whether panel pixel (x, y) is black for Game Boy shade 0 (white, the
// lightest) to 3 (black). The panel here is black and white only, so the two
// greys are dithered: one dot in four for light grey, three in four for dark.
bool shadeIsBlack(const uint8_t shade, const int x, const int y) {
  switch (shade) {
    case 3: return true;
    case 2: return ((x ^ y) & 1) == 0 || (x & 1) == 0;
    case 1: return (x & 1) == 0 && (y & 1) == 0;
    default: return false;
  }
}

}  // namespace

GameBoyActivity::GameBoyActivity(GfxRenderer& rendererIn, MappedInputManager& mappedInputIn, std::string romPathIn)
    : Activity("GameBoy", rendererIn, mappedInputIn), romPath(std::move(romPathIn)) {}

GameBoyActivity::~GameBoyActivity() { cleanup(); }

void GameBoyActivity::onEnter() {
  Activity::onEnter();
  cleanup();

  exitRequested = false;
  saveDirty = false;
  physicalJoypadMask = 0;
  touchJoypadMask = 0;
  activeTouch = TouchControl::None;
  nextFrameUs = 0;
  nextDisplayUs = 0;
  displayDue.store(true, std::memory_order_release);
  error[0] = '\0';
  title[0] = '\0';

  portrait = true;
  rotateRequested = false;
  renderer.setOrientation(GfxRenderer::Portrait);
  setLayout();
  renderer.clearScreen(0xFF);
  chromeKey = CHROME_NONE;

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
#ifndef SIMULATOR
  {
    uint32_t phases[8];
    if (display.lastRefreshTimings(phases)) {
      perfSeenRefreshes = phases[6];
      perfSeenSkipped = phases[7];
    }
  }
#endif
  requestUpdate(true);

  const uint64_t nowUs = nowMicros();
  nextFrameUs = nowUs;
  nextDisplayUs = nowUs + DISPLAY_PERIOD_US;
  nextAutosaveUs = nowUs + AUTOSAVE_PERIOD_US;
  perfWindowStartUs = perfSessionStartUs = nowUs;
  if (fastRefreshValidated()) {
    refreshPreset = 1;
    applyRefreshPreset();
  }
  startEmuTask();
  LOG_INF(TAG, "Started %s (%u bytes, save=%u bytes)", title, static_cast<unsigned>(romSize),
          static_cast<unsigned>(cartRamSize));
}

void GameBoyActivity::onExit() {
  stopEmuTask();  // before the save and cleanup touch its state
  if (initialized.load(std::memory_order_acquire)) {
    if (!saveCartRam()) LOG_ERR(TAG, "Save failed during Game Boy exit: %s", savePath.c_str());
    writePerfLog();
  }
  TouchRegistry::getInstance().clear();
  cleanup();
  if (refreshPreset != 0) {
    refreshPreset = 0;
#ifndef SIMULATOR
    display.finishOverlapped();  // the last picture's refresh may still run
    display.setFastRefreshTuning(0, 0);  // the reader's own refreshes again
    display.setFastRefreshDuScale(0);
    display.setFastRefreshDiffWindow(false);
    display.setDisplaySpiHz(0);
#endif
  }
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

  if (rotateRequested) {
    rotateRequested = false;
    rotate();
  }
  const uint64_t nowUs = nowMicros();
  if (nowUs >= nextDisplayUs) {
    displayDue.store(true, std::memory_order_release);
    // Do not accumulate an unbounded display backlog when an e-ink refresh or
    // a long emulation stretch takes longer than one target period.
    nextDisplayUs = nowUs + (refreshPreset != 0 ? FAST_DISPLAY_PERIOD_US : DISPLAY_PERIOD_US);
    requestUpdate();
  }

  // The emulator has its own task; leave CPU 1 to the render task between
  // input polls (this activity skips the main loop's own delay).
  delay(2);
}

void GameBoyActivity::startEmuTask() {
  emuRun.store(true, std::memory_order_release);
  emuStopped.store(false, std::memory_order_release);
  if (xTaskCreatePinnedToCore(&GameBoyActivity::emuTaskEntry, "GameBoyEmu", 12288, this, 1, &emuTask, 0) != pdPASS) {
    LOG_ERR(TAG, "Could not start the emulator task");
    emuTask = nullptr;
    emuRun.store(false, std::memory_order_release);
    emuStopped.store(true, std::memory_order_release);
    showError("Could not start the emulator");
  }
}

// Asks the emulator task to finish its frame and waits for it (bounded).
void GameBoyActivity::stopEmuTask() {
  if (!emuTask) return;
  emuRun.store(false, std::memory_order_release);
  const unsigned long start = millis();
  while (!emuStopped.load(std::memory_order_acquire) && millis() - start < 1000) delay(1);
  if (!emuStopped.load(std::memory_order_acquire)) LOG_ERR(TAG, "Emulator task did not stop in time");
#ifdef SIMULATOR
  vTaskDelete(emuTask);  // the host thread has returned; release its handle
#endif
  emuTask = nullptr;
}

void GameBoyActivity::emuTaskEntry(void* arg) {
  auto* self = static_cast<GameBoyActivity*>(arg);
  self->emuTaskLoop();
  self->emuStopped.store(true, std::memory_order_release);
#ifndef SIMULATOR
  vTaskDelete(nullptr);
#endif
}

void GameBoyActivity::emuTaskLoop() {
  while (emuRun.load(std::memory_order_acquire)) {
    const uint64_t nowUs = nowMicros();
    if (perfLastLoopUs) perfLoopMaxGapUs = std::max(perfLoopMaxGapUs, static_cast<uint32_t>(nowUs - perfLastLoopUs));
    perfLastLoopUs = nowUs;
    ++perfLoops;
    runEmulation(nowUs);
    updatePerf(nowUs);

    if (refreshPreset != 0 && nowUs >= nextCleanUs) {
      nextCleanUs = nowUs + CLEAN_PERIOD_US;
      fullRefreshDue.store(true, std::memory_order_release);
    }

    if (nowUs >= nextAutosaveUs) {
      nextAutosaveUs = nowUs + AUTOSAVE_PERIOD_US;
      if (saveDirty && !saveCartRam()) LOG_ERR(TAG, "Autosave failed: %s", savePath.c_str());
    }

    // Let CPU 0's idle task (and its watchdog) run between frames.
    delay(1);
  }
}

void GameBoyActivity::render(RenderLock&&) {
  // Taken at the start: a request made while this refresh runs then starts the
  // next one right away. (Cleared at the end, it was lost, and the screen
  // waited up to a display period for the next request.)
  if (!displayDue.exchange(false, std::memory_order_acq_rel)) return;

  if (!initialized.load(std::memory_order_acquire)) {
    chromeKey = CHROME_NONE;
    renderer.clearScreen(0xFF);
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 32, "GAME BOY", true, EpdFontFamily::BOLD);
    if (error[0]) renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 8, error, true);
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2 + 70, "Touch screen to exit", true);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const uint64_t drawStartUs = nowMicros();
  const bool cleaning = fullRefreshDue.exchange(false, std::memory_order_acq_rel);
  // The framebuffer keeps the title and buttons from the last picture (the
  // display is single-buffered): clear and redraw them only when they change,
  // and with every cleaning refresh in case anything else drew on the screen.
  const uint32_t chrome = static_cast<uint32_t>(physicalJoypadMask | touchJoypadMask) |
                          (static_cast<uint32_t>(activeTouch) << 8) | (portrait ? 0x10000u : 0u);
  const bool redrawChrome = cleaning || chrome != chromeKey;
  if (redrawChrome) renderer.clearScreen(0xFF);
  drawGameFrame();
  // The picture is in the display's framebuffer now: hand the Game Boy frame
  // back before the e-ink refresh (hundreds of ms), or the emulator, with
  // both of its buffers held, would stand still for the whole refresh.
  releaseRenderedFrame();
  if (redrawChrome) {
    drawControls();
    chromeKey = chrome;
  }
  registerTouchTargets();
  const uint64_t refreshStartUs = nowMicros();
  // On the fast refresh the call returns while the waveform runs, and the next
  // picture is drawn meanwhile: the driver keeps its own copy of the frame for
  // the rest of the refresh (setFastRefreshDiffWindow). The call first
  // finishes the refresh before. The stock refresh blocks until it is done.
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
  const uint64_t doneUs = nowMicros();
#ifndef SIMULATOR
  uint32_t phases[8];
  if (display.lastRefreshTimings(phases)) {
    const uint32_t completed = phases[6] - perfSeenRefreshes;
    const uint32_t skipped = phases[7] - perfSeenSkipped;
    perfSeenRefreshes = phases[6];
    perfSeenSkipped = phases[7];
    if (!cleaning) {
      // Pictures that had not changed, which the driver did not refresh.
      if (skipped != 0) perfUnchanged.fetch_add(skipped, std::memory_order_relaxed);
      // The figures below are those of the refresh this call completed.
      if (completed == 0) return;
      for (int i = 0; i < 6; ++i) perfPhaseUs[i].fetch_add(phases[i], std::memory_order_relaxed);
    }
  }
#endif
  if (cleaning) {
    // A cleaning refresh takes ~1.6 s: kept out of the Fast refresh figures.
    perfCleanRefreshes.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  perfDisplays.fetch_add(1, std::memory_order_relaxed);
  perfDrawUs.fetch_add(static_cast<uint32_t>(refreshStartUs - drawStartUs), std::memory_order_relaxed);
  const uint32_t refreshUs = static_cast<uint32_t>(doneUs - refreshStartUs);
  perfRefreshUs.fetch_add(refreshUs, std::memory_order_relaxed);
  uint32_t seenMax = perfRefreshMaxUs.load(std::memory_order_relaxed);
  while (refreshUs > seenMax && !perfRefreshMaxUs.compare_exchange_weak(seenMax, refreshUs)) {
  }
}

// Marks the frame drawGameFrame() copied as consumed and lets the emulator
// write into it again.
void GameBoyActivity::releaseRenderedFrame() {
  const uint8_t claimedFrame = renderFrame.load(std::memory_order_acquire);
  if (claimedFrame > 1) return;
  uint8_t expected = claimedFrame;
  readyFrame.compare_exchange_strong(expected, static_cast<uint8_t>(0xFF), std::memory_order_acq_rel,
                                     std::memory_order_acquire);
  renderFrame.store(0xFF, std::memory_order_release);
}

// Every PERF_WINDOW_US: logs the counters and adds them to the session totals.
void GameBoyActivity::updatePerf(const uint64_t nowUs) {
  const uint64_t elapsedUs = nowUs - perfWindowStartUs;
  if (elapsedUs < PERF_WINDOW_US) return;
  const uint32_t displays = perfDisplays.exchange(0, std::memory_order_relaxed);
  const uint32_t drawUs = perfDrawUs.exchange(0, std::memory_order_relaxed);
  const uint32_t refreshUs = perfRefreshUs.exchange(0, std::memory_order_relaxed);
  const uint32_t refreshMaxUs = perfRefreshMaxUs.exchange(0, std::memory_order_relaxed);
  const float seconds = static_cast<float>(elapsedUs) / 1e6f;
  const float fps = static_cast<float>(perfFrames) / seconds;
  const unsigned speed = static_cast<unsigned>(fps * 100.0f / 59.73f + 0.5f);
  const float emuMs = perfFrames ? static_cast<float>(perfEmuUs) / perfFrames / 1000.0f : 0.0f;
  const float refreshMs = displays ? static_cast<float>(refreshUs) / displays / 1000.0f : 0.0f;

  uint32_t phaseUs[6];
  for (int i = 0; i < 6; ++i) phaseUs[i] = perfPhaseUs[i].exchange(0, std::memory_order_relaxed);
  const uint32_t cleanRefreshes = perfCleanRefreshes.exchange(0, std::memory_order_relaxed);
  const uint32_t unchanged = perfUnchanged.exchange(0, std::memory_order_relaxed);
  const float perDisplayMs = displays ? 1.0f / displays / 1000.0f : 0.0f;

  LOG_INF(TAG, "perf: emu %.1f fps (%u%%), frame %.1f ms avg %.1f max, waits %u; lcd %.1f/s, draw %.1f ms, "
          "refresh %.0f ms avg %.0f max; loop %.0f/s, longest gap %u ms",
          fps, speed, emuMs, perfEmuMaxUs / 1000.0f, static_cast<unsigned>(perfWaits), displays / seconds,
          displays ? drawUs / 1000.0f / displays : 0.0f, refreshMs, refreshMaxUs / 1000.0f, perfLoops / seconds,
          static_cast<unsigned>(perfLoopMaxGapUs / 1000));
  perfTotalLoopMaxGapUs = std::max(perfTotalLoopMaxGapUs, perfLoopMaxGapUs);
  perfTotalLoops += perfLoops;
  perfLoops = 0;
  perfLoopMaxGapUs = 0;

  perfTotalFrames += perfFrames;
  perfTotalWaits += perfWaits;
  perfTotalEmuUs += perfEmuUs;
  perfTotalEmuMaxUs = std::max(perfTotalEmuMaxUs, perfEmuMaxUs);
  perfTotalDisplays += displays;
  perfTotalDrawUs += drawUs;
  perfTotalRefreshUs += refreshUs;
  perfTotalRefreshMaxUs = std::max(perfTotalRefreshMaxUs, refreshMaxUs);
  perfTotalCleanRefreshes += cleanRefreshes;
  perfTotalUnchanged += unchanged;
  for (int i = 0; i < 6; ++i) perfTotalPhaseUs[i] += phaseUs[i];
  perfFrames = perfWaits = perfEmuUs = perfEmuMaxUs = 0;
  perfWindowStartUs = nowUs;
}

// Starts a new stretch of figures for gameboy-perf.txt (a new preset).
void GameBoyActivity::resetPerfTotals(const uint64_t nowUs) {
  perfSessionStartUs = perfWindowStartUs = nowUs;
  perfFrames = perfWaits = perfEmuUs = perfEmuMaxUs = 0;
  perfLoops = perfLoopMaxGapUs = 0;
  perfDisplays.store(0);
  perfDrawUs.store(0);
  perfRefreshUs.store(0);
  perfRefreshMaxUs.store(0);
  perfTotalFrames = perfTotalWaits = perfTotalEmuUs = 0;
  perfTotalEmuMaxUs = 0;
  perfTotalDisplays = perfTotalDrawUs = perfTotalRefreshUs = 0;
  perfTotalRefreshMaxUs = 0;
  perfTotalLoops = 0;
  perfTotalLoopMaxGapUs = 0;
  perfCleanRefreshes.store(0);
  perfTotalCleanRefreshes = 0;
  perfUnchanged.store(0);
  perfTotalUnchanged = 0;
  for (int i = 0; i < 6; ++i) {
    perfPhaseUs[i].store(0);
    perfTotalPhaseUs[i] = 0;
  }
}

// One line per game session (and per refresh preset tried in it), for
// comparing readers, builds and presets.
void GameBoyActivity::writePerfLog() {
  const float seconds = static_cast<float>(perfWindowStartUs - perfSessionStartUs) / 1e6f;
  if (seconds < 4.0f || perfTotalFrames == 0) return;
  const float fps = perfTotalFrames / seconds;
  char panel[24];
  panelName(panel, sizeof(panel));
  const float phaseDiv = perfTotalDisplays ? 1000.0f * perfTotalDisplays : 1.0f;
  char line[340];
  const int len = snprintf(
      line, sizeof(line), "%s,%s,%.0f,%.1f,%.0f,%.2f,%.1f,%llu,%.2f,%.1f,%.0f,%.0f,%.0f,%u,%s,%s,%s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%.1f\n", AppVersion::version(), title,
      seconds, fps, fps * 100.0f / 59.73f, static_cast<float>(perfTotalEmuUs) / perfTotalFrames / 1000.0f,
      perfTotalEmuMaxUs / 1000.0f, static_cast<unsigned long long>(perfTotalWaits), perfTotalDisplays / seconds,
      perfTotalDisplays ? perfTotalDrawUs / 1000.0f / perfTotalDisplays : 0.0f,
      perfTotalDisplays ? perfTotalRefreshUs / 1000.0f / perfTotalDisplays : 0.0f, perfTotalRefreshMaxUs / 1000.0f,
      perfTotalLoops / seconds, static_cast<unsigned>(perfTotalLoopMaxGapUs / 1000), portrait ? "portrait" : "landscape",
      panel, kRefreshPresets[refreshPreset].label, static_cast<unsigned long long>(perfTotalCleanRefreshes),
      perfTotalPhaseUs[0] / phaseDiv, perfTotalPhaseUs[1] / phaseDiv, perfTotalPhaseUs[2] / phaseDiv,
      perfTotalPhaseUs[3] / phaseDiv, perfTotalPhaseUs[4] / phaseDiv,
      static_cast<unsigned long long>(perfTotalUnchanged), perfTotalPhaseUs[5] / 1.024f / phaseDiv);
  if (len <= 0 || static_cast<size_t>(len) >= sizeof(line)) return;
  static constexpr char kPerfLog[] = "/.crosspoint/gameboy-perf.txt";
  static constexpr char kHeader[] =
      "# version,rom,seconds,emu_fps,speed_pct,frame_ms_avg,frame_ms_max,waits,displays_per_s,draw_ms_avg,"
      "refresh_ms_avg,refresh_ms_max,loops_per_s,loop_gap_ms_max,orientation,panel,preset,clean_refreshes,copy_ms,new_ms,setup_ms,wave_ms,old_ms,unchanged,plane_kb\n";
  HalFile file = Storage.open(kPerfLog, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  if (file.size() == 0) file.write(kHeader, sizeof(kHeader) - 1);
  if (file.size() < 64 * 1024) file.write(line, static_cast<size_t>(len));
  file.close();
}

void GameBoyActivity::setLayout() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  controlCount = 0;

  if (portrait) {
    // 480x800: the game fills the width on top (y a multiple of 8 for
    // drawGameFramePortrait()), the D-pad and B/A below it as on the handheld,
    // then a row of EXIT, SELECT, START and ROTATE.
    scale = std::max(1, std::min(MAX_GAME_SCALE, screenW / GAMEBOY_WIDTH));
    gameW = GAMEBOY_WIDTH * scale;
    gameH = GAMEBOY_HEIGHT * scale;
    gameX = (screenW - gameW) / 2;
    gameY = 40;
    titleX = 8;
    titleY = 10;

    const int padY = gameY + gameH + 24;
    const int pad = 64;
    const int padX = 14;
    addControl(TouchControl::Up, padX + pad, padY, pad, pad, "U");
    addControl(TouchControl::Left, padX, padY + pad, pad, pad, "L");
    addControl(TouchControl::Right, padX + 2 * pad, padY + pad, pad, pad, "R");
    addControl(TouchControl::Down, padX + pad, padY + 2 * pad, pad, pad, "D");
    const int ab = 80;
    addControl(TouchControl::A, screenW - 20 - ab, padY + 24, ab, ab, "A");
    addControl(TouchControl::B, screenW - 36 - 2 * ab, padY + 24 + ab * 3 / 4, ab, ab, "B");

    const int rowH = 52;
    const int rowY = screenH - 12 - rowH;
    const int rowW = (screenW - 8 - 3 * TOUCH_TARGET_GAP) / 4;
    const TouchControl row[] = {TouchControl::Exit, TouchControl::Select, TouchControl::Start, TouchControl::Rotate};
    const char* rowLabels[] = {"EXIT", "SEL", "START", "ROTATE"};
    for (int i = 0; i < 4; ++i) {
      addControl(row[i], 4 + i * (rowW + TOUCH_TARGET_GAP), rowY, rowW, rowH, rowLabels[i]);
    }
    return;
  }

  // 800x480: the game in the middle, the D-pad and ROTATE on the left, B/A,
  // SELECT/START and EXIT on the right.
  const int availableH = std::max(GAMEBOY_HEIGHT, screenH - 48);
  scale = std::min({MAX_GAME_SCALE, std::max(GAMEBOY_WIDTH, screenW) / GAMEBOY_WIDTH, availableH / GAMEBOY_HEIGHT});
  scale = std::max(1, scale);
  gameW = GAMEBOY_WIDTH * scale;
  gameH = GAMEBOY_HEIGHT * scale;
  gameX = (screenW - gameW) / 2;
  gameY = std::max(0, (screenH - gameH) / 2);
  titleX = 6;
  titleY = 18;

  const int leftW = gameX;
  const int panelY = gameY + 120;
  const int centerX = leftW / 2;
  addControl(TouchControl::Up, centerX - 35, panelY, 70, 58, "U");
  addControl(TouchControl::Down, centerX - 35, panelY + 140, 70, 58, "D");
  addControl(TouchControl::Left, 10, panelY + 58, 60, 82, "L");
  addControl(TouchControl::Right, leftW - 70, panelY + 58, 60, 82, "R");
  addControl(TouchControl::Rotate, 4, gameY + gameH - 52, leftW - 8, 48, "ROTATE");

  const int rightX = gameX + gameW;
  const int rightW = screenW - rightX;
  const int buttonW = std::min(70, std::max(48, (rightW - TOUCH_TARGET_GAP) / 2));
  const int firstX = rightX + 4;
  const int secondX = firstX + buttonW + TOUCH_TARGET_GAP;
  addControl(TouchControl::B, firstX, panelY, buttonW, 70, "B");
  addControl(TouchControl::A, secondX, panelY, buttonW, 70, "A");
  addControl(TouchControl::Select, firstX, gameY + 275, buttonW, 52, "SEL");
  addControl(TouchControl::Start, secondX, gameY + 275, buttonW, 52, "START");
  addControl(TouchControl::Exit, rightX + 4, gameY + gameH - 52, rightW - 8, 48, "EXIT");
}

void GameBoyActivity::addControl(const TouchControl control, const int x, const int y, const int w, const int h,
                                 const char* label) {
  if (controlCount >= MAX_CONTROLS) return;
  controls[controlCount++] = {control, x, y, w, h, label};
}

// Hands the refresh preset to the display driver (under the render lock: a
// refresh may be running) and asks for a cleaning refresh next.
void GameBoyActivity::applyRefreshPreset() {
  const RefreshPreset& p = kRefreshPresets[refreshPreset];
  {
    RenderLock lock(*this);
#ifndef SIMULATOR
    display.finishOverlapped();
    display.setFastRefreshTuning(p.tsset, p.pll);
    display.setFastRefreshDuScale(p.duPct);
    display.setFastRefreshDiffWindow(p.diffWindow);
    display.setDisplaySpiHz(p.spiHz);
#else
    (void)p;
#endif
  }
  nextCleanUs = nowMicros() + CLEAN_PERIOD_US;
  fullRefreshDue.store(true, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
}

void GameBoyActivity::rotate() {
  // The buttons move, so the finger on ROTATE must not press whatever lands
  // under it.
  mappedInput.suppressCurrentTouchContact();
  activeTouch = TouchControl::None;
  touchJoypadMask = 0;
  applyJoypad();
  {
    RenderLock lock(*this);
    portrait = !portrait;
    renderer.setOrientation(portrait ? GfxRenderer::Portrait : GfxRenderer::LandscapeCounterClockwise);
    setLayout();
  }
  fullRefreshDue.store(true, std::memory_order_release);
  displayDue.store(true, std::memory_order_release);
  requestUpdate();
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
  std::memset(frameBuffers[0], 0, EMULATOR_FRAME_BYTES);  // white
  std::memset(frameBuffers[1], 0, EMULATOR_FRAME_BYTES);
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
  if (!saveDirty || cartRamSize == 0 || !cartRam || !rom || !isBatteryBackedCart(rom[0x147])) return true;

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
  saveDirty = false;
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
  saveDirty = false;
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
  if (control == TouchControl::Rotate) {
    rotateRequested = true;
    return;
  }
  if (control == activeTouch) return;
  activeTouch = control;
  touchJoypadMask = joypadBitForTouch(control);
  applyJoypad();
}

GameBoyActivity::TouchControl GameBoyActivity::hitTestTouch(const int x, const int y) const {
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& c = controls[i];
    if (hitRect(x, y, c.x, c.y, c.w, c.h)) return c.control;
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
    if (writeFrame == pending || writeFrame == rendering) {
      ++perfWaits;
      break;
    }

    const uint64_t frameStartUs = nowMicros();
    gb_run_frame(gb);
    const uint32_t frameUs = static_cast<uint32_t>(nowMicros() - frameStartUs);
    perfEmuUs += frameUs;
    perfEmuMaxUs = std::max(perfEmuMaxUs, frameUs);
    ++perfFrames;
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

  const bool drawn =
      portrait ? drawGameFramePortrait(source, framebuffer) : drawGameFrameLandscape(source, framebuffer);
  if (!drawn) renderFrame.store(0xFF, std::memory_order_release);
}

namespace {

// Builds one panel row of the game a byte at a time: `count` game pixels read
// from `src` every `step` bytes, each `scale` panel pixels long. A game pixel
// becomes the bits patterns[shade][first panel pixel's parity] (1 = black,
// most significant first), so the row is about count table lookups.
constexpr int MAX_GAME_ROW_BYTES = 64;
void buildGameRow(uint8_t* out, const uint8_t* src, const int step, const int count, const int scale,
                  const uint8_t (&patterns)[4][2]) {
  uint32_t acc = 0;
  int bits = 0;
  for (int g = 0; g < count; ++g) {
    acc = (acc << scale) | patterns[src[g * step] & 0x03][(g * scale) & 1];
    bits += scale;
    if (bits >= 8) {
      bits -= 8;
      *out++ = static_cast<uint8_t>(~(acc >> bits));  // the framebuffer's 1 is white
      acc &= (1u << bits) - 1u;
    }
  }
}

// patterns[fixedParity][shade][alongParity] for buildGameRow: the dither of
// shadeIsBlack() for a panel row whose fixed coordinate has parity
// fixedParity. alongIsX says whether the row runs along the dither's x.
void gamePatterns(uint8_t (&patterns)[2][4][2], const int scale, const bool alongIsX) {
  for (int fixed = 0; fixed < 2; ++fixed) {
    for (int shade = 0; shade < 4; ++shade) {
      for (int along0 = 0; along0 < 2; ++along0) {
        uint8_t pattern = 0;
        for (int k = 0; k < scale; ++k) {
          const int along = along0 + k;
          const bool black = alongIsX ? shadeIsBlack(shade, along, fixed) : shadeIsBlack(shade, fixed, along);
          pattern = static_cast<uint8_t>((pattern << 1) | (black ? 1 : 0));
        }
        patterns[fixed][shade][along0] = pattern;
      }
    }
  }
}

}  // namespace

bool GameBoyActivity::drawGameFrameLandscape(const uint8_t* source, uint8_t* framebuffer) {
  // LandscapeCounterClockwise is the native X4 Pro framebuffer orientation,
  // so logical (x,y) maps directly to physical (x,y). Peanut-GB's four shades
  // become black, two dithered greys and white (shadeIsBlack()). A game line's
  // panel rows differ only by their y parity: two are built, the rest copied.
  const int panelBytes = renderer.getDisplayWidthBytes();
  const int screenW = renderer.getDisplayWidth();
  const int rowBytes = gameW / 8;
  if (panelBytes <= 0 || screenW <= 0 || gameX < 0 || gameX % 8 != 0 || gameX + gameW > screenW ||
      rowBytes > MAX_GAME_ROW_BYTES) {
    return false;
  }
  uint8_t patterns[2][4][2];
  gamePatterns(patterns, scale, /*alongIsX=*/true);
  uint8_t rows[2][MAX_GAME_ROW_BYTES];
  for (int gy = 0; gy < GAMEBOY_HEIGHT; ++gy) {
    const uint8_t* line = source + gy * GAMEBOY_WIDTH;
    for (int parity = 0; parity < 2; ++parity) {
      buildGameRow(rows[parity], line, 1, GAMEBOY_WIDTH, scale, patterns[parity]);
    }
    for (int sy = 0; sy < scale; ++sy) {
      const int panelY = gy * scale + sy;
      uint8_t* out = framebuffer + static_cast<size_t>(gameY + panelY) * panelBytes + gameX / 8;
      std::memcpy(out, rows[panelY & 1], static_cast<size_t>(rowBytes));
    }
  }
  return true;
}

bool GameBoyActivity::drawGameFramePortrait(const uint8_t* source, uint8_t* framebuffer) {
  // Portrait turns the panel a quarter: logical (x, y) is panel
  // (y, panelHeight - 1 - x), so each game column is one panel row with the
  // game's lines running along it. A column's panel rows differ only by their
  // x parity: two are built, the rest copied.
  const int panelBytes = renderer.getDisplayWidthBytes();
  const int panelW = renderer.getDisplayWidth();
  const int panelH = renderer.getDisplayHeight();
  const int rowBytes = gameH / 8;
  if (panelBytes <= 0 || gameY % 8 != 0 || gameH % 8 != 0 || gameX < 0 || gameX + gameW > panelH ||
      gameY + gameH > panelW || rowBytes > MAX_GAME_ROW_BYTES) {
    return false;
  }
  uint8_t patterns[2][4][2];
  gamePatterns(patterns, scale, /*alongIsX=*/false);
  uint8_t rows[2][MAX_GAME_ROW_BYTES];
  for (int gx = 0; gx < GAMEBOY_WIDTH; ++gx) {
    for (int parity = 0; parity < 2; ++parity) {
      buildGameRow(rows[parity], source + gx, GAMEBOY_WIDTH, GAMEBOY_HEIGHT, scale, patterns[parity]);
    }
    for (int sx = 0; sx < scale; ++sx) {
      const int dstX = gx * scale + sx;
      uint8_t* out = framebuffer + static_cast<size_t>(panelH - 1 - (gameX + dstX)) * panelBytes + gameY / 8;
      std::memcpy(out, rows[dstX & 1], static_cast<size_t>(rowBytes));
    }
  }
  return true;
}

void GameBoyActivity::drawControls() {
  const uint8_t currentMask = static_cast<uint8_t>(physicalJoypadMask | touchJoypadMask);

  renderer.drawRect(gameX - 1, gameY - 1, gameW + 2, gameH + 2, true);
  renderer.drawText(UI_10_FONT_ID, titleX, titleY, title[0] ? title : "GAME BOY", true, EpdFontFamily::BOLD);
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& c = controls[i];
    const uint8_t bit = joypadBitForTouch(c.control);
    const bool active = bit != 0 ? (currentMask & bit) != 0 : activeTouch == c.control;
    renderer.fillRect(c.x + 1, c.y + 1, c.w - 2, c.h - 2, active);
    renderer.drawRect(c.x, c.y, c.w, c.h, true);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, c.label);
    renderer.drawText(UI_10_FONT_ID, c.x + (c.w - tw) / 2, c.y + c.h / 2 - 7, c.label, !active,
                      EpdFontFamily::BOLD);
  }
}

void GameBoyActivity::registerTouchTargets() {
#if CROSSINK_APP_CAP_TOUCH
  auto& registry = TouchRegistry::getInstance();
  for (size_t i = 0; i < controlCount; ++i) {
    const ControlButton& c = controls[i];
    registry.add(Rect(c.x, c.y, c.w, c.h), static_cast<int>(c.control), TouchRegistry::Kind::Button);
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
  if (self->cartRam[addr] != value) self->saveDirty = true;
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
