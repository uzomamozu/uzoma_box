#include "Config.h"
#include "SDManager.h"
#include <NativeEthernet.h>

// ---------------------------------------------------------------------------
// Default values
// ---------------------------------------------------------------------------
void configSetDefaults(AppConfig &cfg)
{
  cfg.mode            = MODE_ARTNET;
  cfg.language        = 0;   // English by default
  cfg.ip              = IPAddress(192, 168, 0, 211);
  cfg.mac[0]          = 0xDE; cfg.mac[1] = 0xAD;
  cfg.mac[2]          = 0xBE; cfg.mac[3] = 0xEF;
  cfg.mac[4]          = 0xBE; cfg.mac[5] = 0xED;
  cfg.ledWidth        = 512;
  cfg.colorOrder      = ORDER_RGB;
  cfg.playbackSpeed   = 1.0f;
  cfg.recordFps       = 30;
  strncpy(cfg.nickname, "UzomaBox", sizeof(cfg.nickname));
  cfg.nickname[sizeof(cfg.nickname) - 1] = 0;

  // Default start universes per strip: 0, 3, 6...
  for (int i = 0; i < MAX_OUTPUTS; i++) {
    cfg.startUniverse[i] = i * 3;
    cfg.outputActive[i]  = (i < ACTIVE_OUTPUTS);
  }
}

// ---------------------------------------------------------------------------
// Parse a mode string ("artnet", "playback", "record") into enum
// ---------------------------------------------------------------------------
static OperatingMode parseMode(const char *str)
{
  if (!strcmp(str, "playback")) return MODE_PLAYBACK;
  if (!strcmp(str, "record"))   return MODE_RECORD;
  if (!strcmp(str, "test"))     return MODE_TEST;
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
    case MODE_TEST:     return "test";
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
      while (tok && idx < MAX_OUTPUTS) {
        cfg.startUniverse[idx++] = (uint16_t)atoi(tok);
        tok = strtok(NULL, ",");
      }
    }
    else if (!strcmp(key, "output_active")) {
      int idx = 0;
      char *tok = strtok(value, ",");
      while (tok && idx < MAX_OUTPUTS) {
        cfg.outputActive[idx++] = (atoi(tok) != 0);
        tok = strtok(NULL, ",");
      }
    }
    else if (!strcmp(key, "color_order")) {
      cfg.colorOrder = parseColorOrder(value);
    }
    else if (!strcmp(key, "playback_speed")) {
      float spd = atof(value);
      if (spd >= 0.05f && spd <= 5.0f) {
        cfg.playbackSpeed = spd;
      }
    }
    else if (!strcmp(key, "record_fps")) {
      int fps = atoi(value);
      if (fps >= 5 && fps <= 60) {
        cfg.recordFps = (uint16_t)fps;
      }
    }
    else if (!strcmp(key, "nickname")) {
      strncpy(cfg.nickname, value, sizeof(cfg.nickname) - 1);
      cfg.nickname[sizeof(cfg.nickname) - 1] = 0;
    }
    else if (!strcmp(key, "language")) {
      if (!strcmp(value, "es")) cfg.language = 1;
      else                      cfg.language = 0;
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

  // Start universes — snprintf with position tracking (bounds-safe)
  int pos = 0;
  for (int i = 0; i < MAX_OUTPUTS; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%u",
                    i > 0 ? "," : "", cfg.startUniverse[i]);
  }
  sdFilePrintf("start_universe=%s\n", buf);

  // Output active
  pos = 0;
  for (int i = 0; i < MAX_OUTPUTS; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s",
                    i > 0 ? "," : "", cfg.outputActive[i] ? "1" : "0");
  }
  sdFilePrintf("output_active=%s\n", buf);

  sdFilePrintf("color_order=%s\n", colorOrderStr(cfg.colorOrder));
  sdFilePrintf("playback_speed=%.2f\n", cfg.playbackSpeed);
  sdFilePrintf("record_fps=%u\n", cfg.recordFps);
  sdFilePrintf("nickname=%s\n", cfg.nickname);
  sdFilePrintf("language=%s\n", cfg.language ? "es" : "en");

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