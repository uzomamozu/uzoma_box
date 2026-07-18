/*  UzomaBox.ino
 *  Teensy 4.1 multi-mode LED controller (16-output branch)
 *
 *  Hardware:
 *    Teensy 4.1 + dual OctoWS2811
 *    16 × WS2811 LED strips (default 512 LEDs each)
 *    74HCT245 buffer + 100Ω resistors on each LED output pin
 *    SD card on built-in microSD slot (BUILTIN_SDCARD)
 *    NativeEthernet for ArtNet + TCP
 *    OLED display (I2C) + 4 buttons
 *
 *  ╔══════════════════════════════════════════════════════════════╗
 *  ║  PIN ASSIGNMENTS                                           ║
 *  ║  ¡Edita Pins.h si cambia el cableado!                     ║
 *  ╚══════════════════════════════════════════════════════════════╝
 *  NOTE: Pin 4 is ENET_RST (Ethernet PHY reset) — output #5 pin 8!
 *  LED outputs 1- 8:   0,  1,  2,  3,  8,  5,  6,  7
 *  LED outputs 9-16:   9, 10, 11, 12, 24, 25, 28, 29
 *  Buttons:   RED=23  UP=22  DOWN=21  SELECT=20  BACK=19
 *  OLED (I2C):    SDA=18  SCL=17  ADDR=0x3C
 */

#include "Pins.h"

// ledPins array — the actual pin numbers for 16 OctoWS2811 outputs.
// Change these values if you rewire the LED strips.
// NOTE: Pin 4 is ENET_RST (Ethernet PHY reset), so output #5 moved to pin 8.
const uint8_t ledPins[16] = {0,  1,  2,  3,  8,  5,  6,  7,
                             9,  10, 11, 12, 24, 25, 28, 29};

#include "Config.h"
#include "SDManager.h"
#include "LEDController.h"
#include "ArtNetHandler.h"
#include "TCPHandler.h"
#include "PlaybackController.h"
#include "UdpDiscovery.h"
#include "MenuManager.h"

// ========================  WATCHDOG  =========================================
// Watchdog functionality is now in Watchdog.h / Watchdog.cpp.
// See that file for configuration and feeding.
#include "Watchdog.h"

// ========================  GLOBAL OBJECTS  ================================

AppConfig          g_config;
LEDController      g_leds;
ArtNetHandler      g_artNet;
TCPHandler         g_tcp;
PlaybackController g_playback;
UdpDiscovery       g_discovery;
MenuManager        g_menu;

// Current operating mode
OperatingMode      g_mode = MODE_ARTNET;

// Recording trigger configuration (active even in ArtNet mode)
uint8_t            g_recStartMode    = 0;  // 0=immediate, 1=first non-zero, 2=channel change
uint8_t            g_recStopMode     = 0;  // 0=immediate, 1=all zero, 2=timer
uint16_t           g_recTrigUniv     = 0;
uint16_t           g_recTrigCh       = 0;
uint32_t           g_recStopSecs     = 0;  // seconds for timer stop
bool               g_recArmed        = false;
uint8_t            g_recLastTrigVal  = 0;
uint32_t           g_recStopStart    = 0;

// Frame timing for ArtNet mode
uint32_t           g_lastArtNetFrame = 0;
uint32_t           g_frameCounter    = 0;

// Test mode state
uint32_t           g_testStartMs     = 0;
uint8_t            g_testPattern     = 0;  // 0=RGBW cycle, 1=rainbow fade, 2=red, 3=green, 4=blue
uint8_t            g_testOutput      = 255; // 255=all, 0-15=specific strip

// Non-blocking IDENTIFY blink state
bool               g_identifyActive     = false;
uint8_t            g_identifyBlinkCount = 0;
uint32_t           g_identifyLastToggle = 0;
bool               g_identifyLedState   = false;

// Throttle for UDP discovery broadcasts (only send every 5s)
uint32_t           g_lastDiscoveryPoll  = 0;

// Incoming ArtNet FPS meter
uint32_t           g_fpsFrames = 0;
uint32_t           g_fpsDisplay = 0;   // stable FPS value (updated each second)
uint32_t           g_fpsLastPrint = 0;

