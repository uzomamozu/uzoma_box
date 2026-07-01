#include "MenuManager.h"
#include "Config.h"
#include "SDManager.h"
#include "LEDController.h"
#include "ArtNetHandler.h"
#include "PlaybackController.h"

// ========================  EXTERNAL GLOBALS  ================================
// These are defined in UzomaBox.ino — we reference them here so menu actions
// call the same code paths as the TCP command handler.
//
// Usage pattern:  MenuItem → menu code → same function TCPHandler would call
// Examples:
//   setMode(MODE_ARTNET)       same as TCP "MODE:artnet"
//   g_playback.playFile("x")   same as TCP "PLAY:x"
//   g_playback.startRecording() same as TCP "REC:START"

extern AppConfig          g_config;
extern LEDController      g_leds;
extern ArtNetHandler      g_artNet;
extern PlaybackController g_playback;
extern OperatingMode      g_mode;
extern bool               g_recordingActive;
extern uint8_t            g_recStartMode;
extern uint8_t            g_recStopMode;
extern uint16_t           g_recTrigUniv;
extern uint16_t           g_recTrigCh;
extern uint32_t           g_frameCounter;
extern uint32_t           g_fpsFrames;
extern void setMode(OperatingMode newMode);
extern void rebootTeensy();

// ========================  STATIC TEXT TABLES  ==============================

static const char *MAIN_ITEMS[] = {
  "Mode",
  "Playback",
  "Record",
  "Settings",
  "Network",
  "Status"
};
#define MAIN_COUNT  6

static const char *MODE_ITEMS[] = {
  "ArtNet",
  "Playback",
  "Record",
  "Test"
};
#define MODE_COUNT  4

static const char *COLOR_ITEMS[] = {
  "RGB",
  "RBG",
  "GRB",
  "GBR",
  "BRG",
  "BGR"
};
#define COLOR_COUNT  6

// ========================  CONSTRUCTOR / DESTRUCTOR  ========================

MenuManager::MenuManager()
  : _display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET)
  , _screen(SCREEN_HOME)
  , _cursor(0)
  , _scrollOffset(0)
  , _editValue(0)
  , _editMin(0)
  , _editMax(0)
  , _editStep(1)
  , _editTarget(nullptr)
  , _editOctet(0)
  , _ipCursor(0)
  , _confirmPrompt(nullptr)
  , _confirmCallback(nullptr)
  , _dirty(true)
  , _lastActivityMs(0)
  , _idleOverride(false)
  , _briefActive(false)
{
  _editLabel[0] = 0;
  _briefMsg[0] = 0;
}

MenuManager::~MenuManager()
{
}

// ========================  BEGIN  ===========================================

void MenuManager::begin()
{
  // Initialise button pins with internal pull-up
  pinMode(PIN_BTN_UP,     INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN,   INPUT_PULLUP);
  pinMode(PIN_BTN_SELECT, INPUT_PULLUP);
  pinMode(PIN_BTN_BACK,   INPUT_PULLUP);

  // Attach Bounce instances
  _btnUp.attach(PIN_BTN_UP,     INPUT_PULLUP);
  _btnDown.attach(PIN_BTN_DOWN, INPUT_PULLUP);
  _btnSelect.attach(PIN_BTN_SELECT, INPUT_PULLUP);
  _btnBack.attach(PIN_BTN_BACK, INPUT_PULLUP);

  // 30 ms debounce
  _btnUp.interval(30);
  _btnDown.interval(30);
  _btnSelect.interval(30);
  _btnBack.interval(30);

  // Initialise I2C and OLED display
  Wire.begin();
  if (!_display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed (0x3C)");
    // Not fatal — device continues without display
  }
  _display.setTextWrap(false);
  _display.clearDisplay();
  _display.display();
  _dirty = true;
  _lastActivityMs = millis();

  Serial.println("MenuManager: OLED + buttons ready");
}

// ========================  UPDATE (call every loop)  ========================

