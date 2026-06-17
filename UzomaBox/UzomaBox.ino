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

// ========================  GLOBAL OBJECTS  ================================

AppConfig          g_config;
LEDController      g_leds;
ArtNetHandler      g_artNet;
TCPHandler         g_tcp;
PlaybackController g_playback;

// Current operating mode
OperatingMode      g_mode = MODE_ARTNET;

// Recording state (active even in ArtNet mode when recording is triggered)
bool               g_recordingActive = false;

// Frame timing for ArtNet mode
uint32_t           g_lastArtNetFrame = 0;
uint32_t           g_frameCounter    = 0;

// ========================  FORWARD DECLARATIONS  ==========================

void onArtNetFrame(const uint8_t *rgbData, uint16_t totalPixels);
void handleTcpCommand(int cmd, const char *cmdStr);
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
              sdCardSkip(dataSize - maxSize);
              g_leds.show();
            }
          } else {
            // File has fewer (or equal) pixels:
            // read everything, zero-fill the rest of drawing memory
            if (sdCardRead(drawMem, dataSize)) {
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
  }

  // ---- Small yield for watchdog / USB tasks -----------------------------
  // On Teensy 4.1, delay(0) or yield() helps with background tasks.
  delay(0);
}

// ========================  ARTNET FRAME CALLBACK  ========================

void onArtNetFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // Push to LEDs
  g_leds.fillFrame(rgbData, totalPixels);
  g_leds.show();

  // If recording, write frame to .BIN file
  if (g_recordingActive) {
    uint32_t now = micros();
    uint32_t dt  = (g_lastArtNetFrame == 0) ? 0 : (now - g_lastArtNetFrame);
    g_lastArtNetFrame = now;
    if (dt > 100000) dt = 16666;   // clamp to ~60 fps max if gap too large

    g_playback.writeFrame(rgbData, totalPixels, dt);
  }

  g_frameCounter++;
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
  }
}

// ========================  STATUS  ========================================

void printStatus()
{
  char buf[256];
  snprintf(buf, sizeof(buf),
    "mode=%s\r\n"
    "ip=%d.%d.%d.%d\r\n"
    "led_width=%u\r\n"
    "fps=%lu\r\n"
    "recording=%s\r\n"
    "playing=%s\r\n"
    "file=%s\r\n"
    "frames=%lu\r\n"
    "artnet_active=%s",
    (g_mode == MODE_ARTNET)   ? "artnet" :
    (g_mode == MODE_PLAYBACK) ? "playback" : "record",
    g_config.ip[0], g_config.ip[1], g_config.ip[2], g_config.ip[3],
    g_config.ledWidth,
    g_frameCounter,
    g_recordingActive ? "yes" : "no",
    g_playback.isPlaying() ? "yes" : "no",
    g_playback.currentFilename(),
    g_playback.framesPlayed(),
    g_artNet.isReceiving() ? "yes" : "no"
  );
  g_tcp.sendResponse(buf);
}

// ========================  REBOOT  ========================================

void rebootTeensy()
{
  // Teensy 4.1 system reset via ARM SCB_AIRCR register
  __disable_irq();
  SCB_AIRCR = 0x05FA0004;
  while (1);  // wait for reset
}