// Playback temporary buffer (max 16 strips × 512 LEDs × 3 bytes = 24576)
// Shared between playback and test mode to avoid stack overflow
DMAMEM static uint8_t g_playbackBuffer[512 * 16 * 3];

// Frame dedup guard: micros() of last recorded frame write.
// ArtNet double-fire (timeout + _allUpdated) can write the same frame twice.
// This guard skips writes that arrive within 15ms of the previous one,
// preventing inflated files that cause playback speed issues.
static uint32_t g_lastRecordUs = 0;

// ========================  FORWARD DECLARATIONS  ==========================

void onArtNetFrame(const uint8_t *rgbData, uint16_t totalPixels);
void handleTcpCommand(int cmd, const char *cmdStr);
void runTestAnimation();
void rebootTeensy();
void printStatus();
void setMode(OperatingMode newMode);

// ========================  SETUP  =========================================

void setup()
{
  // Enable hardware watchdog early (8 second timeout)
  watchdog_enable();

  Serial.begin(115200);
  delay(100);
  watchdog_feed();  // reset WDT counter right after enabling + Serial init
  Serial.println("\n=== UzomaBox (16 outputs) ===");

  // ========================  DIAGNOSTIC: BYPASS SD  ========================
  #ifdef DIAG_NO_SD
    Serial.println("DIAG: NO_SD mode – skipping SD init, using default config");
    // Hardcoded default config (no SD needed)
    g_config.ip = IPAddress(192, 168, 1, 100);
    g_config.mac[0] = 0x04; g_config.mac[1] = 0xE9;
    g_config.mac[2] = 0xE5; g_config.mac[3] = 0x00;
    g_config.mac[4] = 0x00; g_config.mac[5] = 0x01;
    g_config.ledWidth = 512;
    g_config.colorOrder = ORDER_RGB;
    g_config.mode = MODE_ARTNET;
    g_config.recordFps = 30;
    g_config.playbackSpeed = 1.0f;
    strcpy(g_config.nickname, "UzomaBox");
    for (int i = 0; i < MAX_OUTPUTS; i++) {
      g_config.startUniverse[i] = 0;
      g_config.outputActive[i] = (i < ACTIVE_OUTPUTS);
    }
    Serial.print("Output active (default): ");
    for (int i = 0; i < ACTIVE_OUTPUTS; i++) {
      Serial.print("1");
      if (i < ACTIVE_OUTPUTS - 1) Serial.print(",");
    }
    Serial.println();
    Serial.print("Mode: ");
    Serial.println("ArtNet");
    Serial.print("IP: ");   Serial.println(g_config.ip);
    Serial.print("MAC: ");  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                              g_config.mac[0], g_config.mac[1], g_config.mac[2],
                              g_config.mac[3], g_config.mac[4], g_config.mac[5]);
    Serial.print("LEDs/strip: "); Serial.println(g_config.ledWidth);
  #else
    // ---- Initialise SD card (Teensy 4.1 built-in microSD slot) ------------
    if (!sdInit()) {
      Serial.println("FATAL: No SD card found");
      while (1) {
        watchdog_feed();
        delay(1000);
      }
    }
    Serial.println("SD card OK");

    // ---- Load / create config ---------------------------------------------
    loadConfig(g_config);
    Serial.print("Output active loaded: ");
  #endif
  // Continuation of serial prints regardless of DIAG_NO_SD
  #ifndef DIAG_NO_SD
    for (int i = 0; i < ACTIVE_OUTPUTS; i++) {
      Serial.print(g_config.outputActive[i] ? "1" : "0");
      if (i < ACTIVE_OUTPUTS - 1) Serial.print(",");
    }
    Serial.println();
    Serial.print("Mode: ");
    switch (g_config.mode) {
      case MODE_ARTNET:   Serial.println("ArtNet");  break;
      case MODE_PLAYBACK: Serial.println("Playback"); break;
      case MODE_RECORD:   Serial.println("Record");   break;
      case MODE_TEST:     Serial.println("Test");     break;
    }
    Serial.print("IP: ");   Serial.println(g_config.ip);
    Serial.print("MAC: ");  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                              g_config.mac[0], g_config.mac[1], g_config.mac[2],
                              g_config.mac[3], g_config.mac[4], g_config.mac[5]);
    Serial.print("LEDs/strip: "); Serial.println(g_config.ledWidth);
  #endif
  watchdog_feed();  // after config load (SD I/O can take >1s)

  // ---- Initialise LED controller (dual OctoWS2811) ------------------------
  g_leds.begin(g_config.ledWidth);
  g_leds.setOutputMask(g_config.outputActive);
  g_leds.show();
  Serial.println("LEDs OK (16 outputs)");

  // ---- Apply color order from config ------------------------------------
  g_leds.setColorOrder(g_config.colorOrder);

  // ---- Apply playback speed from config ---------------------------------
  g_playback.setSpeed(g_config.playbackSpeed);

  // ---- Initialise OLED menu system (before Ethernet — SSD1306 timeout) ----
  g_menu.begin();
  Serial.println("MenuManager OK");

  watchdog_feed();  // before 1s PHY delay

  // ---- Initialise Ethernet (NativeEthernet on Teensy 4.1) ---------------
  // Delay to allow capacitors to charge before Ethernet PHY power spike
  delay(1000);
  watchdog_feed();  // after 1s PHY delay
  #ifndef DIAG_NO_ETHERNET
    Ethernet.begin(g_config.mac, g_config.ip);
    Serial.print("Ethernet IP: ");
    Serial.println(Ethernet.localIP());
    // ---- Initialise ArtNet ------------------------------------------------
    g_artNet.setLedsPerStrip(g_config.ledWidth);
    g_artNet.setUniverseMapping(g_config.startUniverse);
    g_artNet.setOutputActive(g_config.outputActive);
    g_artNet.setFrameCallback(onArtNetFrame);
    g_artNet.begin();
    // ---- Initialise TCP server --------------------------------------------
    g_tcp.begin();
    Serial.println("TCP server on port 8888");
    // ---- Initialise UDP discovery ------------------------------------------
    g_discovery.begin();
    Serial.println("UDP discovery on port 7777");
  #else
    Serial.println("DIAG: NO_ETHERNET mode – Ethernet/ArtNet/TCP/UDP disabled");
  #endif

  // ---- Set initial mode -------------------------------------------------
  g_mode = g_config.mode;
  if (g_mode == MODE_PLAYBACK) {
    // Resume last file if available, otherwise play sequence
    if (strlen(g_config.lastPlayFile) > 0) {
      Serial.printf("Resuming: %s\n", g_config.lastPlayFile);
      if (!g_playback.playFile(g_config.lastPlayFile)) {
        Serial.println("Resume file not found – playing sequence");
        int n = g_playback.playSequence();
        if (n == 0) g_mode = MODE_ARTNET;
      }
    } else {
      int n = g_playback.playSequence();
      Serial.printf("Playback: %d .BIN files found\n", n);
      if (n == 0) {
        Serial.println("No .BIN files found – switching to ArtNet");
        g_mode = MODE_ARTNET;
      }
    }
  }

  Serial.println("=== Setup complete ===");
  // Force first render after everything is initialised, before entering loop
  g_menu.update();

  // ---- DIAGNOSTIC: Force display ON with test pattern ----
  // Draw every pixel white to confirm the OLED panel works
  g_menu.markDirty();                     // force redraw next update
  for (int i = 0; i < 10; i++) {
    g_menu.update();
    delay(10);
  }
  // If you see a flash, the display hardware works
  // If not, the issue is physical (voltage/ripple/connections)
}

