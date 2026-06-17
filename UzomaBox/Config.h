#ifndef Config_h
#define Config_h

#include <Arduino.h>
#include <NativeEthernet.h>

#define CONFIG_FILENAME "CONFIG.TXT"

enum OperatingMode {
  MODE_ARTNET = 0,
  MODE_PLAYBACK,
  MODE_RECORD
};

struct AppConfig {
  OperatingMode mode;
  IPAddress ip;
  uint8_t mac[6];
  uint16_t ledWidth;
  uint16_t startUniverse[8];
  bool outputActive[8];
};

bool loadConfig(AppConfig &cfg);
bool saveConfig(const AppConfig &cfg);
void applyConfig(const AppConfig &cfg);
void configSetDefaults(AppConfig &cfg);

#endif