void MenuManager::update()
{
  ButtonEvent ev = _readButtons();

  if (ev != BTN_NONE) {
    _lastActivityMs = millis();
    _handleEvent(ev);
  }

  // Auto-return to home after 15s of inactivity (unless editing)
  if (!_idleOverride && _screen != SCREEN_HOME &&
      (millis() - _lastActivityMs > 15000)) {
    _setScreen(SCREEN_HOME, 0);
  }

  // Brief message auto-clear after 1.5s
  if (_briefActive && (millis() - _briefStartMs > 1500)) {
    _briefActive = false;
    _dirty = true;
  }

  // Re-draw home screen every 500ms to update FPS / uptime
  if (_screen == SCREEN_HOME && (millis() - _lastActivityMs) % 500 < 20) {
    _dirty = true;
  }

  if (_dirty) {
    _render();
    _dirty = false;
  }
}

// ========================  PUBLIC HELPERS  ==================================

void MenuManager::forceHome()
{
  _setScreen(SCREEN_HOME, 0);
}

void MenuManager::showStatusBrief(const char *msg)
{
  strncpy(_briefMsg, msg, sizeof(_briefMsg) - 1);
  _briefMsg[sizeof(_briefMsg) - 1] = 0;
  _briefActive = true;
  _briefStartMs = millis();
  _dirty = true;
}

// ========================  BUTTON READING  ==================================

MenuManager::ButtonEvent MenuManager::_readButtons()
{
  _btnUp.update();
  _btnDown.update();
  _btnSelect.update();
  _btnBack.update();

  // Prioritise: OK > BACK > UP > DOWN (only one event per call)
  if (_btnSelect.fell()) return BTN_OK;
  if (_btnBack.fell())   return BTN_BACK;
  if (_btnUp.fell())     return BTN_UP;
  if (_btnDown.fell())   return BTN_DOWN;

  return BTN_NONE;
}

// ========================  SCREEN SET / RETURN  =============================

void MenuManager::_setScreen(MenuScreen s, int8_t cursor)
{
  _screen = s;
  _cursor = cursor;
  _scrollOffset = 0;
  _idleOverride = false;
  _briefActive = false;
  _dirty = true;
}

void MenuManager::_returnToMain()
{
  _setScreen(SCREEN_MAIN, 0);
}

// ========================  EVENT HANDLER  ===================================