// ========================  LOOP  ==========================================

void loop()
{
  watchdog_feed();

  watchdog_heartbeat();

  char cmdBuffer[CMD_BUFFER_SIZE];
  int  cmd = g_tcp.poll(cmdBuffer, sizeof(cmdBuffer));

  if (cmd != CMD_NONE) {
    handleTcpCommand(cmd, cmdBuffer);
  }

  if (g_identifyActive) {
    uint32_t now = millis();
    if (now - g_identifyLastToggle >= 200) {
      g_identifyLastToggle = now;
      g_identifyLedState = !g_identifyLedState;
      digitalWrite(LED_BUILTIN, g_identifyLedState);
      g_identifyBlinkCount++;
      if (g_identifyBlinkCount >= 20) {
        g_identifyActive = false;
        digitalWrite(LED_BUILTIN, LOW);
        pinMode(LED_BUILTIN, INPUT);
      }
    }
  }

  g_discovery.poll(g_config.nickname, MODEL_STRING, FW_VERSION,
                   Ethernet.localIP(), 0);

  switch (g_mode) {

    case MODE_ARTNET:
      g_artNet.poll();
      if (g_artNet.isFrameReady()) {
        g_artNet.clearFrameReady();
        g_leds.show();
      }
      break;

    case MODE_PLAYBACK:
      if (g_playback.isPlaying()) {
        uint32_t frameTimeUs = 0;
        uint16_t pixelCount  = 0;
        if (g_playback.playNextFrame(g_playbackBuffer, &frameTimeUs, &pixelCount)) {
          uint16_t dataBytes = pixelCount * 3;
          uint16_t maxBytes  = g_leds.totalPixels() * 3;
          if (dataBytes > maxBytes) {
            g_leds.fillFromBin(g_playbackBuffer, maxBytes);
          } else {
            g_leds.fillFromBin(g_playbackBuffer, dataBytes);
          }
          g_leds.show();
        }
      } else {
        delay(1);
      }
      break;

    case MODE_RECORD:
      g_artNet.poll();
      if (g_artNet.isFrameReady()) {
        g_artNet.clearFrameReady();
        g_leds.show();
      }
      break;

    case MODE_TEST:
      runTestAnimation();
      break;
  }

  uint32_t now = millis();
  if (now - g_fpsLastPrint >= 1000) {
    g_fpsDisplay = g_fpsFrames;
    g_fpsFrames = 0;
    g_fpsLastPrint = now;
  }

  g_menu.update();

  delay(0);
}

