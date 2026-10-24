#pragma once

#include "activities/Activity.h"

#if CROSSINK_GAMEBOY

#include <string>
#include <vector>

#include "GameBoySettingsStore.h"

class GameLibraryActivity final : public Activity {
 public:
  GameLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool blocksGlobalInput() const override { return true; }

 private:
  enum class Screen : uint8_t { Categories, Games, Settings, Bluetooth, ControllerMapping, SaveManager };
  enum class Filter : uint8_t { All, Favorites, Recent };

  struct GameEntry {
    std::string path;
    std::string title;
  };

  static constexpr size_t MAX_GAMES = 256;
  static constexpr size_t MAX_FAVORITES = 128;
  static constexpr size_t MAX_RECENTS = 32;
  static constexpr unsigned MAX_SCAN_DEPTH = 4;
  static constexpr size_t MAX_VISIBLE_ROWS = 7;

  Screen screen = Screen::Categories;
  Filter filter = Filter::All;
  size_t selectedIndex = 0;
  size_t visibleStart = 0;
  std::vector<GameEntry> games;
  std::vector<size_t> visibleGames;
  std::vector<std::string> favorites;
  std::vector<std::string> recents;
  GameBoyUserSettings settings;
  char status[96] = {};
  uint8_t lastBluetoothDeviceCount = 0;
  bool awaitingControllerMapping = false;
  bool confirmDeleteSave = false;
  std::string pendingDeletePath;
  bool lastBluetoothConnected = false;

  void scanDirectory(const std::string& path, unsigned depth);
  void loadLibraryState();
  bool saveLibraryState();
  void rebuildVisibleGames();
  size_t screenItemCount() const;
  std::string titleForPath(const std::string& path) const;
  std::string savePathForRom(const std::string& romPath) const;
  bool deleteSelectedSave();
  bool isFavorite(const std::string& path) const;
  void toggleFavorite(const std::string& path);
  void recordRecent(const std::string& path);
  void openBluetoothScreen(bool& beginScan);
  void openControllerMapping();
  void refreshBluetooth();
  void stopBluetooth();
  void activateSelected(std::string& launchPath, bool& beginScan, bool& shutdownBluetooth);
};

#endif
