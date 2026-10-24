#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <cstdint>

struct GameBoyUserSettings {
  // layout: 0 portrait, 1 landscape; refresh: 0 auto, 1 stock, 2 fast-if-supported.
  uint8_t layout = 0;
  uint8_t refresh = 0;
  uint8_t touchControls = 1;
  uint8_t bluetoothEnabled = 0;
  // HID keyboard usage IDs: Up, Down, Left, Right, A, B, Start, Select.
  std::array<uint8_t, 8> controllerUsages{{0x52, 0x51, 0x50, 0x4F, 0x1B, 0x1D, 0x28, 0x29}};
};

inline bool loadGameBoyUserSettings(GameBoyUserSettings& settings) {
  FsFile file;
  if (!Storage.openFileForRead("GBSET", "/.crosspoint/game_settings.bin", file)) return false;

  uint8_t bytes[17] = {};
  const int count = file.read(bytes, sizeof(bytes));
  file.close();

  // V1 used 8 bytes, V2 adds Bluetooth enable, and V3 appends 8 HID usages.
  if (count < 8 || bytes[0] != 'I' || bytes[1] != 'B' || bytes[2] != 'G' || bytes[3] != 'S' ||
      bytes[4] < 1 || bytes[4] > 3 || (bytes[4] == 3 && count < 17)) {
    return false;
  }
  settings.layout = bytes[5] <= 1 ? bytes[5] : 0;
  settings.refresh = bytes[6] <= 2 ? bytes[6] : 0;
  settings.touchControls = bytes[7] <= 1 ? bytes[7] : 1;
  settings.bluetoothEnabled = bytes[4] >= 2 && count >= 9 && bytes[8] <= 1 ? bytes[8] : 0;
  settings.controllerUsages = {{0x52, 0x51, 0x50, 0x4F, 0x1B, 0x1D, 0x28, 0x29}};
  if (bytes[4] == 3) {
    for (size_t i = 0; i < settings.controllerUsages.size(); ++i) {
      if (bytes[9 + i] != 0) settings.controllerUsages[i] = bytes[9 + i];
    }
  }
  return true;
}

inline bool saveGameBoyUserSettings(const GameBoyUserSettings& settings) {
  (void)Storage.mkdir("/.crosspoint");
  uint8_t bytes[17] = {'I', 'B', 'G', 'S', 3,
                       static_cast<uint8_t>(settings.layout <= 1 ? settings.layout : 0),
                       static_cast<uint8_t>(settings.refresh <= 2 ? settings.refresh : 0),
                       static_cast<uint8_t>(settings.touchControls != 0),
                       static_cast<uint8_t>(settings.bluetoothEnabled != 0)};
  for (size_t i = 0; i < settings.controllerUsages.size(); ++i) {
    bytes[9 + i] = settings.controllerUsages[i];
  }

  constexpr char tempPath[] = "/.crosspoint/game_settings.tmp";
  constexpr char finalPath[] = "/.crosspoint/game_settings.bin";
  FsFile file;
  if (!Storage.openFileForWrite("GBSET", tempPath, file)) {
    LOG_ERR("GBSET", "Could not open temporary settings file");
    return false;
  }
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes) && file.sync();
  file.close();
  if (!written) {
    Storage.remove(tempPath);
    LOG_ERR("GBSET", "Could not write settings file");
    return false;
  }

  Storage.remove(finalPath);
  if (!Storage.rename(tempPath, finalPath)) {
    LOG_ERR("GBSET", "Could not promote settings file");
    return false;
  }
  return true;
}
