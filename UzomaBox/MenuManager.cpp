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
// g_recordingActive removed — use g_playback.isRecording() instead
extern uint8_t            g_recStartMode;
extern uint8_t            g_recStopMode;
extern uint16_t           g_recTrigUniv;
extern uint16_t           g_recTrigCh;
extern uint32_t           g_frameCounter;
extern uint32_t           g_fpsDisplay;
extern void setMode(OperatingMode newMode);
extern void rebootTeensy();

// ========================  I18N HELPER  =====================================
// Simple inline: if language=1 returns Spanish, otherwise English.
static inline const char* _(const char *en, const char *es) {
  return (g_config.language == 1) ? es : en;
}

// ========================  STATIC TEXT TABLES  ==============================

static const char *MAIN_EN[] = {
  "Run Mode",
  "Play Files",
  "Record Cfg",
  "Settings",
  "Network",
  "Status"
};
static const char *MAIN_ES[] = {
  "Modo Run",
  "Repro Arch",
  "Grabar Cfg",
  "Ajustes",
  "Red",
  "Estado"
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
  , _redPressStartMs(0)
  , _redLongHandled(false)
  , _prevMode(0)
  , _upDownPressStartMs(0)
  , _upDownRepeatActive(false)
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
  pinMode(PIN_BTN_RED,    INPUT_PULLUP);
  pinMode(PIN_BTN_UP,     INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN,   INPUT_PULLUP);
  pinMode(PIN_BTN_SELECT, INPUT_PULLUP);
  pinMode(PIN_BTN_BACK,   INPUT_PULLUP);

  // Attach Bounce instances
  _btnRed.attach(PIN_BTN_RED,    INPUT_PULLUP);
  _btnUp.attach(PIN_BTN_UP,     INPUT_PULLUP);
  _btnDown.attach(PIN_BTN_DOWN, INPUT_PULLUP);
  _btnSelect.attach(PIN_BTN_SELECT, INPUT_PULLUP);
  _btnBack.attach(PIN_BTN_BACK, INPUT_PULLUP);

  // 30 ms debounce
  _btnRed.interval(30);
  _btnUp.interval(30);
  _btnDown.interval(30);
  _btnSelect.interval(30);
  _btnBack.interval(30);

  // Initialise I2C and OLED display (pins defined in Pins.h)
  Wire.begin();
  _displayAvailable = _display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!_displayAvailable) {
    Serial.println("OLED not available — menu disabled");
    return;
  }
  _display.setTextWrap(false);
  _display.clearDisplay();
  _display.display();

  // Force display ON — sometimes SSD1306 enters sleep after long delays
  _display.ssd1306_command(SSD1306_DISPLAYON);

  _dirty = true;
  _lastActivityMs = millis();

  Serial.println("MenuManager: OLED + buttons ready");
}

// ========================  UPDATE (call every loop)  ========================

