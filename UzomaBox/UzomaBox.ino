/*  UzomaBox.ino
 *  Teensy 4.1 multi-mode LED controller
 *
 *  Three modes:
 *    - ArtNet:  receives pixel data over ArtNet UDP (port 6454)
 *    - Playback: plays .BIN files from SD card
 *    - Record:   records ArtNet data to .BIN files on SD card
 *
 *  TCP server on port 8888 accepts commands from a desktop app.
 *
 *  Hardware:
 *    Teensy 4.1 + OctoWS2811 Adaptor
 *    8 × WS2811 LED strips (default 512 LEDs each)
 *    SD card on built-in microSD slot (BUILTIN_SDCARD)
 *    NativeEthernet for ArtNet + TCP
 */

#include "Config.h"
#include "SDManager.h"
#include "LEDController.h"
#include "ArtNetHandler.h"
#include "TCPHandler.h"
#include "PlaybackController.h"
#include "UdpDiscovery.h"

// ========================  GLOBAL OBJECTS  ================================

AppConfig          g_config;
LEDController      g_leds;
ArtNetHandler      g_artNet;
TCPHandler         g_tcp;
PlaybackController g_playback;
UdpDiscovery       g_discovery;

// Current operating mode
OperatingMode      g_mode = MODE_ARTNET;

// Recording state (active even in ArtNet mode when recording is triggered)
bool               g_recordingActive = false;

// Frame timing for ArtNet mode
uint32_t           g_lastArtNetFrame = 0;
uint32_t           g_frameCounter    = 0;

