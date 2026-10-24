#include "GameLibraryActivity.h"

#if CROSSINK_GAMEBOY

#include <Arduino.h>
#include <FsHelpers.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <strings.h>

#include "ActivityManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

#if !defined(SIMULATOR) && defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
#define INKBOY_HAS_BLE_HID 1
#include <BleKeyboardHost.h>
#else
#define INKBOY_HAS_BLE_HID 0
#endif

namespace {
constexpr char kLibraryStatePath[] = "/.crosspoint/game_library.txt";
constexpr char kLibraryTempPath[] = "/.crosspoint/game_library.tmp";
constexpr char kTag[] = "GAMES";
constexpr size_t kNameBufferSize = 256;
constexpr size_t kMaxDirectoriesPerFolder = 64;

bool shouldSkipDirectory(const char* name) {
  if (!name || !name[0] || name[0] == '.') return true;
  return strcasecmp(name, "Android") == 0 || strcasecmp(name, "LOST.DIR") == 0 ||
         strcasecmp(name, "System Volume Information") == 0 || strcasecmp(name, "Recycle.Bin") == 0;
}

bool containsPath(const std::vector<std::string>& paths, const std::string& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

}  // namespace

GameLibraryActivity::GameLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GameLibrary", renderer, mappedInput) {}

void GameLibraryActivity::onEnter() {
  Activity::onEnter();
  screen = Screen::Categories;
  filter = Filter::All;
  selectedIndex = 0;
  visibleStart = 0;
  games.clear();
  visibleGames.clear();
  favorites.clear();
  recents.clear();
  settings = GameBoyUserSettings{};
  loadGameBoyUserSettings(settings);
  loadLibraryState();

  // One bounded SD-card scan per visit. Directory handles are closed before the
  // next recursion level because the physical SD backend has one-reader semantics.
  scanDirectory("/", 0);
  std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  rebuildVisibleGames();
  status[0] = '\0';
  requestUpdate();
}

void GameLibraryActivity::onExit() {
  stopBluetooth();
  Activity::onExit();
}

std::string GameLibraryActivity::titleForPath(const std::string& path) const {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t end = dot != std::string::npos && dot > start ? dot : path.size();
  return path.substr(start, end - start);
}

void GameLibraryActivity::scanDirectory(const std::string& path, const unsigned depth) {
  if (depth > MAX_SCAN_DEPTH || games.size() >= MAX_GAMES) return;

  auto root = Storage.open(path.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  std::vector<std::string> subdirectories;
  subdirectories.reserve(12);
  root.rewindDirectory();
  char name[kNameBufferSize] = {};
  while (games.size() < MAX_GAMES) {
    auto entry = root.openNextFile();
    if (!entry) break;
    std::memset(name, 0, sizeof(name));
    entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();

    if (name[0] != '\0') {
      if (isDirectory) {
        if (depth < MAX_SCAN_DEPTH && !shouldSkipDirectory(name) &&
            subdirectories.size() < kMaxDirectoriesPerFolder) {
          const std::string subPath = path == "/" ? path + name : path + "/" + name;
          subdirectories.push_back(subPath);
        }
      } else if ((FsHelpers::checkFileExtension(std::string_view{name}, ".gb") ||
                  FsHelpers::checkFileExtension(std::string_view{name}, ".gbc")) &&
                 games.size() < MAX_GAMES) {
        const std::string fullPath = path == "/" ? path + name : path + "/" + name;
        const bool alreadyAdded = std::any_of(games.begin(), games.end(), [&fullPath](const GameEntry& g) {
          return g.path == fullPath;
        });
        if (!alreadyAdded) games.push_back({fullPath, titleForPath(fullPath)});
      }
    }
    entry.close();
  }

  if (FsHelpers::directoryIterationFailed(root)) {
    LOG_ERR(kTag, "ROM scan stopped early: %s", path.c_str());
  }
  root.close();

  for (const auto& child : subdirectories) {
    if (games.size() >= MAX_GAMES) break;
    scanDirectory(child, depth + 1);
  }
}

void GameLibraryActivity::loadLibraryState() {
  FsFile file;
  if (!Storage.openFileForRead(kTag, kLibraryStatePath, file)) return;

  std::string line;
  line.reserve(256);
  bool tooLong = false;
  while (true) {
    const int c = file.read();
    if (c < 0) break;
    if (c != '\n') {
      if (line.size() < 768) line.push_back(static_cast<char>(c));
      else tooLong = true;
      continue;
    }
    if (!tooLong && line.size() > 2 && line[1] == '\t') {
      const std::string path = line.substr(2);
      if (line[0] == 'F' && favorites.size() < MAX_FAVORITES && !containsPath(favorites, path)) {
        favorites.push_back(path);
      } else if (line[0] == 'R' && recents.size() < MAX_RECENTS && !containsPath(recents, path)) {
        recents.push_back(path);
      }
    }
    line.clear();
    tooLong = false;
  }
  if (!line.empty() && !tooLong && line.size() > 2 && line[1] == '\t') {
    const std::string path = line.substr(2);
    if (line[0] == 'F' && favorites.size() < MAX_FAVORITES && !containsPath(favorites, path)) {
      favorites.push_back(path);
    } else if (line[0] == 'R' && recents.size() < MAX_RECENTS && !containsPath(recents, path)) {
      recents.push_back(path);
    }
  }
  file.close();
}

bool GameLibraryActivity::saveLibraryState() {
  (void)Storage.mkdir("/.crosspoint");
  std::string contents;
  contents.reserve(4096);
  for (const auto& path : favorites) contents += "F\t" + path + "\n";
  for (const auto& path : recents) contents += "R\t" + path + "\n";

  FsFile file;
  if (!Storage.openFileForWrite(kTag, kLibraryTempPath, file)) {
    LOG_ERR(kTag, "Could not open library state temp file");
    return false;
  }
  const bool ok = file.write(contents.data(), contents.size()) == contents.size() && file.sync();
  file.close();
  if (!ok) {
    Storage.remove(kLibraryTempPath);
    LOG_ERR(kTag, "Could not write library state");
    return false;
  }
  Storage.remove(kLibraryStatePath);
  if (!Storage.rename(kLibraryTempPath, kLibraryStatePath)) {
    LOG_ERR(kTag, "Could not promote library state");
    return false;
  }
  return true;
}

bool GameLibraryActivity::isFavorite(const std::string& path) const {
  return containsPath(favorites, path);
}

void GameLibraryActivity::toggleFavorite(const std::string& path) {
  const auto found = std::find(favorites.begin(), favorites.end(), path);
  if (found == favorites.end()) {
    if (favorites.size() < MAX_FAVORITES) favorites.push_back(path);
  } else {
    favorites.erase(found);
  }
  saveLibraryState();
  rebuildVisibleGames();
}

void GameLibraryActivity::recordRecent(const std::string& path) {
  recents.erase(std::remove(recents.begin(), recents.end(), path), recents.end());
  recents.insert(recents.begin(), path);
  if (recents.size() > MAX_RECENTS) recents.resize(MAX_RECENTS);
  saveLibraryState();
  rebuildVisibleGames();
}

void GameLibraryActivity::rebuildVisibleGames() {
  visibleGames.clear();
  visibleGames.reserve(games.size());
  if (filter == Filter::Recent) {
    for (const auto& path : recents) {
      for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].path == path) {
          visibleGames.push_back(i);
          break;
        }
      }
    }
    return;
  }

  for (size_t i = 0; i < games.size(); ++i) {
    if (filter == Filter::Favorites && !isFavorite(games[i].path)) continue;
    visibleGames.push_back(i);
  }
}

