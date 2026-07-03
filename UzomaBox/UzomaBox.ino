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
 */

#include "Config.h"
#include "SDManager.h"
#include "LEDController.h"
#include "ArtNetHandler.h"
#include "TCPHandler.h"
#include "PlaybackController.h"
#include "UdpDiscovery.h"
#include "MenuManager.h"

// ========================  WATCHDOG  =========================================
// Teensy 4.1 (IMXRT1062) hardware watchdog via WDOG1.
// Uses the internal low-frequency oscillator (~32 kHz) with 256 prescaler.
// WT=255 → timeout ≈ 255 / (32768/256) ≈ 2.0 seconds.
// Called every loop() iteration; any stall >2s triggers a hardware reset.

static void watchdog_enable(void)
{
  // Feed to unlock the configuration register
  WDOG1_WSR = 0x5555;
  WDOG1_WSR = 0xAAAA;
  // WCR: WDE=1 (enable), SRS=1 (assert reset), WDA=1 (allow updates),
  //      WT=255 (max timeout ~2s at 32kHz/256 prescaler)
  WDOG1_WCR = (1 << 0) | (1 << 4) | (1 << 5) | (255 << 8);
}

static inline void watchdog_feed(void)
{
  // Refresh sequence: write 0x5555 then 0xAAAA to the service register
  WDOG1_WSR = 0x5555;
  WDOG1_WSR = 0xAAAA;
}

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

// Recording state (active even in ArtNet mode when recording is triggered)
bool               g_recordingActive = false;
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
uint32_t           g_fpsLastPrint = 0;

// Playback temporary buffer (max 16 strips × 512 LEDs × 3 bytes = 24576)
// Shared between playback and test mode to avoid stack overflow
DMAMEM static uint8_t g_playbackBuffer[512 * 16 * 3];

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
  Serial.println("\n=== UzomaBox (16 outputs) ===");

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

  // ---- Initialise LED controller (dual OctoWS2811) ------------------------
  g_leds.begin(g_config.ledWidth);
  g_leds.setOutputMask(g_config.outputActive);
  g_leds.show();
  Serial.println("LEDs OK (16 outputs)");

  // ---- Initialise Ethernet (NativeEthernet on Teensy 4.1) ---------------
  Ethernet.begin(g_config.mac, g_config.ip);
  Serial.print("Ethernet IP: ");
  Serial.println(Ethernet.localIP());

  // ---- Initialise ArtNet ------------------------------------------------
  g_artNet.setLedsPerStrip(g_config.ledWidth);
  g_artNet.setUniverseMapping(g_config.startUniverse);
  g_artNet.setFrameCallback(onArtNetFrame);
  g_artNet.begin();

  // ---- Initialise TCP server --------------------------------------------
  g_tcp.begin();
  Serial.println("TCP server on port 8888");

  // ---- Initialise UDP discovery ------------------------------------------
  g_discovery.begin();
  Serial.println("UDP discovery on port 7777");

  // ---- Apply color order from config ------------------------------------
  g_leds.setColorOrder(g_config.colorOrder);

  // ---- Apply playback speed from config ---------------------------------
  g_playback.setSpeed(g_config.playbackSpeed);

  // ---- Initialise OLED menu system ----------------------------------------
  g_menu.begin();
  Serial.println("MenuManager OK");

  // ---- Set initial mode -------------------------------------------------
  g_mode = g_config.mode;
  if (g_mode == MODE_PLAYBACK) {
    int n = g_playback.playSequence();
    Serial.printf("Playback: %d .BIN files found\n", n);
    if (n == 0) {
      Serial.println("No .BIN files found – switching to ArtNet");
      g_mode = MODE_ARTNET;
    }
  }

  Serial.println("=== Setup complete ===");
}

// ========================  LOOP  ==========================================

