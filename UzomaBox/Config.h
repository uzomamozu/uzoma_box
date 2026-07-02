#ifndef Config_h
#define Config_h

#include <Arduino.h>
#include <NativeEthernet.h>

#include "LEDController.h"

#define CONFIG_FILENAME "CONFIG.TXT"
#define MODEL_STRING    "UzomaBox T4.1"
#define FW_VERSION      "1.0.0"

// ---------------------------------------------------------------------------
// Number of LED output channels.
// 16 → dual OctoWS2811 (first 8 via OctoWS2811 instance #1 on default pins,
//       next 8 via OctoWS2811 instance #2 on alternate pins)
// ---------------------------------------------------------------------------
#define NUM_OUTPUTS  16

// ---------------------------------------------------------------------------
// SSD1306 OLED display (128×64, I2C)
// ---------------------------------------------------------------------------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET     -1       // no reset pin on most SSD1306 modules

// ---------------------------------------------------------------------------
// Button pins (active LOW with internal pull-up)
// ---------------------------------------------------------------------------
#define PIN_BTN_UP      22
#define PIN_BTN_DOWN    23
#define PIN_BTN_SELECT   9
#define PIN_BTN_BACK    33

enum OperatingMode {
  MODE_ARTNET = 0,
  MODE_PLAYBACK,
  MODE_RECORD,
  MODE_TEST
};

struct AppConfig {
  OperatingMode mode;
  IPAddress ip;
  uint8_t mac[6];
  uint16_t ledWidth;
  uint16_t startUniverse[NUM_OUTPUTS];
  bool outputActive[NUM_OUTPUTS];
  ColorOrder colorOrder;
  float playbackSpeed;                     // 0.05 – 5.0
  uint16_t recordFps;                      // Recording FPS (5-60)
  char nickname[32];                       // User-assignable device name
};

bool loadConfig(AppConfig &cfg);
bool saveConfig(const AppConfig &cfg);
void applyConfig(const AppConfig &cfg);
void configSetDefaults(AppConfig &cfg);

#endif