size_t GameLibraryActivity::screenItemCount() const {
  switch (screen) {
    case Screen::Categories: return 5;
    case Screen::Games: return visibleGames.size();
    case Screen::Settings: return 5;
#if INKBOY_HAS_BLE_HID
    case Screen::Bluetooth: return static_cast<size_t>(BleHid.deviceCount()) + 2;
#else
    case Screen::Bluetooth: return 1;
#endif
  }
  return 0;
}

void GameLibraryActivity::openBluetoothScreen(bool& beginScan) {
  screen = Screen::Bluetooth;
  selectedIndex = 0;
  visibleStart = 0;
  settings.bluetoothEnabled = 1;
  saveGameBoyUserSettings(settings);
  beginScan = true;
}

void GameLibraryActivity::refreshBluetooth() {
  char nextStatus[sizeof(status)] = {};
#if INKBOY_HAS_BLE_HID
  if (BleHid.isStopping() && !BleHid.end(1000)) {
    std::snprintf(nextStatus, sizeof(nextStatus), "Bluetooth is still shutting down");
  } else if (!BleHid.isRunning() && !BleHid.begin("Ink-boy")) {
    std::snprintf(nextStatus, sizeof(nextStatus), "Bluetooth initialization failed");
  } else {
    BleHid.startScan(8000);
    lastBluetoothDeviceCount = BleHid.deviceCount();
    std::snprintf(nextStatus, sizeof(nextStatus), "Scanning for BLE HID controllers...");
  }
#else
  std::snprintf(nextStatus, sizeof(nextStatus), "BLE HID is available in the X4 Pro hardware build only");
#endif
  // BLE APIs run outside RenderLock. Publish status only while drawing is excluded.
  {
    RenderLock lock(*this);
    std::strncpy(status, nextStatus, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
  }
  requestUpdate();
}

void GameLibraryActivity::stopBluetooth() {
#if INKBOY_HAS_BLE_HID
  if (BleHid.isRunning() || BleHid.isStopping()) {
    if (!BleHid.end(1000) && !BleHid.end(1000)) {
      LOG_ERR(kTag, "BLE host is still stopping");
    }
  }
#endif
}

void GameLibraryActivity::activateSelected(std::string& launchPath, bool& beginScan, bool& shutdownBluetooth) {
  switch (screen) {
    case Screen::Categories:
      if (selectedIndex <= 2) {
        filter = selectedIndex == 0 ? Filter::All : (selectedIndex == 1 ? Filter::Favorites : Filter::Recent);
        screen = Screen::Games;
        selectedIndex = 0;
        visibleStart = 0;
        rebuildVisibleGames();
      } else if (selectedIndex == 3) {
        screen = Screen::Settings;
        selectedIndex = 0;
        visibleStart = 0;
      } else {
        openBluetoothScreen(beginScan);
      }
      break;

    case Screen::Games:
      if (selectedIndex < visibleGames.size()) {
        const std::string path = games[visibleGames[selectedIndex]].path;
        recordRecent(path);
        launchPath = path;
      }
      break;

    case Screen::Settings:
      switch (selectedIndex) {
        case 0: settings.layout = settings.layout == 0 ? 1 : 0; break;
        case 1: settings.refresh = static_cast<uint8_t>((settings.refresh + 1) % 3); break;
        case 2: settings.touchControls = settings.touchControls == 0 ? 1 : 0; break;
        case 3:
          settings.bluetoothEnabled = settings.bluetoothEnabled == 0 ? 1 : 0;
          shutdownBluetooth = settings.bluetoothEnabled == 0;
          break;
        case 4:
          openBluetoothScreen(beginScan);
          return;
      }
      saveGameBoyUserSettings(settings);
      std::snprintf(status, sizeof(status), "Settings saved");
      break;

    case Screen::Bluetooth:
#if INKBOY_HAS_BLE_HID
      if (BleHid.isRunning()) {
        const size_t deviceCount = BleHid.deviceCount();
        if (selectedIndex < deviceCount) {
          const auto& device = BleHid.device(static_cast<uint8_t>(selectedIndex));
          if (device.hid && device.connectable && BleHid.connect(device.addr)) {
            std::snprintf(status, sizeof(status), "Connecting to %.31s...", device.name);
          } else {
            std::snprintf(status, sizeof(status), "That device is not connectable BLE HID");
          }
        } else if (selectedIndex == deviceCount) {
          beginScan = true;
        } else {
          if (BleHid.isConnected()) {
            BleHid.disconnect();
            std::snprintf(status, sizeof(status), "Controller disconnected");
          } else {
            std::snprintf(status, sizeof(status), "No controller connected");
          }
        }
      } else {
        beginScan = true;
      }
#else
      std::snprintf(status, sizeof(status), "Use the X4 Pro build for BLE HID");
#endif
      break;
  }
}

void GameLibraryActivity::loop() {
#if INKBOY_HAS_BLE_HID
  if (BleHid.isRunning()) {
    BleHid.poll();
    const uint8_t discoveredNow = BleHid.deviceCount();
    if (discoveredNow != lastBluetoothDeviceCount) {
      lastBluetoothDeviceCount = discoveredNow;
      requestUpdate();
    }
    char nextStatus[sizeof(status)] = {};
    char failure[48] = {};
    bool statusEvent = false;
    if (BleHid.takeConnectFailure(failure, sizeof(failure))) {
      std::snprintf(nextStatus, sizeof(nextStatus), "Connect failed: %.45s", failure);
      statusEvent = true;
    }
    uint32_t passkey = 0;
    if (BleHid.takePairingPasskey(passkey)) {
      std::snprintf(nextStatus, sizeof(nextStatus), "Pairing code: %06lu", static_cast<unsigned long>(passkey));
      statusEvent = true;
    }
    if (!statusEvent && BleHid.isConnected()) {
      std::snprintf(nextStatus, sizeof(nextStatus), "Connected: %.31s", BleHid.connectedName());
      statusEvent = true;
    }
    if (statusEvent) {
      bool changed = false;
      {
        RenderLock lock(*this);
        changed = std::strncmp(status, nextStatus, sizeof(status)) != 0;
        if (changed) {
          std::strncpy(status, nextStatus, sizeof(status) - 1);
          status[sizeof(status) - 1] = '\0';
        }
      }
      if (changed) requestUpdate();
    }
  }
#endif

  using Button = MappedInputManager::Button;
  int touched = -1;
  if (mappedInput.wasItemTapped(touched)) {
    std::string launchPath;
    bool beginScan = false;
    bool shutdownBluetooth = false;
    {
      RenderLock lock(*this);
      const size_t absolute = visibleStart + static_cast<size_t>(std::max(0, touched));
      if (absolute < screenItemCount()) selectedIndex = absolute;
      activateSelected(launchPath, beginScan, shutdownBluetooth);
    }
    if (beginScan) refreshBluetooth();
    if (shutdownBluetooth) stopBluetooth();
    requestUpdate();
    if (!launchPath.empty()) activityManager.goToGameBoy(std::move(launchPath));
    return;
  }

  if (!mappedInput.wasReleased(Button::Up) && !mappedInput.wasReleased(Button::Down) &&
      !mappedInput.wasReleased(Button::Left) && !mappedInput.wasReleased(Button::Right) &&
      !mappedInput.wasReleased(Button::Confirm) && !mappedInput.wasReleased(Button::Back) &&
      !mappedInput.wasReleased(Button::Power)) {
    return;
  }

  std::string launchPath;
  bool beginScan = false;
  bool shutdownBluetooth = false;
  bool exit = false;
  bool changed = false;
  {
    RenderLock lock(*this);
    const size_t count = screenItemCount();
    if (mappedInput.wasReleased(Button::Back)) {
      if (screen == Screen::Categories) {
        exit = true;
      } else {
        screen = Screen::Categories;
        selectedIndex = 0;
        visibleStart = 0;
        changed = true;
      }
    } else if (mappedInput.wasReleased(Button::Up) && count > 0) {
      selectedIndex = selectedIndex == 0 ? count - 1 : selectedIndex - 1;
      changed = true;
    } else if (mappedInput.wasReleased(Button::Down) && count > 0) {
      selectedIndex = (selectedIndex + 1) % count;
      changed = true;
    } else if (screen == Screen::Games && mappedInput.wasReleased(Button::Left)) {
      filter = static_cast<Filter>((static_cast<uint8_t>(filter) + 2) % 3);
      selectedIndex = 0;
      rebuildVisibleGames();
      changed = true;
    } else if (screen == Screen::Games && mappedInput.wasReleased(Button::Right)) {
      filter = static_cast<Filter>((static_cast<uint8_t>(filter) + 1) % 3);
      selectedIndex = 0;
      rebuildVisibleGames();
      changed = true;
    } else if (screen == Screen::Games && mappedInput.wasReleased(Button::Power) &&
               selectedIndex < visibleGames.size()) {
      toggleFavorite(games[visibleGames[selectedIndex]].path);
      selectedIndex = std::min(selectedIndex, visibleGames.empty() ? size_t{0} : visibleGames.size() - 1);
      changed = true;
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      activateSelected(launchPath, beginScan, shutdownBluetooth);
      changed = true;
    }
    if (count > 0 && selectedIndex >= count) selectedIndex = count - 1;
  }

  if (beginScan) refreshBluetooth();
  if (shutdownBluetooth) stopBluetooth();
  if (!launchPath.empty()) {
    requestUpdate();
    activityManager.goToGameBoy(std::move(launchPath));
    return;
  }
  if (exit) {
    finish();
    return;
  }
  if (changed) requestUpdate();
}

void GameLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen(0xFF);
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.homeTopPadding}, nullptr);

  std::string header;
  std::vector<std::string> labels;
  labels.reserve(MAX_VISIBLE_ROWS);
  size_t count = 0;
  size_t selectedLocal = 0;

  switch (screen) {
    case Screen::Categories:
      header = tr(STR_GAME_LIBRARY);
      labels.push_back(std::string(tr(STR_GAME_ALL)) + " (" + std::to_string(games.size()) + ")");
      labels.push_back(std::string(tr(STR_GAME_FAVORITES)) + " (" +
                       std::to_string(std::count_if(games.begin(), games.end(), [this](const GameEntry& g) {
                         return isFavorite(g.path);
                       })) + ")");
      labels.push_back(std::string(tr(STR_GAME_RECENT)) + " (" + std::to_string(recents.size()) + ")");
      labels.push_back(tr(STR_GAME_SETTINGS));
      labels.push_back(tr(STR_GAME_CONTROLLER));
      count = labels.size();
      selectedLocal = selectedIndex;
      break;

    case Screen::Games: {
      header = filter == Filter::All ? tr(STR_GAME_ALL)
               : filter == Filter::Favorites ? tr(STR_GAME_FAVORITES) : tr(STR_GAME_RECENT);
      count = std::min(MAX_VISIBLE_ROWS, visibleGames.size());
      size_t start = selectedIndex > MAX_VISIBLE_ROWS / 2 ? selectedIndex - MAX_VISIBLE_ROWS / 2 : 0;
      if (start + count > visibleGames.size()) start = visibleGames.size() > count ? visibleGames.size() - count : 0;
      visibleStart = start;
      for (size_t i = 0; i < count; ++i) {
        const auto& game = games[visibleGames[start + i]];
        labels.push_back((isFavorite(game.path) ? "* " : "  ") + game.title);
      }
      selectedLocal = selectedIndex >= visibleStart ? selectedIndex - visibleStart : 0;
      if (visibleGames.empty()) {
        labels.emplace_back(tr(STR_GAME_NO_ROMS));
        count = 1;
        selectedLocal = 0;
        visibleStart = 0;
      }
      break;
    }

    case Screen::Settings:
      header = tr(STR_GAME_SETTINGS);
      labels.push_back(std::string(tr(STR_GAME_LAYOUT)) + ": " +
                       (settings.layout == 0 ? tr(STR_GAME_PORTRAIT) : tr(STR_GAME_LANDSCAPE)));
      labels.push_back(std::string(tr(STR_GAME_REFRESH_MODE)) + ": " +
                       (settings.refresh == 0 ? tr(STR_GAME_REFRESH_AUTO)
                        : settings.refresh == 1 ? tr(STR_GAME_REFRESH_STOCK) : tr(STR_GAME_REFRESH_FAST)));
      labels.push_back(std::string(tr(STR_GAME_TOUCH_CONTROLS)) + ": " +
                       (settings.touchControls ? tr(STR_ON) : tr(STR_OFF)));
      labels.push_back(std::string(tr(STR_GAME_BLUETOOTH)) + ": " +
                       (settings.bluetoothEnabled ? tr(STR_ON) : tr(STR_OFF)));
      labels.push_back(tr(STR_GAME_MANAGE_CONTROLLER));
      count = labels.size();
      selectedLocal = selectedIndex;
      break;

    case Screen::Bluetooth:
      header = tr(STR_GAME_CONTROLLER);
#if INKBOY_HAS_BLE_HID
      if (BleHid.isRunning()) {
        const size_t deviceCount = BleHid.deviceCount();
        const size_t total = deviceCount + 2;
        size_t start = selectedIndex > MAX_VISIBLE_ROWS / 2 ? selectedIndex - MAX_VISIBLE_ROWS / 2 : 0;
        count = std::min(MAX_VISIBLE_ROWS, total);
        if (start + count > total) start = total > count ? total - count : 0;
        visibleStart = start;
        for (size_t i = 0; i < count; ++i) {
          const size_t index = start + i;
          if (index < deviceCount) {
            const auto& device = BleHid.device(static_cast<uint8_t>(index));
            char label[64];
            std::snprintf(label, sizeof(label), "%s%s (%d dBm)", device.name,
                          device.hid ? "" : " [not HID]", device.rssi);
            labels.emplace_back(label);
          } else if (index == deviceCount) {
            labels.emplace_back(tr(STR_GAME_SCAN_AGAIN));
          } else {
            labels.emplace_back(BleHid.isConnected() ? tr(STR_GAME_DISCONNECT) : tr(STR_GAME_NOT_CONNECTED));
          }
        }
        selectedLocal = selectedIndex >= visibleStart ? selectedIndex - visibleStart : 0;
      } else {
        labels.emplace_back(tr(STR_GAME_SCAN_AGAIN));
        count = 1;
        visibleStart = 0;
        selectedLocal = 0;
      }
#else
      labels.emplace_back(tr(STR_GAME_BLE_X4PRO_ONLY));
      count = 1;
      visibleStart = 0;
      selectedLocal = 0;
#endif
      break;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.homeTopPadding + 15, header.c_str(), true, EpdFontFamily::BOLD);
  if (status[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, 8, metrics.homeTopPadding + 30, status, true, EpdFontFamily::REGULAR);
  }
  const int menuTop = metrics.homeTopPadding + (status[0] ? 44 : 28);
  GUI.drawButtonMenu(renderer, Rect{0, menuTop, width, height - menuTop},
                     static_cast<int>(count), static_cast<int>(selectedLocal),
                     [&labels](int index) { return labels[static_cast<size_t>(index)].c_str(); },
                     [](int) { return Folder; });

  MappedInputManager::Labels hints{};
  if (screen == Screen::Categories) {
    hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  } else if (screen == Screen::Games) {
    hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_PLAY), tr(STR_GAME_FILTER), tr(STR_GAME_FAVORITE_ACTION));
  } else {
    hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif
