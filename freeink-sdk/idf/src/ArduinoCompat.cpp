// FreeInk SDK — ESP-IDF backing for idf/include/Arduino.h and SPI.h.
// Only the ESP-IDF component builds this file (see CMakeLists.txt at the repo root).

#include <Arduino.h>
#include <SPI.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <stdarg.h>

static const char* TAG = "freeink";

HardwareSerial Serial, Serial0;
SPIClass SPI;

int HardwareSerial::printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

void pinMode(uint8_t pin, uint8_t mode) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  // Outputs keep their input enabled so digitalRead() reads back the level, as on Arduino.
  cfg.mode = (mode & OUTPUT) == OUTPUT ? ((mode & OPEN_DRAIN) ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT)
                                       : GPIO_MODE_INPUT;
  cfg.pull_up_en = (mode & PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = (mode & PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

static void IRAM_ATTR isrTrampoline(void* arg) { reinterpret_cast<void (*)(void)>(arg)(); }

void attachInterrupt(uint8_t pin, void (*isr)(void), int mode) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  gpio_set_intr_type(gpio, mode == RISING    ? GPIO_INTR_POSEDGE
                           : mode == FALLING ? GPIO_INTR_NEGEDGE
                                             : GPIO_INTR_ANYEDGE);
  // Adding replaces a handler the pin had. The firmware may have installed
  // the GPIO ISR service already; install it only when adding says it's missing.
  esp_err_t err = gpio_isr_handler_add(gpio, isrTrampoline, reinterpret_cast<void*>(isr));
  if (err == ESP_ERR_INVALID_STATE) {
    err = gpio_install_isr_service(0);
    if (err == ESP_OK) err = gpio_isr_handler_add(gpio, isrTrampoline, reinterpret_cast<void*>(isr));
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "attachInterrupt(%d): %s", pin, esp_err_to_name(err));
    return;
  }
  gpio_intr_enable(gpio);
}

void detachInterrupt(uint8_t pin) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  gpio_intr_disable(gpio);
  gpio_isr_handler_remove(gpio);
  gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
}

// ── SPI ──────────────────────────────────────────────────────────────────────

static constexpr spi_host_device_t kHost = SPI2_HOST;
static constexpr size_t kChunk = 4092;  // one DMA descriptor's worth
static uint8_t* s_dma;                  // bounce buffer: callers pass PSRAM/flash data

void SPIClass::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t) {
  if (!s_dma) s_dma = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_DMA));
  spi_bus_config_t bus = {};
  bus.sclk_io_num = sck;
  bus.miso_io_num = miso;
  bus.mosi_io_num = mosi;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kChunk;
  const esp_err_t err = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
  _ownsBus = err == ESP_OK;
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: firmware owns the bus
    ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
  }
}

void SPIClass::end() {
  if (_dev) spi_bus_remove_device(_dev);
  _dev = nullptr;
  if (_ownsBus) spi_bus_free(kHost);
  _ownsBus = false;
}

void SPIClass::beginTransaction(const SPISettings& s) {
  if (_dev && (s.clock != _settings.clock || s.dataMode != _settings.dataMode)) {
    spi_bus_remove_device(_dev);
    _dev = nullptr;
  }
  if (!_dev) {
    spi_device_interface_config_t dev = {};
    dev.mode = s.dataMode;
    dev.clock_speed_hz = static_cast<int>(s.clock);
    dev.spics_io_num = -1;  // the drivers drive CS themselves
    dev.queue_size = 1;
    dev.flags = s.bitOrder == LSBFIRST ? SPI_DEVICE_BIT_LSBFIRST : 0;
    const esp_err_t err = spi_bus_add_device(kHost, &dev, &_dev);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
      _dev = nullptr;
      return;
    }
    _settings = s;
  }
  spi_device_acquire_bus(_dev, portMAX_DELAY);  // keeps other devices (SD) off the bus
}

void SPIClass::endTransaction() {
  if (_dev) spi_device_release_bus(_dev);
}

uint8_t SPIClass::transfer(uint8_t data) {
  if (!_dev) return 0;
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
  t.length = 8;
  t.tx_data[0] = data;
  spi_device_polling_transmit(_dev, &t);
  return t.rx_data[0];
}

void SPIClass::writeBytes(const uint8_t* data, uint32_t size) {
  if (!_dev || !s_dma) return;
  while (size) {
    const size_t n = size < kChunk ? size : kChunk;
    memcpy(s_dma, data, n);
    spi_transaction_t t = {};
    t.length = n * 8;
    t.tx_buffer = s_dma;
    spi_device_polling_transmit(_dev, &t);
    data += n;
    size -= n;
  }
}