void MenuManager::_handleEvent(ButtonEvent ev)
{
  // If brief message showing, dismiss it and ignore event
  if (_briefActive) {
    _briefActive = false;
    _dirty = true;
    return;
  }

  switch (_screen) {

    // ==================== HOME ====================
    case SCREEN_HOME:
      if (ev == BTN_OK || ev == BTN_UP || ev == BTN_DOWN) {
        _returnToMain();
      }
      break;

    // ==================== MAIN MENU ====================
    case SCREEN_MAIN:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < MAIN_COUNT - 1) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        switch (_cursor) {
          case 0: _setScreen(SCREEN_MODE, g_mode); break;
          case 1: _setScreen(SCREEN_PLAYBACK, 0); break;
          case 2: _setScreen(SCREEN_RECORD, 0); break;
          case 3: _setScreen(SCREEN_SETTINGS, 0); break;
          case 4: _setScreen(SCREEN_NETWORK, 0); break;
          case 5: _setScreen(SCREEN_HOME, 0); _lastActivityMs = 0; break;  // show status
        }
      } else if (ev == BTN_BACK) {
        _setScreen(SCREEN_HOME, 0);
      }
      break;

    // ==================== MODE SELECT ====================
    case SCREEN_MODE:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < MODE_COUNT - 1) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        OperatingMode modes[] = { MODE_ARTNET, MODE_PLAYBACK, MODE_RECORD, MODE_TEST };
        if (_cursor >= 0 && _cursor < 4) {
          setMode(modes[_cursor]);
          showStatusBrief(MAIN_ITEMS[_cursor]);
          _setScreen(SCREEN_HOME, 0);
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== PLAYBACK ====================
    case SCREEN_PLAYBACK:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 3) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        if (_cursor == 0) {  // Play Sequence
          if (g_playback.playSequence() > 0) {
            g_mode = MODE_PLAYBACK;
            showStatusBrief("Playing seq");
          } else {
            showStatusBrief("No .BIN files");
          }
          _setScreen(SCREEN_HOME, 0);
        } else if (_cursor == 1) {  // List files
          _setScreen(SCREEN_FILELIST, 0);
        } else if (_cursor == 2) {  // Speed
          _editValue = (int32_t)(g_playback.getSpeed() * 100.0f);
          _editMin = 5;    // 0.05
          _editMax = 500;  // 5.00
          _editStep = 5;   // 0.05 steps
          strcpy(_editLabel, "Speed (x0.01)");
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_VALUE, 0);
        } else if (_cursor == 3) {  // Stop
          g_playback.stop();
          g_leds.clear();
          g_leds.show();
          showStatusBrief("Stopped");
          _setScreen(SCREEN_HOME, 0);
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== FILE LIST ====================
    case SCREEN_FILELIST:
      {
        char names[64][16];
        int count = sdListBinFiles(names, 64);
        if (ev == BTN_UP) {
          if (_cursor > 0) { _cursor--; _dirty = true; }
        } else if (ev == BTN_DOWN) {
          if (_cursor < count - 1) { _cursor++; _dirty = true; }
        } else if (ev == BTN_OK) {
          if (count > 0 && _cursor >= 0 && _cursor < count) {
            if (g_playback.playFile(names[_cursor])) {
              g_mode = MODE_PLAYBACK;
              showStatusBrief("Now playing");
            } else {
              showStatusBrief("Cannot open");
            }
            _setScreen(SCREEN_HOME, 0);
          }
        } else if (ev == BTN_BACK) {
          _setScreen(SCREEN_PLAYBACK, 1);
        }
      }
      break;

    // ==================== RECORD ====================
    case SCREEN_RECORD:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 3) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        if (_cursor == 0) {  // Start / Stop recording
          if (g_recordingActive) {
            g_playback.stopRecording();
            g_recordingActive = false;
            showStatusBrief("Recording stop");
          } else {
            if (g_playback.startRecording()) {
              g_recordingActive = true;
              showStatusBrief("Recording...");
            } else {
              showStatusBrief("Record failed");
            }
          }
          _setScreen(SCREEN_HOME, 0);
        } else if (_cursor == 1) {  // Set record FPS
          _editValue = g_config.recordFps;
          _editMin = 5;
          _editMax = 60;
          _editStep = 1;
          strcpy(_editLabel, "Record FPS");
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_VALUE, 0);
        } else if (_cursor == 2) {  // Trigger settings
          _setScreen(SCREEN_REC_TRIGGER, 0);
        } else if (_cursor == 3) {  // Stop current (alias for safety)
          g_playback.stopRecording();
          g_recordingActive = false;
          showStatusBrief("Stopped");
          _setScreen(SCREEN_HOME, 0);
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== RECORD TRIGGER ====================
    case SCREEN_REC_TRIGGER:
      if (ev == BTN_BACK) {
        _setScreen(SCREEN_RECORD, 2);
      }
      // Simplified — just show current trigger config, BACK exits
      break;

    // ==================== SETTINGS ====================
    case SCREEN_SETTINGS:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 3) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        if (_cursor == 0) {  // Color order
          _setScreen(SCREEN_COLOR_ORDER, g_config.colorOrder);
        } else if (_cursor == 1) {  // LEDs per strip
          _editValue = g_config.ledWidth;
          _editMin = 1;
          _editMax = 512;
          _editStep = 1;
          strcpy(_editLabel, "LEDs/strip");
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_VALUE, 0);
        } else if (_cursor == 2) {  // Playback speed (alias)
          _editValue = (int32_t)(g_config.playbackSpeed * 100.0f);
          _editMin = 5;
          _editMax = 500;
          _editStep = 5;
          strcpy(_editLabel, "Speed (x0.01)");
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_VALUE, 0);
        } else if (_cursor == 3) {  // Record FPS (alias)
          _editValue = g_config.recordFps;
          _editMin = 5;
          _editMax = 60;
          _editStep = 1;
          strcpy(_editLabel, "Record FPS");
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_VALUE, 0);
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== COLOR ORDER ====================
    case SCREEN_COLOR_ORDER:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < COLOR_COUNT - 1) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        g_config.colorOrder = (ColorOrder)_cursor;
        g_leds.setColorOrder(g_config.colorOrder);
        saveConfig(g_config);
        showStatusBrief("Color set");
        _setScreen(SCREEN_HOME, 0);
      } else if (ev == BTN_BACK) {
        _setScreen(SCREEN_SETTINGS, 0);
      }
      break;

    // ==================== NETWORK ====================
    case SCREEN_NETWORK:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 2) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        if (_cursor == 0) {  // Edit IP
          _ipOctets[0] = g_config.ip[0];
          _ipOctets[1] = g_config.ip[1];
          _ipOctets[2] = g_config.ip[2];
          _ipOctets[3] = g_config.ip[3];
          _ipCursor = 0;
          _idleOverride = true;
          _setScreen(SCREEN_EDIT_IP, 0);
        } else if (_cursor == 1) {  // View MAC + save & reboot
          _setScreen(SCREEN_HOME, 0);
          _rebootApply();
        } else if (_cursor == 2) {  // Back
          _returnToMain();
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== EDIT IP ====================
    case SCREEN_EDIT_IP:
      if (ev == BTN_UP) {
        if (_ipOctets[_ipCursor] < 255) _ipOctets[_ipCursor]++;
        _dirty = true;
      } else if (ev == BTN_DOWN) {
        if (_ipOctets[_ipCursor] > 0) _ipOctets[_ipCursor]--;
        _dirty = true;
      } else if (ev == BTN_OK) {
        if (_ipCursor < 3) {
          _ipCursor++;
        } else {
          // Save IP and reboot
          g_config.ip = IPAddress(_ipOctets[0], _ipOctets[1],
                                  _ipOctets[2], _ipOctets[3]);
          saveConfig(g_config);
          showStatusBrief("Rebooting...");
          delay(100);
          rebootTeensy();
        }
        _dirty = true;
      } else if (ev == BTN_BACK) {
        _setScreen(SCREEN_NETWORK, 0);
      }
      break;

    // ==================== EDIT VALUE ====================
    case SCREEN_EDIT_VALUE:
      if (ev == BTN_UP) {
        _editValue += _editStep;
        if (_editValue > _editMax) _editValue = _editMax;
        _dirty = true;
      } else if (ev == BTN_DOWN) {
        _editValue -= _editStep;
        if (_editValue < _editMin) _editValue = _editMin;
        _dirty = true;
      } else if (ev == BTN_OK) {
        _saveEditValue();
        _setScreen(SCREEN_HOME, 0);
      } else if (ev == BTN_BACK) {
        // Discard changes
        _setScreen(SCREEN_HOME, 0);
      }
      break;

    // ==================== CONFIRM ====================
    case SCREEN_CONFIRM:
      if (ev == BTN_UP || ev == BTN_DOWN) {
        _cursor = (_cursor == 0) ? 1 : 0;
        _dirty = true;
      } else if (ev == BTN_OK) {
        if (_confirmCallback) {
          _confirmCallback(_cursor == 0);
        }
        _setScreen(SCREEN_HOME, 0);
      } else if (ev == BTN_BACK) {
        if (_confirmCallback) {
          _confirmCallback(false);
        }
        _setScreen(SCREEN_HOME, 0);
      }
      break;

    // ==================== DEFAULT / UNUSED ====================
    default:
      break;
  }
}

