#include "Config.h"
#include "SDManager.h"
#include <NativeEthernet.h>

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------
void configSetDefaults(AppConfig &cfg)
{
  cfg.mode            = MODE_ARTNET;
  cfg.ip              = IPAddress(192, 168, 0, 211);
  cfg.mac[0]          = 0xDE; cfg.mac[1] = 0xAD;
  cfg.mac[2]          = 0xBE; cfg.mac[3] = 0xEF;
  cfg.mac[4]          = 0xBE; cfg.mac[5] = 0xED;
  cfg.ledWidth        = 512;
  cfg.colorOrder      = ORDER_RGB;
  cfg.playbackSpeed   = 1.0f;

  // Default start universes per strip: 0, 3, 6, 9, 12, 15, 18, 21
  for (int i = 0; i < 8; i++) {
    cfg.startUniverse[i] = i * 3;
    cfg.outputActive[i]  = true;
  }
}

// ---------------------------------------------------------------------------
// Parse a mode string ("artnet", "playback", "record") into enum
// ---------------------------------------------------------------------------
static OperatingMode parseMode(const char *str)
{
  if (!strcmp(str, "playback")) return MODE_PLAYBACK;
  if (!strcmp(str, "record"))   return MODE_RECORD;
  return MODE_ARTNET;
}

// ---------------------------------------------------------------------------
// Mode to string
// ---------------------------------------------------------------------------
static const char *modeStr(OperatingMode m)
{
  switch (m) {
    case MODE_ARTNET:   return "artnet";
    case MODE_PLAYBACK: return "playback";
    case MODE_RECORD:   return "record";
    default:            return "artnet";
  }
}

// ---------------------------------------------------------------------------
// Load configuration from SD card
// ---------------------------------------------------------------------------
bool loadConfig(AppConfig &cfg)
{
  configSetDefaults(cfg);                         // start with defaults

  if (!sdFileOpen(CONFIG_FILENAME, FILE_READ)) {
    // File doesn't exist – create it with defaults
    saveConfig(cfg);
    return true;
  }

  char line[128];
  while (sdFileReadLine(line, sizeof(line))) {
    // Trim trailing whitespace / CR / LF
    char *p = line + strlen(line);
    while (p > line && (p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ')) *--p = 0;
    if (line[0] == '#' || line[0] == 0) continue;   // skip comments & empty

    char *key   = strtok(line, "=");
    char *value = strtok(NULL, "");
    if (!key || !value) continue;

    // Remove leading spaces from value
    while (*value == ' ') value++;

    if      (!strcmp(key, "mode")) {
      cfg.mode = parseMode(value);
    }
    else if (!strcmp(key, "ip")) {
      cfg.ip.fromString(value);
    }
    else if (!strcmp(key, "mac")) {
      uint8_t m[6];
      if (sscanf(value, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                 &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        memcpy(cfg.mac, m, 6);
      }
    }
    else if (!strcmp(key, "led_width")) {
      int w = atoi(value);
      if (w > 0) cfg.ledWidth = (uint16_t)w;
    }
    else if (!strcmp(key, "start_universe")) {
      int idx = 0;
      char *tok = strtok(value, ",");
      while (tok && idx < 8) {
        cfg.startUniverse[idx++] = (uint16_t)atoi(tok);
        tok = strtok(NULL, ",");
      }
    }
    else if (!strcmp(key, "output_active")) {
      int idx = 0;
      char *tok = strtok(value, ",");
      while (tok && idx < 8) {
        cfg.outputActive[idx++] = (atoi(tok) != 0);
        tok = strtok(NULL, ",");
      }
    }
    else if (!strcmp(key, "color_order")) {
      cfg.colorOrder = parseColorOrder(value);
    }
    else if (!strcmp(key, "playback_speed")) {
      float spd = atof(value);
      if (spd >= 0.05f && spd <= 5.0f) cfg.playbackSpeed = spd;
    }
  }

  sdFileClose();
  return true;
}

// ---------------------------------------------------------------------------
// Save configuration to SD card
// ---------------------------------------------------------------------------
bool saveConfig(const AppConfig &cfg)
{
  // O_WRITE | O_CREAT | O_TRUNC = overwrite from beginning
  if (!sdFileOpen(CONFIG_FILENAME, O_WRITE | O_CREAT | O_TRUNC)) {
    return false;
  }

  char buf[256];

  sdFilePrintf("mode=%s\n", modeStr(cfg.mode));
  sdFilePrintf("ip=%d.%d.%d.%d\n", cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
  sdFilePrintf("mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
               cfg.mac[0], cfg.mac[1], cfg.mac[2],
               cfg.mac[3], cfg.mac[4], cfg.mac[5]);
  sdFilePrintf("led_width=%u\n", cfg.ledWidth);

  // Start universes
  buf[0] = 0;
  for (int i = 0; i < 8; i++) {
    if (i > 0) strcat(buf, ",");
    char tmp[8]; sprintf(tmp, "%u", cfg.startUniverse[i]);
    strcat(buf, tmp);
  }
  sdFilePrintf("start_universe=%s\n", buf);

  // Output active
  buf[0] = 0;
  for (int i = 0; i < 8; i++) {
    if (i > 0) strcat(buf, ",");
    strcat(buf, cfg.outputActive[i] ? "1" : "0");
  }
  sdFilePrintf("output_active=%s\n", buf);

  sdFilePrintf("color_order=%s\n", colorOrderStr(cfg.colorOrder));
  sdFilePrintf("playback_speed=%.2f\n", cfg.playbackSpeed);

  sdFileClose();
  return true;
}

// ---------------------------------------------------------------------------
// Apply config to hardware (Ethernet, etc.)
// ---------------------------------------------------------------------------
void applyConfig(const AppConfig &cfg)
{
  // Ethernet.begin(mac, ip) is called in main setup()
  // Here we just validate / prepare – actual Ethernet init happens in UzomaBox.ino
  (void)cfg;
}