void loop()
{
  // Feed the watchdog — if any operation hangs, board auto-reboots after 8s
  watchdog_feed();

  char cmdBuffer[CMD_BUFFER_SIZE];
  int  cmd = g_tcp.poll(cmdBuffer, sizeof(cmdBuffer));

  if (cmd != CMD_NONE) {
    handleTcpCommand(cmd, cmdBuffer);
  }

  // ---- Non-blocking IDENTIFY blink --------------------------------------
  if (g_identifyActive) {
    uint32_t now = millis();
    if (now - g_identifyLastToggle >= 200) {
      g_identifyLastToggle = now;
      g_identifyLedState = !g_identifyLedState;
      digitalWrite(LED_BUILTIN, g_identifyLedState);
      g_identifyBlinkCount++;
      if (g_identifyBlinkCount >= 20) {  // 10 full blinks (20 toggles)
        g_identifyActive = false;
        digitalWrite(LED_BUILTIN, LOW);
        pinMode(LED_BUILTIN, INPUT);
      }
    }
  }

  // ---- Poll UDP discovery (throttled to every 5s) -----------------------
  if (millis() - g_lastDiscoveryPoll > 5000) {
    g_lastDiscoveryPoll = millis();
    g_discovery.poll(g_config.nickname, MODEL_STRING, FW_VERSION,
                     Ethernet.localIP(), 0);
  }

  // ---- Mode-specific behaviour ------------------------------------------

  switch (g_mode) {

    case MODE_ARTNET:
      // Poll for incoming ArtNet packets – the callback (onArtNetFrame)
      // will be called when a complete frame is assembled.
      g_artNet.poll();

      // If recording is active, frames are captured in the callback
      break;

    case MODE_PLAYBACK:
      if (g_playback.isPlaying()) {
        uint32_t frameTimeUs = 0;
        uint16_t pixelCount  = 0;

        if (g_playback.playNextFrame(&frameTimeUs, &pixelCount)) {
          uint16_t dataSize = pixelCount * 3;
          uint16_t maxSize  = g_leds.totalPixels() * 3;  // _ledsPerStrip * 16 * 3

          if (dataSize > maxSize) {
            // File has more pixels than we can display
            if (sdCardRead(g_playbackBuffer, maxSize)) {
              g_leds.fillFromBin(g_playbackBuffer, maxSize);
              g_leds.show();
            }
            sdCardSkip(dataSize - maxSize);  // keep SD aligned
          } else {
            // File has fewer (or equal) pixels
            if (sdCardRead(g_playbackBuffer, dataSize)) {
              g_leds.fillFromBin(g_playbackBuffer, dataSize);
              g_leds.show();
            }
          }
        }
      } else {
        delay(10);
      }
      break;

    case MODE_RECORD:
      // In record mode, we poll ArtNet to see incoming data
      // (the user triggers REC:START / REC:STOP via TCP)
      g_artNet.poll();
      break;

    case MODE_TEST:
      runTestAnimation();
      break;
  }

  // ---- Incoming ArtNet FPS meter (kept for STATUS, no serial print) -----
  uint32_t now = millis();
  if (now - g_fpsLastPrint >= 1000) {
    g_fpsFrames = 0;
    g_fpsLastPrint = now;
  }

  // ---- Poll OLED menu system (non-blocking) ------------------------------
  g_menu.update();

  // ---- Small yield for watchdog / USB tasks -----------------------------
  // On Teensy 4.1, delay(0) or yield() helps with background tasks.
  delay(0);
}

// ========================  ARTNET FRAME CALLBACK  ========================

void onArtNetFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // Push to LEDs – fillFrameDirect uses memcpy per strip for ORDER_RGB
  g_leds.fillFrameDirect(rgbData, totalPixels);
  g_leds.show();

  // ---- Recording trigger logic ------------------------------------------
  // Start triggers (when armed)
  if (g_recArmed && !g_recordingActive) {
    bool shouldStart = false;
    if (g_recStartMode == 0) {
      shouldStart = true;  // Immediate
    } else if (g_recStartMode == 1) {
      // First non-zero: check if any pixel has data > 0
      for (uint16_t i = 0; i < totalPixels * 3; i++) {
        if (rgbData[i] != 0) {
          shouldStart = true;
          Serial.println("TRIGGER: First non-zero pixel detected, starting recording");
          break;
        }
      }
    } else if (g_recStartMode == 2) {
      // Channel change: check specific universe/channel
      uint16_t pixelIdx = g_recTrigUniv * 512 + g_recTrigCh;
      if (pixelIdx < totalPixels * 3) {
        uint8_t val = rgbData[pixelIdx];
        if (val != g_recLastTrigVal) {
          g_recLastTrigVal = val;
          shouldStart = true;
          Serial.printf("TRIGGER: Channel %d changed to %d\n", g_recTrigCh, val);
        }
      }
    }
    if (shouldStart) {
      g_recArmed = false;
      g_recordingActive = true;
      g_playback.resetFrameCount();
      g_recStopStart = millis() / 1000;
      Serial.println("Recording started by trigger");
    }
  }

  // Write frame if recording is active
  if (g_recordingActive) {
    uint32_t frameTimeUs = 1000000 / g_config.recordFps;
    g_playback.writeFrame(rgbData, totalPixels, frameTimeUs);

    // Stop triggers
    bool shouldStop = false;
    if (g_recStopMode == 0) {
      // Immediate — only stop via REC:STOP command
    } else if (g_recStopMode == 1) {
      // All zero: check if every pixel is 0
      bool allZero = true;
      for (uint16_t i = 0; i < totalPixels * 3; i++) {
        if (rgbData[i] != 0) {
          allZero = false;
          break;
        }
      }
      if (allZero) {
        shouldStop = true;
        Serial.println("STOP TRIGGER: All pixels zero, stopping recording");
      }
    } else if (g_recStopMode == 2) {
      // Timer: check elapsed seconds
      uint32_t elapsed = (millis() / 1000) - g_recStopStart;
      if (elapsed >= g_recStopSecs) {
        shouldStop = true;
        Serial.printf("STOP TRIGGER: Timer expired (%lu secs)\n", g_recStopSecs);
      }
    }
    if (shouldStop) {
      g_playback.stopRecording();
      g_recordingActive = false;
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
          g_recordingActive = true;
          g_lastArtNetFrame = micros();
          g_playback.resetFrameCount();
          g_tcp.sendResponse("OK:recording started");
          g_recStopStart = millis() / 1000;
          Serial.printf("Recording started: %s\n", g_playback.currentFilename());
        } else {
          g_tcp.sendResponse("ERR:could not start recording");
        }
      } else {
        // Non-immediate start: arm and wait for ArtNet trigger
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
        int m = atoi(cmdStr + 15); // skip "REC:START_MODE="
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
        int m = atoi(cmdStr + 14); // skip "REC:STOP_MODE="
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
      if (g_recordingActive) {
        g_playback.stopRecording();
        g_recordingActive = false;
        g_tcp.sendResponse("OK:recording stopped");
        Serial.println("Recording stopped");
      } else {
        g_tcp.sendResponse("ERR:not recording");
      }
      break;

    case CMD_CONFIG: {
      // cmdStr format: "CONFIG:key=value" or "CONFIG:key=val1,val2,..."
      // Extract the config part after "CONFIG:"
      const char *kv = cmdStr + 7;   // skip "CONFIG:"
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
        while (tok && idx < NUM_OUTPUTS) {
          g_config.startUniverse[idx++] = (uint16_t)atoi(tok);
          tok = strtok(NULL, ",");
        }
        saveConfig(g_config);
        g_tcp.sendResponse("OK:start_universe updated – rebooting");
        delay(100);
        rebootTeensy();
      } else if (!strncmp(kv, "output_active=", 14)) {
        int idx = 0;
        char val[64]; strncpy(val, kv + 14, 63); val[63] = 0;
        char *tok = strtok(val, ",");
        while (tok && idx < NUM_OUTPUTS) {
          g_config.outputActive[idx++] = (atoi(tok) != 0);
          tok = strtok(NULL, ",");
        }
        saveConfig(g_config);
        g_tcp.sendResponse("OK:output_active updated – rebooting");
        delay(100);
        rebootTeensy();
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
      // cmdStr: "PLAY:filename.BIN"
      const char *fn = cmdStr + 5;
      if (g_playback.playFile(fn)) {
        g_mode = MODE_PLAYBACK;
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
      g_playback.stop();
      g_recordingActive = false;
      g_leds.clear();
      g_leds.show();
      g_tcp.sendResponse("OK:stopped");
      break;

    case CMD_SPEED: {
      // cmdStr: "SPEED:1.5"
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
      // List all .BIN files on SD card
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
      // cmdStr: "DELETE:filename.BIN"
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
        int pat = atoi(cmdStr + 21); // skip "COMMAND:TEST_PATTERN="
        Serial.printf("DEBUG: CMD_TEST_PATTERN parsed pat=%d from: %s\n", pat, cmdStr);
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
        int out = atoi(cmdStr + 20); // skip "COMMAND:TEST_OUTPUT="
        Serial.printf("DEBUG: CMD_TEST_OUTPUT parsed out=%d from: %s\n", out, cmdStr);
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
        snprintf(reply, sizeof(reply), "NUM_OUTPUTS=%d", NUM_OUTPUTS);
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
  // Stop any ongoing activity
  g_playback.stop();
  g_recordingActive = false;
  g_leds.clear();
  g_leds.show();

  g_mode = newMode;

  // Update and persist config
  g_config.mode = newMode;
  saveConfig(g_config);

  if (newMode == MODE_PLAYBACK) {
    // Auto-start sequence
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
  // Build comma-separated start_universe string (bounds-safe)
  char su[128];
  int pos = 0;
  for (int i = 0; i < NUM_OUTPUTS; i++) {
    pos += snprintf(su + pos, sizeof(su) - pos, "%s%u",
                    i > 0 ? "," : "", g_config.startUniverse[i]);
  }

  char buf[512];
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
    "file_total=%lu"
    "output_count=%d",
    (g_mode == MODE_ARTNET)   ? "artnet" :
    (g_mode == MODE_PLAYBACK) ? "playback" :
    (g_mode == MODE_RECORD)   ? "record" : "test",
    g_config.ip[0], g_config.ip[1], g_config.ip[2], g_config.ip[3],
    g_config.ledWidth,
    g_frameCounter,
    g_recordingActive ? "yes" : "no",
    g_playback.isPlaying() ? "yes" : "no",
    g_playback.currentFilename(),
    g_playback.framesPlayed(),
    g_artNet.isReceiving() ? "yes" : "no",
    (g_mode == MODE_ARTNET) ? g_fpsFrames : 0,
    colorOrderStr(g_config.colorOrder),
    g_playback.getSpeed(),
    g_config.recordFps,
    g_playback.getRecordTime(),
    su,
    (g_playback.isPlaying() ? g_playback.filePosition() : 0),
    (g_playback.isPlaying() ? g_playback.fileSize() : 0),
    NUM_OUTPUTS
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
    // Pattern 0: RGBW Cycle (R→G→B→W→R repeating, 1s per color)
    uint32_t slot = (millis() - g_testStartMs) / 1000;  // 0,1,2,3,0,1,...
    slot &= 3;
    if      (slot == 0) { r = 255; g = 0;   b = 0;   }
    else if (slot == 1) { r = 0;   g = 255; b = 0;   }
    else if (slot == 2) { r = 0;   g = 0;   b = 255; }
    else                { r = 255; g = 255; b = 255; }
  } else if (g_testPattern == 1) {
    // Pattern 1: Color Fade (hue wheel, no white gaps, smooth)
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

  // Build frame data in the playback buffer, then use fillFromBin
  // This handles both Octo instances and respects color order
  uint8_t *buf = g_playbackBuffer;

  if (g_testOutput == 255) {
    // All strips: fill entire buffer with the computed color
    for (uint16_t i = 0; i < totalPixels; i++) {
      buf[i*3 + 0] = r;
      buf[i*3 + 1] = g;
      buf[i*3 + 2] = b;
    }
  } else {
    // Single strip: zero all, then fill only the target strip
    memset(buf, 0, totalPixels * 3);
    uint8_t s = g_testOutput;
    uint8_t *dst = buf + s * stripLen * 3;
    for (uint16_t i = 0; i < stripLen; i++) {
      dst[i*3 + 0] = r;
      dst[i*3 + 1] = g;
      dst[i*3 + 2] = b;
    }
  }

  // Use fillFromBin which handles both Octo instances and color ordering
  g_leds.fillFromBin(buf, totalPixels * 3);
  g_leds.show();
}

// ========================  REBOOT  ========================================

void rebootTeensy()
{
  // Teensy 4.1 system reset via ARM SCB_AIRCR register
  __disable_irq();
  SCB_AIRCR = 0x05FA0004;
  while (1);  // wait for reset
}