// ========================  SAVE EDIT VALUE  ================================

void MenuManager::_saveEditValue()
{
  // Determine which variable to update based on the edit label
  if (strcmp(_editLabel, "Speed (x0.01)") == 0) {
    float speed = _editValue / 100.0f;
    if (speed < 0.05f) speed = 0.05f;
    if (speed > 5.0f)  speed = 5.0f;
    g_playback.setSpeed(speed);
    g_config.playbackSpeed = speed;
    saveConfig(g_config);
    showStatusBrief("Speed saved");
  } else if (strcmp(_editLabel, "Record FPS") == 0) {
    g_config.recordFps = (uint16_t)_editValue;
    saveConfig(g_config);
    showStatusBrief("FPS saved");
  } else if (strcmp(_editLabel, "LEDs/strip") == 0) {
    g_config.ledWidth = (uint16_t)_editValue;
    saveConfig(g_config);
    showStatusBrief("Reboot to apply");
  }
}

// ========================  REBOOT  ==========================================

void MenuManager::_rebootApply()
{
  saveConfig(g_config);
  showStatusBrief("Rebooting...");
  delay(100);
  rebootTeensy();
}

// ========================  RENDERING  =======================================

void MenuManager::_render()
{
  _display.clearDisplay();

  switch (_screen) {
    case SCREEN_HOME:        _drawHome();           break;
    case SCREEN_MAIN:        _drawMainMenu();       break;
    case SCREEN_MODE:        _drawModeScreen();     break;
    case SCREEN_PLAYBACK:    _drawPlaybackScreen(); break;
    case SCREEN_FILELIST:    _drawFileList();       break;
    case SCREEN_RECORD:      _drawRecordScreen();   break;
    case SCREEN_REC_TRIGGER: _drawRecordTrigger();  break;
    case SCREEN_SETTINGS:    _drawSettings();       break;
    case SCREEN_COLOR_ORDER: _drawColorOrder();     break;
    case SCREEN_NETWORK:     _drawNetwork();        break;
    case SCREEN_EDIT_IP:     _drawEditIP();         break;
    case SCREEN_EDIT_VALUE:  _drawEditValue();      break;
    case SCREEN_CONFIRM:     _drawConfirm();        break;
    default:                 _drawHome();           break;
  }

  // Brief overlay message
  if (_briefActive) {
    _display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 56);
    _display.print(_briefMsg);
  }

  _display.display();
}

