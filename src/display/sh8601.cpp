// SH8601 AMOLED driver for the Waveshare ESP32-S3-Touch-AMOLED-1.8 board.
// Implements the same entry points as axs15231b.cpp so DisplayManager is shared.
// Only compiled in the `amoled` PlatformIO env (see build_src_filter).
#include "display/axs15231b.h"

#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_log.h>

#include "board/BoardConfig.h"

namespace {

// 80 MHz is the SH8601's rated QSPI ceiling and halves the ~16.5 ms it takes to
// push a full 368x448 frame. If the panel ever shows tearing, shimmer, or
// dropped pixel rows, drop this back to 40000000 first.
constexpr int kSpiFrequency = 80000000;
constexpr int kChunkPixels = 0x4000;  // 16384 px per DMA chunk
static const char *kTag = "sh8601";

constexpr uint8_t kExpanderAddr = 0x20;  // XCA9554 drives LCD reset/power

spi_device_handle_t gSpi = nullptr;
bool gBusReady = false;
uint8_t gBrightnessPercent = 100;
bool gPanelOn = false;

inline void csLow() { digitalWrite(BoardConfig::PIN_LCD_CS, LOW); }
inline void csHigh() { digitalWrite(BoardConfig::PIN_LCD_CS, HIGH); }

// XCA9554/TCA9554: output reg 0x01, config reg 0x03 (0 = output).
void expanderWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kExpanderAddr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void resetPanelViaExpander() {
  Wire.setClock(100000);
  Wire.beginTransmission(kExpanderAddr);
  const bool present = (Wire.endTransmission() == 0);
  ESP_LOGI(kTag, "XCA9554 expander present=%d", present);
  expanderWrite(0x03, 0xF8);  // pins 0,1,2 outputs
  expanderWrite(0x01, 0x00);  // assert (low)
  delay(30);
  expanderWrite(0x01, 0x07);  // release (high)
  delay(50);
}

// Command phase uses the QSPI 0x02 opcode with the DCS command in the address.
void sendCommand(uint8_t command, const uint8_t *data, uint32_t length) {
  if (gSpi == nullptr) {
    return;
  }
  csLow();
  spi_transaction_t transaction = {};
  transaction.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  transaction.cmd = 0x02;
  transaction.addr = static_cast<uint32_t>(command) << 8;
  if (length != 0) {
    transaction.tx_buffer = data;
    transaction.length = length * 8;
  }
  ESP_ERROR_CHECK(spi_device_polling_transmit(gSpi, &transaction));
  csHigh();
}

void setAddressWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  const uint16_t x1 = x + w - 1;
  const uint16_t y1 = y + h - 1;
  const uint8_t caset[] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                           static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)};
  sendCommand(0x2A, caset, sizeof(caset));
  const uint8_t paset[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                           static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)};
  sendCommand(0x2B, paset, sizeof(paset));
}

void applyBrightness() {
  if (gSpi == nullptr) {
    return;
  }
  uint8_t value = static_cast<uint8_t>((static_cast<uint16_t>(gBrightnessPercent) * 255U) / 100U);
  sendCommand(0x51, &value, 1);
}

}  // namespace