// ========================  ARTNET FRAME CALLBACK  ========================

void onArtNetFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  g_leds.fillFrameDirect(rgbData, totalPixels);

  if (g_recArmed && !g_playback.isRecording()) {
    bool shouldStart = false;
    if (g_recStartMode == 0) {
      shouldStart = true;
    } else if (g_recStartMode == 1) {
      shouldStart = g_artNet.hasNonZeroPixels();
      if (shouldStart) {
        Serial.println("TRIGGER: First non-zero pixel detected, starting recording");
      }
    } else if (g_recStartMode == 2) {
      uint32_t pxl = (uint32_t)g_recTrigUniv * 170 + (uint32_t)(g_recTrigCh / 3);
      if (pxl < totalPixels) {
        uint8_t val = rgbData[pxl * 3];
        if (val != g_recLastTrigVal) {
          g_recLastTrigVal = val;
          shouldStart = true;
          Serial.printf("TRIGGER: Channel %d changed to %d\n", g_recTrigCh, val);
        }
      }
    }
    if (shouldStart) {
      g_recArmed = false;
      g_playback.startRecording();
      g_playback.resetFrameCount();
      g_recStopStart = millis() / 1000;
      if (g_mode == MODE_ARTNET) g_mode = MODE_RECORD;
      Serial.println("Recording started by trigger");
    }
  }

  if (g_playback.isRecording()) {
    uint32_t nowUs = micros();
    uint32_t sinceLast = (uint32_t)(nowUs - g_lastRecordUs);
    bool dedup = (g_lastRecordUs != 0 && sinceLast < 15000);
    if (!dedup) {
      uint32_t frameTimeUs;
      if (g_lastRecordUs == 0) {
        frameTimeUs = 1000000 / g_config.recordFps;
      } else {
        frameTimeUs = sinceLast;
        if (frameTimeUs < 1000) frameTimeUs = 1000;
        if (frameTimeUs > 2000000) frameTimeUs = 2000000;
      }
      g_lastRecordUs = nowUs;
      g_playback.writeFrame(rgbData, totalPixels, frameTimeUs);
    }

    bool shouldStop = false;
    if (g_recStopMode == 0) {
    } else if (g_recStopMode == 1) {
      if (!g_artNet.hasNonZeroPixels()) {
        shouldStop = true;
        Serial.println("STOP TRIGGER: All pixels zero, stopping recording");
      }
    } else if (g_recStopMode == 2) {
      uint32_t elapsed = (millis() / 1000) - g_recStopStart;
      if (elapsed >= g_recStopSecs) {
        shouldStop = true;
        Serial.printf("STOP TRIGGER: Timer expired (%lu secs)\n", g_recStopSecs);
      }
    }
    if (shouldStop) {
      g_playback.stopRecording();
      g_recArmed = false;
      Serial.println("Recording stopped by trigger");
    }
  }

  g_frameCounter++;
  g_fpsFrames++;
}