void MenuManager::update()
{
  if (!_displayAvailable) return;

  // ---- Read button events (Bounce-based) ----
  ButtonEvent ev = _readButtons();

  // ---- Botón rojo: control de modo test ----
  _btnRed.update();
  bool redPressed = (digitalRead(PIN_BTN_RED) == LOW);

  if (g_mode != MODE_TEST) {
    // No estamos en TEST: long-press 3s → entrar TEST
    if (redPressed) {
      if (_redPressStartMs == 0) {
        _redPressStartMs = millis();
        _redLongHandled = false;
      } else if (!_redLongHandled && (millis() - _redPressStartMs >= 3000)) {
        _redLongHandled = true;
        _prevMode = (uint8_t)g_mode;
        setMode(MODE_TEST);
        extern uint8_t g_testPattern;
        extern uint32_t g_testStartMs;
        g_testPattern = 1;  // Color Fade
        g_testStartMs = millis();
        showStatusBrief("Test mode");
        _setScreen(SCREEN_HOME, 0);
        _dirty = true;
      }
    } else {
      _redPressStartMs = 0;
    }
  } else {
    // Estamos en TEST
    if (redPressed) {
      if (_redPressStartMs == 0) {
        _redPressStartMs = millis();
        _redLongHandled = false;
      } else if (!_redLongHandled && (millis() - _redPressStartMs >= 3000)) {
        // Long-press 3s en TEST → salir, restaurar modo anterior
        _redLongHandled = true;
        OperatingMode prev = (OperatingMode)_prevMode;
        if (prev == MODE_TEST) prev = MODE_ARTNET;  // safety fallback
        setMode(prev);
        showStatusBrief("Test off");
        _setScreen(SCREEN_HOME, 0);
        _dirty = true;
      }
    } else {
      // Botón liberado: si fue < 3s → toggle pattern
      if (_redPressStartMs != 0 && !_redLongHandled) {
        extern uint8_t g_testPattern;
        extern uint32_t g_testStartMs;
        g_testPattern = (g_testPattern == 0) ? 1 : 0;
        g_testStartMs = millis();
        showStatusBrief(g_testPattern == 0 ? "RGBW" : "Fade");
        _dirty = true;
      }
      _redPressStartMs = 0;
    }
  }

  // ---- Auto-repeat in value editor when button held ----
  if (_screen == SCREEN_EDIT_VALUE) {
    bool upPressed = (digitalRead(PIN_BTN_UP) == LOW);
    bool downPressed = (digitalRead(PIN_BTN_DOWN) == LOW);
    if (upPressed || downPressed) {
      if (_upDownPressStartMs == 0) {
        _upDownPressStartMs = millis();
        _upDownRepeatActive = false;
      } else if (!_upDownRepeatActive && (millis() - _upDownPressStartMs > 500)) {
        _upDownRepeatActive = true;
      }
      if (_upDownRepeatActive) {
        if (upPressed) {
          _editValue += _editStep * 10;
          if (_editValue > _editMax) _editValue = _editMax;
          _dirty = true;
        }
        if (downPressed) {
          _editValue -= _editStep * 10;
          if (_editValue < _editMin) _editValue = _editMin;
          _dirty = true;
        }
      }
    } else {
      _upDownPressStartMs = 0;
      _upDownRepeatActive = false;
    }
  }

  // ---- Toggle test pattern when OK pressed in Test mode on HOME ----
  if (g_mode == MODE_TEST && ev == BTN_OK && _screen == SCREEN_HOME) {
    extern uint8_t g_testPattern;
    extern uint32_t g_testStartMs;
    g_testPattern = (g_testPattern == 0) ? 1 : 0;
    g_testStartMs = millis();
    showStatusBrief(g_testPattern == 0 ? "RGBW" : "Fade");
    _dirty = true;
    ev = BTN_NONE;  // consume event, don't open menu
  }

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
      if (ev == BTN_OK) {
        _returnToMain();
      } else if (ev == BTN_UP || ev == BTN_DOWN) {
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
          showStatusBrief(g_config.language ? MAIN_ES[_cursor] : MAIN_EN[_cursor]);
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
              strncpy(g_config.lastPlayFile, names[_cursor], sizeof(g_config.lastPlayFile) - 1);
              g_config.lastPlayFile[sizeof(g_config.lastPlayFile) - 1] = 0;
              saveConfig(g_config);
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
          if (g_playback.isRecording()) {
            g_playback.stopRecording();
            showStatusBrief("Recording stop");
          } else {
            if (g_playback.startRecording()) {
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
          showStatusBrief("Stopped");
          _setScreen(SCREEN_HOME, 0);
        }
      } else if (ev == BTN_BACK) {
        _returnToMain();
      }
      break;

    // ==================== RECORD TRIGGER ====================
    case SCREEN_REC_TRIGGER:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 3) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        switch (_cursor) {
          case 0: // Start mode: cycle 0→1→2→0
            g_recStartMode = (g_recStartMode + 1) % 3;
            _dirty = true;
            break;
          case 1: // Stop mode: cycle 0→1→2→0
            g_recStopMode = (g_recStopMode + 1) % 3;
            _dirty = true;
            break;
          case 2: // Trigger universe
            _editValue = g_recTrigUniv;
            _editMin = 0; _editMax = 32767; _editStep = 1;
            strcpy(_editLabel, "Trigger Univ");
            _idleOverride = true;
            _setScreen(SCREEN_EDIT_VALUE, 0);
            break;
          case 3: // Trigger channel
            _editValue = g_recTrigCh;
            _editMin = 0; _editMax = 511; _editStep = 1;
            strcpy(_editLabel, "Trigger Ch");
            _idleOverride = true;
            _setScreen(SCREEN_EDIT_VALUE, 0);
            break;
        }
      } else if (ev == BTN_BACK) {
        _setScreen(SCREEN_RECORD, 2);
      }
      break;

    // ==================== SETTINGS ====================
    case SCREEN_SETTINGS:
      if (ev == BTN_UP) {
        if (_cursor > 0) { _cursor--; _dirty = true; }
      } else if (ev == BTN_DOWN) {
        if (_cursor < 4) { _cursor++; _dirty = true; }
      } else if (ev == BTN_OK) {
        if (_cursor == 0) {  // Color order
          _setScreen(SCREEN_COLOR_ORDER, g_config.colorOrder);
        } else if (_cursor == 1) {  // LEDs per strip
          _editValue = g_config.ledWidth;
          _editMin = 1;
          _editMax = MAX_LEDS_PER_STRIP;
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
        } else if (_cursor == 4) {  // Language toggle
          g_config.language = !g_config.language;
          saveConfig(g_config);
          _dirty = true;
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
  } else if (strcmp(_editLabel, "Trigger Univ") == 0) {
    g_recTrigUniv = (uint16_t)_editValue;
    showStatusBrief("Univ saved");
  } else if (strcmp(_editLabel, "Trigger Ch") == 0) {
    g_recTrigCh = (uint16_t)_editValue;
    showStatusBrief("Ch saved");
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
  if (!_displayAvailable) return;

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

  // Mode icon — small geometric play triangle (5x5px)
  if (g_mode == MODE_RECORD && g_playback.isRecording()) {
    _display.fillCircle(4, 3, 2, SSD1306_WHITE);   // ● when recording
  } else {
    _display.fillTriangle(1, 1, 1, 6, 5, 3, SSD1306_WHITE); // ▶ for any active mode
  }
  _display.setCursor(8, 0);
  _display.print(" ");

  // Nickname (truncated to ~10 chars)
  char buf[12];
  strncpy(buf, g_config.nickname, 11);
  buf[11] = 0;
  _display.print(buf);

  // FPS on right
  _display.setCursor(128 - 40, 0);
  _display.print(g_fpsDisplay);
  _display.print("fps");
}

// ========================  DRAW: HOME (STATUS)  =============================

void MenuManager::_drawHome()
{
  _display.setTextSize(1);
  _display.setTextColor(SSD1306_WHITE);

  // Mode line with ▶ icon before mode name
  _display.setCursor(0, 0);
  _display.print("Mode: ");
  int16_t cx = 42;  // fixed X after "Mode: " (6 chars * 6px + 6px margin)
  _display.fillTriangle(cx, 1, cx, 7, cx+5, 4, SSD1306_WHITE);
  _display.setCursor(cx + 8, 0);
  _display.print(" ");
  switch (g_mode) {
    case MODE_ARTNET:   _display.print("ArtNet"); break;
    case MODE_PLAYBACK: _display.print("Playback"); break;
    case MODE_RECORD:   _display.print("Record"); break;
    case MODE_TEST:     _display.print("Test"); break;
  }
  // ● recording indicator instead of [REC]
  if (g_playback.isRecording()) {
    _display.fillCircle(116, 4, 3, SSD1306_WHITE);
  }

  // IP
  _display.setCursor(0, 12);
  _display.print("IP: ");
  _display.print(g_config.ip[0]); _display.print(".");
  _display.print(g_config.ip[1]); _display.print(".");
  _display.print(g_config.ip[2]); _display.print(".");
  _display.print(g_config.ip[3]);

  // FPS
  _display.setCursor(0, 24);
  _display.print("FPS: ");
  _display.print(g_fpsDisplay);

  // Current file (if playing)
  if (g_playback.isPlaying()) {
    _display.setCursor(0, 36);
    _display.print("File: ");
    _display.print(g_playback.currentFilename());

    // Progress bar (thin 4px) using file position / file size
    uint32_t pos  = g_playback.filePosition();
    uint32_t size = g_playback.fileSize();
    if (size > 0) {
      int16_t barW = (int16_t)((uint64_t)pos * 120 / size);
      _display.drawRect(2, 48, 124, 4, SSD1306_WHITE);
      if (barW > 0) {
        _display.fillRect(3, 49, barW, 2, SSD1306_WHITE);
      }
    }
  }

  // Hint at bottom
  _display.setCursor(0, 56);
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

    _display.print(g_config.language ? MAIN_ES[i] : MAIN_EN[i]);
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
  // Custom drawer: each item has a ▶ icon because these are action modes
  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("SELECT MODE");
  _display.setTextColor(SSD1306_WHITE);

  int8_t y = 11;
  for (int8_t i = 0; i < MODE_COUNT; i++) {
    if (i == _cursor) {
      _display.fillRect(0, y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
      // Draw play icon in black on white background
      _display.fillTriangle(2, y+1, 2, y+7, 8, y+4, SSD1306_BLACK);
      _display.setCursor(11, y);
    } else {
      _display.setTextColor(SSD1306_WHITE);
      // Draw play icon white on black
      _drawIcon(2, y, 'p');
      _display.setCursor(11, y);
    }
    _display.print(MODE_ITEMS[i]);
    y += 9;
  }
}

// ========================  DRAW: PLAYBACK  ==================================

void MenuManager::_drawPlaybackScreen()
{
  static const char *items[] = {
    "Play Now",
    "Browse Files",
    "Set Speed",
    "Stop"
  };
  static const char icons[] = { 'p', 0, 0, 's' };
  #define PB_COUNT 4

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("PLAY FILES");
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
    // Draw icon if applicable
    if (icons[i]) {
      if (i == _cursor) {
        // Inverted: draw icon in black
        _display.fillTriangle(2, y+1, 2, y+7, 8, y+4, SSD1306_BLACK);
      } else {
        _drawIcon(2, y, icons[i]);
      }
      _display.setCursor(11, y);
    } else {
      _display.setCursor(2, y);
    }
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
    "Record",
    "Set FPS",
    "Triggers",
    "Stop"
  };
  static const char icons[] = { 'r', 0, 0, 's' };
  #define REC_COUNT 4

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print("RECORD CFG");
  if (g_playback.isRecording()) {
    _display.setCursor(90, 0);
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
    // Draw icon if applicable
    if (icons[i]) {
      if (i == _cursor) {
        // Inverted: draw icon in black
        if (icons[i] == 'r') {
          _display.fillCircle(5, y+4, 3, SSD1306_BLACK);
        } else {
          _display.fillRect(2, y+2, 5, 5, SSD1306_BLACK);
        }
      } else {
        _drawIcon(2, y, icons[i]);
      }
      _display.setCursor(11, y);
    } else {
      _display.setCursor(2, y);
    }
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

  static const char *startLabels[] = { "Immediate", "Non-zero", "Channel" };
  static const char *stopLabels[]  = { "Manual",    "All zero", "Timer" };

  struct TrigItem { int8_t y; const char *label; const char *value; };
  TrigItem items[4];
  items[0] = { 14, "Start", startLabels[g_recStartMode] };
  items[1] = { 24, "Stop",  stopLabels[g_recStopMode]  };

  char univStr[8], chStr[8];
  snprintf(univStr, sizeof(univStr), "%u", g_recTrigUniv);
  snprintf(chStr,   sizeof(chStr),   "%u", g_recTrigCh);
  items[2] = { 34, "Univ", univStr };
  items[3] = { 44, "Ch",   chStr   };

  for (int8_t i = 0; i < 4; i++) {
    if (i == _cursor) {
      _display.fillRect(0, items[i].y, 128, 9, SSD1306_WHITE);
      _display.setTextColor(SSD1306_BLACK);
    } else {
      _display.setTextColor(SSD1306_WHITE);
    }
    _display.setCursor(2, items[i].y);
    _display.print(i == _cursor ? ">" : " ");
    _display.print(items[i].label);
    _display.print(": ");
    _display.print(items[i].value);
  }

  _display.setCursor(2, 56);
  _display.print("BACK=exit");
}

// ========================  DRAW: SETTINGS  ==================================

void MenuManager::_drawSettings()
{
  static const char *itemsEN[] = {
    "Color Order", "LEDs/strip", "Playback Spd", "Record FPS", "Language"
  };
  static const char *itemsES[] = {
    "Orden Color", "LEDs/tira", "Veloc. Reprod", "FPS Grab", "Idioma"
  };
  #define SET_COUNT 5

  _display.setTextSize(1);

  _display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _display.setTextColor(SSD1306_BLACK);
  _display.setCursor(2, 0);
  _display.print(_("SETTINGS", "AJUSTES"));
  _display.setTextColor(SSD1306_WHITE);

  const char *const *items = g_config.language ? itemsES : itemsEN;
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
    if (i == 4) {
      // Show current language value
      _display.print(g_config.language ? "Espa" "\xF1" "ol" : "English");
    } else {
      _display.print(items[i]);
    }
    y += 9;
  }

  // Show current values
  y = 14 + SET_COUNT * 9;
  _display.setCursor(2, y);
  _display.print(_("Color:", "Color:"));
  _display.print(" ");
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

// ========================  ICON DRAWING  ====================================

void MenuManager::_drawIcon(uint8_t x, uint8_t y, char type)
{
  // Draws a 6x6 px geometric icon within a 9px-tall row.
  //   'p' = play (triangle ▶)     's' = stop (square ■)
  //   'r' = record (circle ●)
  switch (type) {
    case 'p': // ▶ play triangle
      _display.fillTriangle(x, y+1, x, y+7, x+6, y+4, SSD1306_WHITE);
      break;
    case 's': // ■ stop square
      _display.fillRect(x, y+2, 5, 5, SSD1306_WHITE);
      break;
    case 'r': // ● record circle
      _display.fillCircle(x+3, y+4, 3, SSD1306_WHITE);
      break;
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
