#include "GpspGBA.h"

#include <HalStorage.h>
#include <libretro.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <string>

// The variables below are exported by gpSP's C core. Keep the declarations
// local to this adapter rather than importing its platform-specific frontend.
extern "C" {
extern uint32_t backup_type;
extern uint32_t backup_type_reset;
extern uint32_t flash_bank_cnt;
extern uint32_t eeprom_size;
extern uint8_t *gamepak_backup;
int gpsp_alloc_cpu_buffers(void);
void gpsp_free_cpu_buffers(void);

int gpsp_alloc_backup_buffer(void);
void gpsp_free_backup_buffer(void);

int gpsp_alloc_video_buffers(void);
void gpsp_free_video_buffers(void);

int gpsp_alloc_sound_buffer(void);
void gpsp_free_sound_buffer(void);
}

struct retro_vfs_file_handle {
  HalFile file;
  std::string path;
};

namespace {
constexpr char TAG[] = "GBA-CORE";
constexpr uint32_t BACKUP_SRAM_VALUE = 0;
constexpr uint32_t BACKUP_FLASH_VALUE = 1;
constexpr uint32_t BACKUP_EEPROM_VALUE = 2;
constexpr uint32_t BACKUP_UNKN_VALUE = 3;
constexpr uint32_t FLASH_SIZE_128KB_VALUE = 2;
constexpr uint32_t EEPROM_8_KBYTE_VALUE = 16;

bool sCoreInitialized = false;
bool sCoreBuffersAllocated = false;
bool sGameLoaded = false;
uint16_t sJoypadMask = 0;
gpsp::VideoCallback sVideoCallback = nullptr;
void* sVideoUser = nullptr;
char sLastError[160] = {};

const char* RETRO_CALLCONV vfsGetPath(retro_vfs_file_handle* stream) {
  return stream ? stream->path.c_str() : nullptr;
}

retro_vfs_file_handle* RETRO_CALLCONV vfsOpen(const char* path, unsigned mode, unsigned) {
  if (!path || !(mode & RETRO_VFS_FILE_ACCESS_READ) ||
      (mode & (RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING))) {
    return nullptr;
  }

  auto* handle = new (std::nothrow) retro_vfs_file_handle();
  if (!handle) return nullptr;
  handle->path = path;
  if (!Storage.openFileForRead(TAG, handle->path, handle->file)) {
    delete handle;
    return nullptr;
  }
  return handle;
}

int RETRO_CALLCONV vfsClose(retro_vfs_file_handle* stream) {
  if (!stream) return -1;
  stream->file.close();
  delete stream;
  return 0;
}

int64_t RETRO_CALLCONV vfsSize(retro_vfs_file_handle* stream) {
  if (!stream) return -1;
  const uint64_t size = stream->file.fileSize64();
  return size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
             ? -1
             : static_cast<int64_t>(size);
}

int64_t RETRO_CALLCONV vfsTell(retro_vfs_file_handle* stream) {
  if (!stream) return -1;
  const uint64_t pos = stream->file.position();
  return pos > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
             ? -1
             : static_cast<int64_t>(pos);
}

int64_t RETRO_CALLCONV vfsSeek(retro_vfs_file_handle* stream, int64_t offset, int whence) {
  if (!stream) return -1;
  int64_t base = 0;
  if (whence == RETRO_VFS_SEEK_POSITION_CURRENT) {
    const int64_t current = vfsTell(stream);
    if (current < 0) return -1;
    base = current;
  } else if (whence == RETRO_VFS_SEEK_POSITION_END) {
    base = vfsSize(stream);
    if (base < 0) return -1;
  } else if (whence != RETRO_VFS_SEEK_POSITION_START) {
    return -1;
  }

  if ((offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
      (offset < 0 && offset < -base)) {
    return -1;
  }
  const int64_t target = base + offset;
  if (target < 0 || !stream->file.seekSet(static_cast<uint64_t>(target))) return -1;
  return target;
}

int64_t RETRO_CALLCONV vfsRead(retro_vfs_file_handle* stream, void* out, uint64_t length) {
  if (!stream || (!out && length != 0)) return -1;
  if (length == 0) return 0;
  const size_t amount = static_cast<size_t>(std::min<uint64_t>(length, INT_MAX));
  const int result = stream->file.read(out, amount);
  return result < 0 ? -1 : static_cast<int64_t>(result);
}

// The core's game SRAM is persisted by GbaActivity in the same directory as
// the selected ROM. Unsupported generic VFS writes fail clearly; ROM reads
// are the only file-stream operation required by this frontend.
int64_t RETRO_CALLCONV vfsWrite(retro_vfs_file_handle*, const void*, uint64_t) { return -1; }
int RETRO_CALLCONV vfsFlush(retro_vfs_file_handle*) { return 0; }
int RETRO_CALLCONV vfsRemove(const char*) { return -1; }
int RETRO_CALLCONV vfsRename(const char*, const char*) { return -1; }
int64_t RETRO_CALLCONV vfsTruncate(retro_vfs_file_handle*, int64_t) { return -1; }

retro_vfs_interface sVfs = {
    vfsGetPath, vfsOpen, vfsClose, vfsSize, vfsTell, vfsSeek, vfsRead, vfsWrite,
    vfsFlush, vfsRemove, vfsRename, vfsTruncate,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

bool RETRO_CALLCONV environmentCallback(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
      auto* info = static_cast<retro_vfs_interface_info*>(data);
      if (!info || info->required_interface_version > 2) return false;
      info->iface = &sVfs;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      if (!data) return false;
      const auto* format = static_cast<const retro_pixel_format*>(data);
      return *format == RETRO_PIXEL_FORMAT_RGB565;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      if (data) *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      return false;
    default:
      // Core options, messages, rumble and other optional libretro services
      // are intentionally absent. The core has defaults and the device has
      // no audio output in this activity.
      return false;
  }
}

void RETRO_CALLCONV videoCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || !sVideoCallback) return;
  sVideoCallback(sVideoUser, static_cast<const uint16_t*>(data), width, height, pitch);
}

