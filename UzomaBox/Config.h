#ifndef Config_h
#define Config_h

#include <Arduino.h>
#include <NativeEthernet.h>

#include "Pins.h"
#include "LEDController.h"

#define CONFIG_FILENAME "CONFIG.TXT"
#define MODEL_STRING    "UzomaBox T4.1"
#define FW_VERSION      "1.0.0"

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
  uint16_t startUniverse[MAX_OUTPUTS];
  bool outputActive[MAX_OUTPUTS];
  ColorOrder colorOrder;
  float playbackSpeed;                     // 0.05 – 5.0
  uint16_t recordFps;                      // Recording FPS (5-60)
  char nickname[32];                       // User-assignable device name
  uint8_t language;                        // 0 = English, 1 = Español
};

bool loadConfig(AppConfig &cfg);
bool saveConfig(const AppConfig &cfg);
void applyConfig(const AppConfig &cfg);
void configSetDefaults(AppConfig &cfg);

#endif