// ========================  TCP COMMAND HANDLER  ==========================

void handleTcpCommand(int cmd, const char *cmdStr)
{
  switch (cmd) {

    case CMD_MODE_ARTNET:
      g_tcp.sendResponse("OK:switching to ArtNet mode");
      setMode(MODE_ARTNET);
      break;

    case CMD_MODE_PLAYBACK:
      g_tcp.sendResponse("OK:switching to Playback mode");
      setMode(MODE_PLAYBACK);
      break;

    case CMD_MODE_RECORD:
      g_tcp.sendResponse("OK:switching to Record mode");
      setMode(MODE_RECORD);
      break;

    case CMD_MODE_TEST:
      g_tcp.sendResponse("OK:switching to Test mode");
      g_testStartMs = millis();
      setMode(MODE_TEST);
      break;

    case CMD_REC_START:
      if (g_recStartMode == 0 || g_recArmed) {
        if (g_playback.startRecording()) {
          g_lastArtNetFrame = micros();
          g_playback.resetFrameCount();
          g_recStopStart = millis() / 1000;
          g_recArmed = false;
          if (g_mode == MODE_ARTNET) g_mode = MODE_RECORD;
          g_tcp.sendResponse("OK:recording started");
          Serial.printf("Recording started: %s\n", g_playback.currentFilename());
        } else {
          g_tcp.sendResponse("ERR:could not start recording");
        }
      } else {
        g_recArmed = true;
        g_tcp.sendResponse("OK:armed, waiting for trigger");
        Serial.println("Recording armed, waiting for trigger");
      }
      break;

    case CMD_REC_ARM:
      g_recArmed = true;
      g_recLastTrigVal = 0;
      g_tcp.sendResponse("OK:armed for trigger");
      Serial.println("Recording armed by REC:ARM");
      break;

    case CMD_REC_START_MODE:
      {
        int m = atoi(cmdStr + 15);
        if (m >= 0 && m <= 2) {
          g_recStartMode = (uint8_t)m;
          g_tcp.sendResponse("OK:start mode set");
        } else {
          g_tcp.sendResponse("ERR:invalid start mode (0-2)");
        }
      }
      break;

    case CMD_REC_STOP_MODE:
      {
        int m = atoi(cmdStr + 14);
        if (m >= 0 && m <= 2) {
          g_recStopMode = (uint8_t)m;
          g_tcp.sendResponse("OK:stop mode set");
        } else {
          g_tcp.sendResponse("ERR:invalid stop mode (0-2)");
        }
      }
      break;

    case CMD_REC_TRIGGER_UNIV:
      g_recTrigUniv = (uint16_t)atoi(cmdStr + 17);
      g_tcp.sendResponse("OK:trigger universe set");
      break;

    case CMD_REC_TRIGGER_CH:
      g_recTrigCh = (uint16_t)atoi(cmdStr + 15);
      g_tcp.sendResponse("OK:trigger channel set");
      break;

    case CMD_REC_STOP_SECS:
      g_recStopSecs = (uint32_t)atoi(cmdStr + 14);
      g_tcp.sendResponse("OK:stop seconds set");
      break;

    case CMD_REC_STOP:
      if (g_playback.stopRecording()) {
        g_tcp.sendResponse("OK:recording stopped");
        Serial.println("Recording stopped");
      } else {
        g_tcp.sendResponse("ERR:not recording");
      }
      break;

    case CMD_CONFIG: {
      const char *kv = cmdStr + 7;
      if (!strncmp(kv, "ip=", 3)) {
        IPAddress newIp; newIp.fromString(kv + 3);
        g_config.ip = newIp;
        saveConfig(g_config);
        g_tcp.sendResponse("OK:ip updated – rebooting");
        delay(100);
        rebootTeensy();
      } else if (!strncmp(kv, "mac=", 4)) {
        uint8_t m[6];
        if (sscanf(kv + 4, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
          memcpy(g_config.mac, m, 6);
          saveConfig(g_config);
          g_tcp.sendResponse("OK:mac updated – rebooting");
          delay(100);
          rebootTeensy();
        } else {
          g_tcp.sendResponse("ERR:invalid MAC format");
        }
      } else if (!strncmp(kv, "led_width=", 10)) {
        int w = atoi(kv + 10);
        if (w > 0) {
          g_config.ledWidth = (uint16_t)w;
          saveConfig(g_config);
          g_tcp.sendResponse("OK:led_width updated – rebooting");
          delay(100);
          rebootTeensy();
        } else {
          g_tcp.sendResponse("ERR:invalid led_width");
        }
      } else if (!strncmp(kv, "start_universe=", 15)) {
        int idx = 0;
        char val[128]; strncpy(val, kv + 15, 127); val[127] = 0;
        char *tok = strtok(val, ",");
        while (tok && idx < MAX_OUTPUTS) {
          g_config.startUniverse[idx++] = (uint16_t)atoi(tok);
          tok = strtok(NULL, ",");
        }
        saveConfig(g_config);
        g_tcp.sendResponse("OK:start_universe saved");
      } else if (!strncmp(kv, "output_active=", 14)) {
        int idx = 0;
        char val[64]; strncpy(val, kv + 14, 63); val[63] = 0;
        char *tok = strtok(val, ",");
        Serial.print("output_active received: ");
        while (tok && idx < MAX_OUTPUTS) {
          g_config.outputActive[idx++] = (atoi(tok) != 0);
          Serial.print(tok);
          Serial.print(idx < MAX_OUTPUTS ? "," : "");
          tok = strtok(NULL, ",");
        }
        Serial.println();
        saveConfig(g_config);
        Serial.println("saveConfig() done");
        g_artNet.setOutputActive(g_config.outputActive);
        g_tcp.sendResponse("OK:output_active saved (live)");
      } else if (!strncmp(kv, "color_order=", 12)) {
        ColorOrder order = parseColorOrder(kv + 12);
        g_config.colorOrder = order;
        g_leds.setColorOrder(order);
        saveConfig(g_config);
        g_tcp.sendResponse("OK:color_order updated (live)");
        Serial.printf("Color order set to: %s\n", colorOrderStr(order));
      } else if (!strncmp(kv, "record_fps=", 11)) {
        int fps = atoi(kv + 11);
        if (fps >= 5 && fps <= 60) {
          g_config.recordFps = (uint16_t)fps;
          saveConfig(g_config);
          g_tcp.sendResponse("OK:record_fps updated – rebooting");
          delay(100);
          rebootTeensy();
        } else {
          g_tcp.sendResponse("ERR:record_fps must be 5-60");
        }
      } else if (!strncmp(kv, "nickname=", 9)) {
        strncpy(g_config.nickname, kv + 9, sizeof(g_config.nickname) - 1);
        g_config.nickname[sizeof(g_config.nickname) - 1] = 0;
        saveConfig(g_config);
        g_tcp.sendResponse("OK:nickname updated (live)");
        Serial.printf("Nickname set to: %s\n", g_config.nickname);
      } else {
        g_tcp.sendResponse("ERR:unknown config key");
      }
      break;
    }

    case CMD_PLAY_FILE: {
      const char *fn = cmdStr + 5;
      if (g_playback.playFile(fn)) {
        g_mode = MODE_PLAYBACK;
        strncpy(g_config.lastPlayFile, fn, sizeof(g_config.lastPlayFile) - 1);
        g_config.lastPlayFile[sizeof(g_config.lastPlayFile) - 1] = 0;
        saveConfig(g_config);
        g_tcp.sendResponse("OK:playing file");
      } else {
        g_tcp.sendResponse("ERR:could not open file");
      }
      break;
    }

    case CMD_PLAY_SEQUENCE: {
      int n = g_playback.playSequence();
      if (n > 0) {
        g_mode = MODE_PLAYBACK;
        g_tcp.sendResponse("OK:playing sequence");
      } else {
        g_tcp.sendResponse("ERR:no .BIN files found");
      }
      break;
    }

    case CMD_STOP:
      if (g_playback.isPlaying()) {
        g_leds.clear();
        g_leds.show();
      }
      g_playback.stop();
      g_tcp.sendResponse("OK:stopped");
      break;

    case CMD_SPEED: {
      float speed = atof(cmdStr + 6);
      if (speed >= 0.05f && speed <= 5.0f) {
        g_playback.setSpeed(speed);
        g_config.playbackSpeed = speed;
        saveConfig(g_config);
        g_tcp.sendResponse("OK:speed set");
        Serial.printf("Playback speed: %.2f\n", speed);
      } else {
        g_tcp.sendResponse("ERR:speed out of range (0.05-5.0)");
      }
      break;
    }

    case CMD_LIST: {
      char names[64][16];
      int count = sdListBinFiles(names, 64);
      g_tcp.sendResponse("OK:LIST");
      for (int i = 0; i < count; i++) {
        g_tcp.sendResponse(names[i]);
      }
      g_tcp.sendResponse("END:LIST");
      break;
    }

    case CMD_DELETE: {
      const char *fn = cmdStr + 7;
      if (sdFileDelete(fn)) {
        g_tcp.sendResponse("OK:deleted");
        Serial.printf("Deleted: %s\n", fn);
      } else {
        g_tcp.sendResponse("ERR:could not delete");
      }
      break;
    }

    case CMD_TEST_PATTERN:
      {
        int pat = atoi(cmdStr + 21);
        if (pat >= 0 && pat <= 4) {
          g_testPattern = (uint8_t)pat;
          g_testStartMs = millis();
          g_tcp.sendResponse("OK:test pattern set");
        } else {
          g_tcp.sendResponse("ERR:invalid test pattern (0-4)");
        }
      }
      break;

    case CMD_TEST_OUTPUT:
      {
        int out = atoi(cmdStr + 20);
        if (out == 255 || (out >= 0 && out <= 15)) {
          g_testOutput = (uint8_t)out;
          g_tcp.sendResponse("OK:test output set");
        } else {
          g_tcp.sendResponse("ERR:invalid test output (0-15 or 255)");
        }
      }
      break;

    case CMD_IDENTIFY:
      Serial.println("IDENTIFY: flashing LED (non-blocking)");
      g_tcp.sendResponse("OK:identify");
      pinMode(LED_BUILTIN, OUTPUT);
      g_identifyActive     = true;
      g_identifyBlinkCount = 0;
      g_identifyLastToggle = millis();
      g_identifyLedState   = false;
      digitalWrite(LED_BUILTIN, LOW);
      break;

    case CMD_PING:
      g_tcp.sendResponse("PONG");
      break;

    case CMD_NUM_OUTPUTS:
      {
        char reply[32];
        snprintf(reply, sizeof(reply), "OUTPUTS=%d,%d", ACTIVE_OUTPUTS, MAX_OUTPUTS);
        g_tcp.sendResponse(reply);
      }
      break;

    case CMD_STATUS:
      printStatus();
      break;

    case CMD_UNKNOWN:
      g_tcp.sendResponse("ERR:unknown command");
      break;

    default:
      break;
  }
}

// ========================  MODE SWITCH  ===================================

void setMode(OperatingMode newMode)
{
  g_playback.stop();
  g_leds.clear();
  g_leds.show();

  g_mode = newMode;

  g_config.mode = newMode;
  saveConfig(g_config);

  if (newMode == MODE_PLAYBACK) {
    int n = g_playback.playSequence();
    if (n == 0) {
      Serial.println("No .BIN files for playback");
    }
  }

  Serial.print("Mode set to: ");
  switch (newMode) {
    case MODE_ARTNET:   Serial.println("ArtNet");  break;
    case MODE_PLAYBACK: Serial.println("Playback"); break;
    case MODE_RECORD:   Serial.println("Record");   break;
    case MODE_TEST:     Serial.println("Test");     break;
  }
}

// ========================  STATUS  ========================================

void printStatus()
{
  char su[128];
  int pos = 0;
  for (int i = 0; i < ACTIVE_OUTPUTS; i++) {
    pos += snprintf(su + pos, sizeof(su) - pos, "%s%u",
                    i > 0 ? "," : "", g_config.startUniverse[i]);
  }

  char oa[64];
  pos = 0;
  for (int i = 0; i < ACTIVE_OUTPUTS; i++) {
    pos += snprintf(oa + pos, sizeof(oa) - pos, "%s%c",
                    i > 0 ? "," : "", g_config.outputActive[i] ? '1' : '0');
  }

  char buf[640];
  snprintf(buf, sizeof(buf),
    "mode=%s\r\n"
    "ip=%d.%d.%d.%d\r\n"
    "led_width=%u\r\n"
    "fps=%lu\r\n"
    "recording=%s\r\n"
    "playing=%s\r\n"
    "file=%s\r\n"
    "frames=%lu\r\n"
    "artnet_active=%s\r\n"
    "artnet_fps=%lu\r\n"
    "color_order=%s\r\n"
    "playback_speed=%.2f\r\n"
    "record_fps=%u\r\n"
    "record_time=%lu\r\n"
    "start_universe=%s\r\n"
    "file_pos=%lu\r\n"
    "file_total=%lu\r\n"
    "output_active=%s\r\n"
    "output_count=%d",
    (g_mode == MODE_ARTNET)   ? "artnet" :
    (g_mode == MODE_PLAYBACK) ? "playback" :
    (g_mode == MODE_RECORD)   ? "record" : "test",
    g_config.ip[0], g_config.ip[1], g_config.ip[2], g_config.ip[3],
    g_config.ledWidth,
    g_frameCounter,
    g_playback.isRecording() ? "yes" : "no",
    g_playback.isPlaying() ? "yes" : "no",
    g_playback.currentFilename(),
    g_playback.framesPlayed(),
    g_artNet.isReceiving() ? "yes" : "no",
    (g_mode == MODE_ARTNET) ? g_fpsDisplay : 0,
    colorOrderStr(g_config.colorOrder),
    g_playback.getSpeed(),
    g_config.recordFps,
    g_playback.getRecordTime(),
    su,
    (g_playback.isPlaying() ? g_playback.filePosition() : 0),
    (g_playback.isPlaying() ? g_playback.fileSize() : 0),
    oa,
    ACTIVE_OUTPUTS
  );
  g_tcp.sendResponse(buf);
}

// ========================  TEST MODE ANIMATION  ==========================

void runTestAnimation()
{
  uint8_t r, g, b;
  uint16_t stripLen = g_leds.getLedsPerStrip();
  uint16_t totalPixels = g_leds.totalPixels();

  if (g_testPattern == 0) {
    uint32_t slot = (millis() - g_testStartMs) / 1000;
    slot &= 3;
    if      (slot == 0) { r = 255; g = 0;   b = 0;   }
    else if (slot == 1) { r = 0;   g = 255; b = 0;   }
    else if (slot == 2) { r = 0;   g = 0;   b = 255; }
    else                { r = 255; g = 255; b = 255; }
  } else if (g_testPattern == 1) {
    uint32_t elapsed = millis() - g_testStartMs;
    uint16_t hue = (elapsed * 85UL) / 1000UL;
    hue &= 0xFF;
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - region * 43) * 6;
    switch (region) {
      case 0: r = 255; g = remainder;       b = 0;          break;
      case 1: r = 255 - remainder; g = 255; b = 0;          break;
      case 2: r = 0;          g = 255; b = remainder;       break;
      case 3: r = 0;          g = 255 - remainder; b = 255; break;
      case 4: r = remainder;  g = 0;          b = 255;      break;
      default: r = 255;       g = 0;          b = 255 - remainder; break;
    }
  } else if (g_testPattern == 2) {
    r = 255; g = 0; b = 0;
  } else if (g_testPattern == 3) {
    r = 0; g = 255; b = 0;
  } else {
    r = 0; g = 0; b = 255;
  }

  uint8_t *buf = g_playbackBuffer;

  if (g_testOutput == 255) {
    for (uint16_t i = 0; i < totalPixels; i++) {
      buf[i*3 + 0] = r;
      buf[i*3 + 1] = g;
      buf[i*3 + 2] = b;
    }
  } else {
    memset(buf, 0, totalPixels * 3);
    uint8_t s = g_testOutput;
    uint8_t *dst = buf + s * stripLen * 3;
    for (uint16_t i = 0; i < stripLen; i++) {
      dst[i*3 + 0] = r;
      dst[i*3 + 1] = g;
      dst[i*3 + 2] = b;
    }
  }

  g_leds.fillFromBin(buf, totalPixels * 3);
  g_leds.show();
}

// ========================  REBOOT  ========================================

void rebootTeensy()
{
  __disable_irq();
  SCB_AIRCR = 0x05FA0004;
  while (1);
}