// ========================  DRAW: STATUS BAR  ================================

void MenuManager::_drawStatusBar()
{
  // 8-pixel tall bar at top with mode indicator and nickname
  _display.setTextSize(1);
  _display.setTextColor(SSD1306_WHITE);

  // Mode icon
  const char *icon = "?";
  switch (g_mode) {
    case MODE_ARTNET:   icon = "A";  break;
    case MODE_PLAYBACK: icon = "P";  break;
    case MODE_RECORD:   icon = "R";  break;
    case MODE_TEST:     icon = "T";  break;
  }
  _display.setCursor(0, 0);
  _display.print(icon);
  _display.print(" ");

  // Nickname (truncated to ~10 chars)
  char buf[12];
  strncpy(buf, g_config.nickname, 11);
  buf[11] = 0;
  _display.print(buf);

  // FPS on right
  _display.setCursor(128 - 40, 0);
  _display.print(g_fpsFrames);
  _display.print("fps");
}

// ========================  DRAW: HOME (STATUS)  =============================

void MenuManager::_drawHome()
{
  _display.setTextSize(1);
  _display.setTextColor(SSD1306_WHITE);

  // Top bar
  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("UZOMABOX v");
  _display.print(FW_VERSION);
  _display.setTextColor(SSD1306_WHITE);

  // Mode line
  _display.setCursor(0, 12);
  _display.print("Mode: ");
  switch (g_mode) {
    case MODE_ARTNET:   _display.print("ArtNet"); break;
    case MODE_PLAYBACK: _display.print("Playback"); break;
    case MODE_RECORD:   _display.print("Record"); break;
    case MODE_TEST:     _display.print("Test"); break;
  }
  if (g_recordingActive) {
    _display.print(" [REC]");
  }

  // IP
  _display.setCursor(0, 22);
  _display.print("IP: ");
  _display.print(g_config.ip[0]); _display.print(".");
  _display.print(g_config.ip[1]); _display.print(".");
  _display.print(g_config.ip[2]); _display.print(".");
  _display.print(g_config.ip[3]);

  // FPS
  _display.setCursor(0, 32);
  _display.print("FPS: ");
  _display.print(g_fpsFrames);

  // Current file (if playing)
  if (g_playback.isPlaying()) {
    _display.setCursor(0, 42);
    _display.print("File: ");
    _display.print(g_playback.currentFilename());
  }

  // Uptime (simplified — show seconds since last activity reset)
  uint32_t secs = millis() / 1000;
  _display.setCursor(0, 52);
  _display.print("Up: ");
  _display.print(secs / 3600); _display.print("h");
  _display.print((secs % 3600) / 60); _display.print("m");
  _display.print(secs % 60); _display.print("s");

  // Hint at bottom
  _display.setCursor(0, 62);
  _display.print("OK=menu");
}

// ========================  DRAW: MAIN MENU  =================================

void MenuManager::_drawMainMenu()
{
  _drawStatusBar();

  _display.setTextSize(1);
  _display.setTextColor(SSD1306_WHITE);

  int8_t y = 10;
  for (int8_t i = 0; i < MAIN_COUNT; i++) {
    if (i == _cursor) {
      // Inverted highlight
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }

    // Cursor arrow
    if (i == _cursor) {
      _display.setCursor(2, y);
      _display.print(">");
    } else {
      _display.setCursor(2, y);
      _display.print(" ");
    }

    _display.print(MAIN_ITEMS[i]);
    y += 9;
  }

  // Scroll indicator for items beyond screen bottom
  _drawScrollIndicators(MAIN_COUNT, (64 - 10) / 9);
}