void RETRO_CALLCONV audioSampleCallback(int16_t, int16_t) {}

size_t RETRO_CALLCONV audioBatchCallback(const int16_t*, size_t frames) {
  // Tell gpSP that all audio samples were consumed. Ink-boy's monochrome
  // activity deliberately has no sound output path, but timing stays correct.
  return frames;
}

void RETRO_CALLCONV inputPollCallback() {}

int16_t RETRO_CALLCONV inputStateCallback(unsigned port, unsigned device, unsigned, unsigned id) {
  if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16) return 0;
  return (sJoypadMask & (static_cast<uint16_t>(1u) << id)) ? 1 : 0;
}

void setError(const char* message) {
  std::strncpy(sLastError, message ? message : "gpSP failed", sizeof(sLastError) - 1);
  sLastError[sizeof(sLastError) - 1] = '\0';
}

bool allocateCoreBuffers() {
  if (gpsp_alloc_cpu_buffers() &&
      gpsp_alloc_backup_buffer() &&
      gpsp_alloc_video_buffers() &&
      gpsp_alloc_sound_buffer()) {
    sCoreBuffersAllocated = true;
    return true;
  }

  gpsp_free_sound_buffer();
  gpsp_free_video_buffers();
  gpsp_free_backup_buffer();
  gpsp_free_cpu_buffers();

  return false;
}

void freeCoreBuffers() {
  if (!sCoreBuffersAllocated) return;

  gpsp_free_sound_buffer();
  gpsp_free_video_buffers();
  gpsp_free_backup_buffer();
  gpsp_free_cpu_buffers();

  sCoreBuffersAllocated = false;
}

}  // namespace

namespace gpsp {

bool initialize(const char* romPath, VideoCallback video, void* user) {
  shutdown();
  sLastError[0] = '\0';
  if (!romPath || !*romPath) {
    setError("No GBA ROM path supplied");
    return false;
  }

  sVideoCallback = video;
  sVideoUser = user;
  sJoypadMask = 0;
  if (!allocateCoreBuffers()) {
  setError("Not enough PSRAM for GBA core buffers");
  sVideoCallback = nullptr;
  sVideoUser = nullptr;
  return false;
}

  retro_set_environment(environmentCallback);
  retro_set_video_refresh(videoCallback);
  retro_set_audio_sample(audioSampleCallback);
  retro_set_audio_sample_batch(audioBatchCallback);
  retro_set_input_poll(inputPollCallback);
  retro_set_input_state(inputStateCallback);
  retro_init();
  sCoreInitialized = true;

  retro_game_info info{};
  info.path = romPath;
  if (!retro_load_game(&info)) {
    setError("gpSP could not load this GBA ROM");
    retro_deinit();
    sCoreInitialized = false;
    sVideoCallback = nullptr;
    sVideoUser = nullptr;
    return false;
  }
  sGameLoaded = true;
  return true;
}

void runFrame(const uint16_t joypadMask) {
  if (!sCoreInitialized || !sGameLoaded) return;
  sJoypadMask = joypadMask;
  retro_run();
}

uint8_t* batteryRamData() {
  return static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
}

size_t batteryRamSize() {
  if (!sCoreInitialized || !sGameLoaded) return 0;
  uint32_t type = (backup_type == BACKUP_UNKN_VALUE) ? backup_type_reset : backup_type;
  switch (type) {
    case BACKUP_SRAM_VALUE:
      // gpSP's cartridge SRAM is 32 KiB; this source snapshot declares an
      // sram_bankcount extern but does not define it, so don't depend on it.
      return 32u * 1024u;
    case BACKUP_FLASH_VALUE:
      return (flash_bank_cnt == FLASH_SIZE_128KB_VALUE) ? 128u * 1024u : 64u * 1024u;
    case BACKUP_EEPROM_VALUE:
      return (eeprom_size == EEPROM_8_KBYTE_VALUE) ? 8u * 1024u : 512u;
    default:
      return 0;
  }
}

const char* lastError() { return sLastError; }
bool isLoaded() { return sCoreInitialized && sGameLoaded; }

void shutdown() {
  if (sGameLoaded) {
    retro_unload_game();
    sGameLoaded = false;
  }
  if (sCoreInitialized) {
    retro_deinit();
    sCoreInitialized = false;
  }
  sJoypadMask = 0;
  sVideoCallback = nullptr;
  sVideoUser = nullptr;
}

}  // namespace gpsp
