#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>

struct GameBoyUserSettings {
  // layout: 0 portrait, 1 landscape; refresh: 0 auto, 1 stock, 2 fast-if-supported.
  uint8_t layout = 0;
  uint8_t refresh = 0;
  uint8_t touchControls = 1;
  uint8_t bluetoothEnabled = 0;
};

inline bool loadGameBoyUserSettings(GameBoyUserSettings& settings) {
  FsFile file;
  if (!Storage.openFileForRead("GBSET", "/.crosspoint/game_settings.bin", file)) return false;

  uint8_t bytes[9] = {};
  const int count = file.read(bytes, sizeof(bytes));
  file.close();

  // Version 1 stored layout/refresh/touch as eight bytes. Version 2 adds Bluetooth
  // preference in byte 8. Defaults remain safe when the file is missing or corrupt.
  if (count < 8 || bytes[0] != 'I' || bytes[1] != 'B' || bytes[2] != 'G' || bytes[3] != 'S' ||
      (bytes[4] != 1 && bytes[4] != 2)) {
    return false;
  }
  settings.layout = bytes[5] <= 1 ? bytes[5] : 0;
  settings.refresh = bytes[6] <= 2 ? bytes[6] : 0;
  settings.touchControls = bytes[7] <= 1 ? bytes[7] : 1;
  settings.bluetoothEnabled = bytes[4] == 2 && count >= 9 && bytes[8] <= 1 ? bytes[8] : 0;
  return true;
}

inline bool saveGameBoyUserSettings(const GameBoyUserSettings& settings) {
  (void)Storage.mkdir("/.crosspoint");
  const uint8_t bytes[9] = {'I', 'B', 'G', 'S', 2,
                            static_cast<uint8_t>(settings.layout <= 1 ? settings.layout : 0),
                            static_cast<uint8_t>(settings.refresh <= 2 ? settings.refresh : 0),
                            static_cast<uint8_t>(settings.touchControls != 0),
                            static_cast<uint8_t>(settings.bluetoothEnabled != 0)};
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