// ========================  DRAW: SUB-MENU GENERIC  ==========================

void MenuManager::_drawSubMenu(const char *title, const char * const *items, int8_t count)
{
  _display.setTextSize(1);

  // Title bar
  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print(title);
  _display.setTextColor(SSD1306_WHITE);

  int8_t y = 11;
  for (int8_t i = 0; i < count; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i]);
    y += 9;
  }

  _drawScrollIndicators(count, (64 - 11) / 9);
}

// ========================  DRAW: MODE SCREEN  ===============================

void MenuManager::_drawModeScreen()
{
  _drawSubMenu("MODE", MODE_ITEMS, MODE_COUNT);
}

// ========================  DRAW: PLAYBACK  ==================================

void MenuManager::_drawPlaybackScreen()
{
  static const char *items[] = {
    "Play Sequence",
    "List Files",
    "Speed",
    "Stop"
  };
  #define PB_COUNT 4

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("PLAYBACK");
  _display.setTextColor(SSD1306_WHITE);

  // Show current speed in title bar
  char speedStr[8];
  sprintf(speedStr, "x%.2f", g_playback.getSpeed());
  _display.setCursor(80, 0);
  _display.print(speedStr);

  int8_t y = 11;
  for (int8_t i = 0; i < PB_COUNT; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i]);
    y += 9;
  }
}

// ========================  DRAW: FILE LIST  =================================

void MenuManager::_drawFileList()
{
  char names[64][16];
  int count = sdListBinFiles(names, 64);

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("FILES (");
  _display.print(count);
  _display.print(")");
  _display.setTextColor(SSD1306_WHITE);

  // Limit visible items to fit screen
  int8_t maxVisible = (64 - 11) / 9;  // ~5 items
  if (_cursor < _scrollOffset) _scrollOffset = _cursor;
  if (_cursor >= _scrollOffset + maxVisible) _scrollOffset = _cursor - maxVisible + 1;
  if (_scrollOffset < 0) _scrollOffset = 0;

  int8_t y = 11;
  for (int8_t i = _scrollOffset; i < count && i < _scrollOffset + maxVisible; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(names[i]);
    y += 9;
  }

  _drawScrollIndicators(count, maxVisible);
}

// ========================  DRAW: RECORD  ====================================

void MenuManager::_drawRecordScreen()
{
  static const char *items[] = {
    "Start/Stop",
    "Set FPS",
    "Triggers",
    "Stop (safe)"
  };
  #define REC_COUNT 4

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("RECORD");
  if (g_recordingActive) {
    _display.setCursor(80, 0);
    _display.print("[REC]");
  }
  _display.setTextColor(SSD1306_WHITE);

  int8_t y = 11;
  for (int8_t i = 0; i < REC_COUNT; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i]);
    y += 9;
  }

  // Show current FPS
  _display.setCursor(2, y + 2);
  _display.print("FPS: ");
  _display.print(g_config.recordFps);
}

// ========================  DRAW: RECORD TRIGGER  ============================

void MenuManager::_drawRecordTrigger()
{
  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("TRIGGERS");
  _display.setTextColor(SSD1306_WHITE);

  _display.setCursor(2, 14);
  _display.print("Start: ");
  _display.print(g_recStartMode == 0 ? "Immediate" :
                  g_recStartMode == 1 ? "Non-zero" : "Channel");

  _display.setCursor(2, 24);
  _display.print("Stop: ");
  _display.print(g_recStopMode == 0 ? "Manual" :
                  g_recStopMode == 1 ? "All zero" : "Timer");

  _display.setCursor(2, 34);
  _display.print("Univ: ");
  _display.print(g_recTrigUniv);

  _display.setCursor(2, 44);
  _display.print("Ch: ");
  _display.print(g_recTrigCh);

  _display.setCursor(2, 56);
  _display.print("BACK=exit");
}

// ========================  DRAW: SETTINGS  ==================================