void axs15231bInit() {
  resetPanelViaExpander();

  pinMode(BoardConfig::PIN_LCD_CS, OUTPUT);
  csHigh();

  if (!gBusReady) {
    spi_bus_config_t busConfig = {};
    busConfig.data0_io_num = BoardConfig::PIN_LCD_DATA0;
    busConfig.data1_io_num = BoardConfig::PIN_LCD_DATA1;
    busConfig.data2_io_num = BoardConfig::PIN_LCD_DATA2;
    busConfig.data3_io_num = BoardConfig::PIN_LCD_DATA3;
    busConfig.sclk_io_num = BoardConfig::PIN_LCD_SCLK;
    busConfig.max_transfer_sz = (kChunkPixels * static_cast<int>(sizeof(uint16_t))) + 8;
    busConfig.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    spi_device_interface_config_t deviceConfig = {};
    deviceConfig.command_bits = 8;
    deviceConfig.address_bits = 24;
    deviceConfig.mode = 0;  // SH8601 = SPI_MODE0
    deviceConfig.clock_speed_hz = kSpiFrequency;
    deviceConfig.spics_io_num = -1;  // CS driven manually so it stays low across pixel bursts
    deviceConfig.flags = SPI_DEVICE_HALFDUPLEX;
    deviceConfig.queue_size = 10;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &busConfig, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &deviceConfig, &gSpi));
    gBusReady = true;
  }

  sendCommand(0x01, nullptr, 0);  // SWRESET (reset line is behind the expander)
  delay(200);
  sendCommand(0x11, nullptr, 0);  // SLPOUT
  delay(120);
  sendCommand(0x13, nullptr, 0);  // NORON
  sendCommand(0x20, nullptr, 0);  // INVOFF
  uint8_t pixfmt = 0x05;
  sendCommand(0x3A, &pixfmt, 1);  // 16bit/pixel RGB565
  sendCommand(0x29, nullptr, 0);  // DISPON
  uint8_t ctrld1 = 0x28;
  sendCommand(0x53, &ctrld1, 1);  // brightness control + dimming on
  uint8_t wce = 0x00;
  sendCommand(0x58, &wce, 1);  // contrast enhancement off
  gPanelOn = true;
  applyBrightness();
  delay(10);
  ESP_LOGI(kTag, "SH8601 init complete");
}

void axs15231bSetBacklight(bool on) {
  // AMOLED has no backlight; map to display on/off.
  if (gSpi == nullptr) {
    return;
  }
  if (on) {
    sendCommand(0x29, nullptr, 0);
    applyBrightness();
  } else {
    sendCommand(0x28, nullptr, 0);
  }
  gPanelOn = on;
}

void axs15231bSetBrightnessPercent(uint8_t percent) {
  if (percent == 0) {
    percent = 1;
  } else if (percent > 100) {
    percent = 100;
  }
  gBrightnessPercent = percent;
  applyBrightness();
}

void axs15231bSleep() {
  if (gSpi == nullptr) {
    return;
  }
  sendCommand(0x28, nullptr, 0);  // DISPOFF
  delay(20);
  sendCommand(0x10, nullptr, 0);  // SLPIN
  delay(120);
  gPanelOn = false;
}

void axs15231bWake() {
  if (gSpi == nullptr) {
    return;
  }
  sendCommand(0x11, nullptr, 0);  // SLPOUT
  delay(120);
  sendCommand(0x29, nullptr, 0);  // DISPON
  gPanelOn = true;
  applyBrightness();
}

void axs15231bPushColors(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                         const uint16_t *data) {
  if (gSpi == nullptr || data == nullptr || width == 0 || height == 0) {
    return;
  }

  setAddressWindow(x, y, width, height);
  sendCommand(0x2C, nullptr, 0);  // RAMWR: begin memory write

  size_t pixelsRemaining = static_cast<size_t>(width) * height;
  const uint16_t *cursor = data;
  bool firstSend = true;

  csLow();  // hold CS low for the entire pixel burst
  while (pixelsRemaining > 0) {
    size_t chunkPixels = pixelsRemaining > static_cast<size_t>(kChunkPixels)
                             ? static_cast<size_t>(kChunkPixels)
                             : pixelsRemaining;
    spi_transaction_ext_t transaction = {};
    if (firstSend) {
      transaction.base.flags = SPI_TRANS_MODE_QIO;
      transaction.base.cmd = 0x32;
      transaction.base.addr = 0x003C00;  // WRMC continue (matches Waveshare driver)
      firstSend = false;
    } else {
      transaction.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                               SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      transaction.command_bits = 0;
      transaction.address_bits = 0;
      transaction.dummy_bits = 0;
    }
    transaction.base.tx_buffer = cursor;
    transaction.base.length = chunkPixels * 16;
    ESP_ERROR_CHECK(
        spi_device_polling_transmit(gSpi, reinterpret_cast<spi_transaction_t *>(&transaction)));
    pixelsRemaining -= chunkPixels;
    cursor += chunkPixels;
  }
  csHigh();
}