// Test mode state
uint32_t           g_testStartMs     = 0;
uint8_t            g_testPattern     = 0;  // 0=RGBW cycle, 1=rainbow fade, 2=red, 3=green, 4=blue
uint8_t            g_testOutput      = 255; // 255=all, 0-7=specific strip

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
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== UzomaBox ===");

  // ---- Initialise SD card (Teensy 4.1 built-in microSD slot) ------------
  if (!sdInit()) {
    Serial.println("FATAL: No SD card found");
    while (1) { delay(1000); }
  }
  Serial.println("SD card OK");

  // ---- Load / create config ---------------------------------------------
  loadConfig(g_config);
  Serial.print("Mode: ");
  switch (g_config.mode) {
    case MODE_ARTNET:   Serial.println("ArtNet");  break;
    case MODE_PLAYBACK: Serial.println("Playback"); break;
    case MODE_RECORD:   Serial.println("Record");   break;
  }
  Serial.print("IP: ");   Serial.println(g_config.ip);
  Serial.print("MAC: ");  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                            g_config.mac[0], g_config.mac[1], g_config.mac[2],
                            g_config.mac[3], g_config.mac[4], g_config.mac[5]);
  Serial.print("LEDs/strip: "); Serial.println(g_config.ledWidth);

  // ---- Initialise LED controller ----------------------------------------
  g_leds.begin(g_config.ledWidth);
  g_leds.setOutputMask(g_config.outputActive);
  g_leds.show();
  Serial.println("LEDs OK");

  // ---- Initialise Ethernet (NativeEthernet on Teensy 4.1) ---------------
  Ethernet.begin(g_config.mac, g_config.ip);
  delay(1000);
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
          uint8_t *drawMem = g_leds.getDrawingMemory();
          uint16_t maxSize  = g_leds.totalPixels() * 3;

          if (dataSize > maxSize) {
            // File has more pixels than we can display:
            // read what fits, then skip the rest so SD stays aligned
            if (sdCardRead(drawMem, maxSize)) {
              // Apply color-order reordering inline on the drawing memory
              {
                const uint8_t *perm = colorOrderPerm[g_leds.getColorOrder()];
                uint16_t pxCount = maxSize / 3;
                for (uint16_t pi = 0; pi < pxCount; pi++) {
                  uint8_t *px = drawMem + pi * 3;
                  uint8_t tmp[3] = {px[0], px[1], px[2]};
                  px[0] = tmp[perm[0]];
                  px[1] = tmp[perm[1]];
                  px[2] = tmp[perm[2]];
                }
              }
              sdCardSkip(dataSize - maxSize);
              g_leds.show();
            }
          } else {
            // File has fewer (or equal) pixels:
            // read everything, zero-fill the rest of drawing memory
            if (sdCardRead(drawMem, dataSize)) {
              // Apply color-order reordering inline on the drawing memory
              {
                const uint8_t *perm = colorOrderPerm[g_leds.getColorOrder()];
                uint16_t pxCount = dataSize / 3;
                for (uint16_t pi = 0; pi < pxCount; pi++) {
                  uint8_t *px = drawMem + pi * 3;
                  uint8_t tmp[3] = {px[0], px[1], px[2]};
                  px[0] = tmp[perm[0]];
                  px[1] = tmp[perm[1]];
                  px[2] = tmp[perm[2]];
                }
              }
              if (dataSize < maxSize) {
                memset(drawMem + dataSize, 0, maxSize - dataSize);
              }
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

  // ---- Incoming ArtNet FPS meter ----------------------------------------
  uint32_t now = millis();
  if (now - g_fpsLastPrint >= 1000) {
    Serial.printf("ArtNet FPS: %lu\n", g_fpsFrames);
    g_fpsFrames = 0;
    g_fpsLastPrint = now;
  }

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

  // If recording, write frame to .BIN file with fixed frame time
  if (g_recordingActive) {
    // Use a fixed frame time based on the configured recording FPS.
    // This ensures all frames are evenly spaced and playback at 1x
    // reproduces the exact same frame rate as recording.
    uint32_t frameTimeUs = 1000000 / g_config.recordFps;
    g_playback.writeFrame(rgbData, totalPixels, frameTimeUs);
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
      if (g_playback.startRecording()) {
        g_recordingActive = true;
        g_lastArtNetFrame = micros();
        g_playback.resetFrameCount();
        g_tcp.sendResponse("OK:recording started");
        Serial.printf("Recording started: %s\n", g_playback.currentFilename());
      } else {
        g_tcp.sendResponse("ERR:could not start recording");
      }
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
        char val[64]; strncpy(val, kv + 15, 63); val[63] = 0;
        char *tok = strtok(val, ",");
        while (tok && idx < 8) {
          g_config.startUniverse[idx++] = (uint16_t)atoi(tok);
          tok = strtok(NULL, ",");
        }
        saveConfig(g_config);
        g_tcp.sendResponse("OK:start_universe updated – rebooting");
        delay(100);
        rebootTeensy();
      } else if (!strncmp(kv, "output_active=", 14)) {
        int idx = 0;
        char val[32]; strncpy(val, kv + 14, 31); val[31] = 0;
        char *tok = strtok(val, ",");
        while (tok && idx < 8) {
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
        int pat = atoi(cmdStr + 19); // skip "COMMAND:TEST_PATTERN="
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
        int out = atoi(cmdStr + 19); // skip "COMMAND:TEST_OUTPUT="
        if (out == 255 || (out >= 0 && out <= 7)) {
          g_testOutput = (uint8_t)out;
          g_tcp.sendResponse("OK:test output set");
        } else {
          g_tcp.sendResponse("ERR:invalid test output (0-7 or 255)");
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
  // Build comma-separated start_universe string
  char su[64];
  su[0] = 0;
  for (int i = 0; i < 8; i++) {
    if (i > 0) strcat(su, ",");
    char tmp[8]; sprintf(tmp, "%u", g_config.startUniverse[i]);
    strcat(su, tmp);
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
    "file_total=%lu",
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
    (g_playback.isPlaying() ? g_playback.fileSize() : 0)
  );
  g_tcp.sendResponse(buf);
}

// ========================  TEST MODE ANIMATION  ==========================

void runTestAnimation()
{
  uint8_t r, g, b;
  uint16_t stripLen = g_leds.getLedsPerStrip();

  if (g_testPattern == 0) {
    // Pattern 0: RGBW Cycle (R→G→B→W→R repeating, 1s per color)
    uint32_t slot = (millis() - g_testStartMs) / 1000;  // 0,1,2,3,0,1,...
    slot &= 3;
    if      (slot == 0) { r = 255; g = 0;   b = 0;   }
    else if (slot == 1) { r = 0;   g = 255; b = 0;   }
    else if (slot == 2) { r = 0;   g = 0;   b = 255; }
    else                { r = 255; g = 255; b = 255; }
  } else if (g_testPattern == 1) {
    // Pattern 1: Rainbow Fade (sine-based, 6s cycle)
    static const uint8_t sin256[1024] = {
      0,1,2,3,5,6,7,8,10,11,12,13,15,16,17,18,20,21,22,23,25,26,27,28,30,31,
      32,33,34,36,37,38,39,41,42,43,44,46,47,48,49,50,52,53,54,55,56,58,59,60,
      61,62,64,65,66,67,68,69,71,72,73,74,75,76,78,79,80,81,82,83,84,86,87,88,
      89,90,91,92,93,94,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,
      111,112,113,114,115,116,117,118,119,120,121,122,122,123,124,125,126,127,
      128,129,129,130,131,132,133,133,134,135,136,137,137,138,139,140,140,141,
      142,143,143,144,145,145,146,147,147,148,149,149,150,151,151,152,152,153,
      154,154,155,155,156,156,157,158,158,159,159,160,160,160,161,161,162,162,
      163,163,163,164,164,165,165,165,166,166,166,167,167,168,168,168,168,169,
      169,169,170,170,170,170,171,171,171,171,171,172,172,172,172,173,173,173,
      173,173,173,174,174,174,174,174,174,174,174,175,175,175,175,175,175,175,
      175,175,175,175,175,175,175,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,176,
      176,176,176,176,176,176,176,176,176,175,175,175,175,175,175,175,175,175,
      175,175,175,175,175,174,174,174,174,174,174,174,174,173,173,173,173,173,
      173,172,172,172,172,171,171,171,171,171,170,170,170,170,169,169,169,168,
      168,168,168,167,167,166,166,166,165,165,165,164,164,163,163,163,162,162,
      161,161,160,160,160,159,159,158,158,157,156,156,155,155,154,154,153,152,
      152,151,151,150,149,149,148,147,147,146,145,145,144,143,143,142,141,140,
      140,139,138,137,137,136,135,134,133,133,132,131,130,129,129,128,127,126,
      125,124,123,122,122,121,120,119,118,117,116,115,114,113,112,111,110,109,
      108,107,106,105,104,103,102,101,100,99,98,97,96,94,93,92,91,90,89,88,87,
      86,84,83,82,81,80,79,78,76,75,74,73,72,71,69,68,67,66,65,64,62,61,60,59,
      58,56,55,54,53,52,50,49,48,47,46,44,43,42,41,39,38,37,36,34,33,32,31,30,
      28,27,26,25,23,22,21,20,18,17,16,15,13,12,11,10,8,7,6,5,3,2,1,0
    };
    uint32_t elapsed = millis() - g_testStartMs;
    uint32_t phase = (elapsed * 682UL) / 1000UL;
    phase &= 0xFFF;
    uint32_t p = phase;
    if (p < 1024)       r = sin256[p];
    else if (p < 2048)  r = sin256[2047 - p];
    else if (p < 3072)  r = 255 - sin256[p - 2048];
    else                r = 255 - sin256[4095 - p];
    p = (phase + 1365) & 0xFFF;
    if (p < 1024)       g = sin256[p];
    else if (p < 2048)  g = sin256[2047 - p];
    else if (p < 3072)  g = 255 - sin256[p - 2048];
    else                g = 255 - sin256[4095 - p];
    p = (phase + 2730) & 0xFFF;
    if (p < 1024)       b = sin256[p];
    else if (p < 2048)  b = sin256[2047 - p];
    else if (p < 3072)  b = 255 - sin256[p - 2048];
    else                b = 255 - sin256[4095 - p];
  } else if (g_testPattern == 2) {
    r = 255; g = 0; b = 0;
  } else if (g_testPattern == 3) {
    r = 0; g = 255; b = 0;
  } else {
    r = 0; g = 0; b = 255;
  }

  // Fill — either all outputs or a single output
  if (g_testOutput == 255) {
    for (uint8_t s = 0; s < 8; s++) {
      for (uint16_t i = 0; i < stripLen; i++) {
        g_leds.setPixel(s, i, r, g, b);
      }
    }
  } else {
    uint8_t s = g_testOutput;
    for (uint16_t i = 0; i < stripLen; i++) {
      g_leds.setPixel(s, i, r, g, b);
    }
  }
  g_leds.show();
  delay(16);
}

// ========================  REBOOT  ========================================

void rebootTeensy()
{
  // Teensy 4.1 system reset via ARM SCB_AIRCR register
  __disable_irq();
  SCB_AIRCR = 0x05FA0004;
  while (1);  // wait for reset
}