void MenuManager::_drawSettings()
{
  static const char *items[] = {
    "Color Order",
    "LEDs/strip",
    "Playback Spd",
    "Record FPS"
  };
  #define SET_COUNT 4

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("SETTINGS");
  _display.setTextColor(SSD1306_WHITE);

  int8_t y = 11;
  for (int8_t i = 0; i < SET_COUNT; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i]);
    y += 9;
  }

  // Show current values
  y = 14 + SET_COUNT * 9;
  _display.setCursor(2, y);
  _display.print("Color: ");
  _display.print(colorOrderStr(g_config.colorOrder));
}

// ========================  DRAW: COLOR ORDER  ==============================

void MenuManager::_drawColorOrder()
{
  _drawSubMenu("COLOR ORDER", COLOR_ITEMS, COLOR_COUNT);
}

// ========================  DRAW: NETWORK  ===================================

void MenuManager::_drawNetwork()
{
  static const char *items[] = {
    "Edit IP",
    "Save & Reboot",
    "Back"
  };
  #define NET_COUNT 3

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("NETWORK");
  _display.setTextColor(SSD1306_WHITE);

  // Show current IP and MAC
  _display.setCursor(2, 10);
  _display.print("IP: ");
  _display.print(g_config.ip[0]); _display.print(".");
  _display.print(g_config.ip[1]); _display.print(".");
  _display.print(g_config.ip[2]); _display.print(".");
  _display.print(g_config.ip[3]);

  _display.setCursor(2, 20);
  _display.print("MAC: ");
  _display.printf("%02X:%02X:%02X:%02X:%02X:%02X",
    g_config.mac[0], g_config.mac[1], g_config.mac[2],
    g_config.mac[3], g_config.mac[4], g_config.mac[5]);

  // Menu items below
  int8_t y = 34;
  for (int8_t i = 0; i < NET_COUNT; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i]);
    y += 9;
  }
}

// ========================  DRAW: EDIT IP  ===================================

void MenuManager::_drawEditIP()
{
  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("EDIT IP");
  _display.setTextColor(SSD1306_WHITE);

  _display.setCursor(2, 20);
  for (int8_t i = 0; i < 4; i++) {
    if (i == _ipCursor) {
      _display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.print(_ipOctets[i]);
    _display.setTextColor(SSD1306_WHITE);
    if (i < 3) _display.print(".");
  }

  // Hint
  _display.setCursor(2, 40);
  _display.print("UP/DOWN=change");
  _display.setCursor(2, 50);
  _display.print("OK=next  BACK=done");
}

// ========================  DRAW: EDIT VALUE  ===============================

void MenuManager::_drawEditValue()
{
  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print(_editLabel);
  _display.setTextColor(SSD1306_WHITE);

  // Large value display (text size 2)
  _display.setTextSize(2);
  _display.setCursor(30, 18);
  _display.print(_editValue);

  // Bar graph showing relative position
  _display.setTextSize(1);
  int32_t range = _editMax - _editMin;
  if (range > 0) {
    int16_t barWidth = map(_editValue, _editMin, _editMax, 0, 120);
    _display.drawRect(4, 42, 120, 6, SSD1306_WHITE);
    if (barWidth > 0) {
      _display.fillRect(5, 43, barWidth, 4, SSD1306_WHITE);
    }
  }

  // Hints
  _display.setCursor(2, 54);
  _display.print("OK=save  BACK=discard");
}

// ========================  DRAW: CONFIRM  ===================================

void MenuManager::_drawConfirm()
{
  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("CONFIRM");
  _display.setTextColor(SSD1306_WHITE);

  // Prompt (truncated to 21 chars)
  _display.setCursor(2, 16);
  if (_confirmPrompt) {
    _display.print(_confirmPrompt);
  } else {
    _display.print("Are you sure?");
  }

  // Yes / No options
  int8_t y = 40;
  static const char *opts[] = { "Yes", "No" };
  for (int8_t i = 0; i < 2; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(opts[i]);
    y += 10;
  }
}

// ========================  SCROLL INDICATORS  ===============================

void MenuManager::_drawScrollIndicators(int8_t total, int8_t visible)
{
  if (total > visible) {
    // Draw up/down arrows if scrolling is possible
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    if (_cursor > 0) {
      _display.setCursor(120, 0);
      _display.print("^");
    }
    if (_cursor < total - 1) {
      _display.setCursor(120, 56);
      _display.print("v");
    }
  }
}