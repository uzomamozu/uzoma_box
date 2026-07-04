#ifndef MenuManager_h
#define MenuManager_h

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Bounce2.h>

// ---------------------------------------------------------------------------
// Screen enumeration for the state machine
// ---------------------------------------------------------------------------
enum MenuScreen {
  SCREEN_HOME,        // Live status display (idle)
  SCREEN_MAIN,        // Main menu list
  SCREEN_MODE,        // Mode selection submenu
  SCREEN_PLAYBACK,    // Playback controls
  SCREEN_FILELIST,    // List .BIN files
  SCREEN_RECORD,      // Record controls
  SCREEN_REC_TRIGGER, // Record trigger settings
  SCREEN_SETTINGS,    // Settings menu
  SCREEN_COLOR_ORDER, // Color order choice
  SCREEN_NETWORK,     // Network info / edit
  SCREEN_EDIT_IP,     // Edit IP address digits
  SCREEN_EDIT_VALUE,  // Generic value editor (speed, FPS, etc.)
  SCREEN_EDIT_BRIGHT, // Brightness slider
  SCREEN_CONFIRM      // Yes/No confirmation dialog
};

// ---------------------------------------------------------------------------
// MenuManager — non-blocking OLED menu system
// ---------------------------------------------------------------------------
class MenuManager {
public:
  MenuManager();
  ~MenuManager();

  // Initialise OLED + button pins
  void begin();

  // Call every loop() — reads buttons, renders if dirty, returns immediately
  void update();

  // Force return to the home/idle status screen
  void forceHome();

  // Show a brief message on screen for ~1.5s then return to previous screen
  void showStatusBrief(const char *msg);

  // Check if menu is currently active (not idle home)
  bool isMenuActive() const { return _screen != SCREEN_HOME; }

  // Call this from loop so screen updates when state changes externally
  void markDirty() { _dirty = true; }

private:
  // OLED display instance
  Adafruit_SSD1306 _display;

  // Bounce instances for each button
  Bounce _btnUp;
  Bounce _btnDown;
  Bounce _btnSelect;
  Bounce _btnBack;

  // Current screen and cursor position
  MenuScreen _screen;
  int8_t     _cursor;
  int8_t     _scrollOffset;   // for file lists longer than screen

  // Value editing state
  char       _editLabel[20];
  int32_t    _editValue;
  int32_t    _editMin;
  int32_t    _editMax;
  int32_t    _editStep;
  void      *_editTarget;     // pointer to variable being edited (int32_t*)

  // IP editing state
  uint8_t    _editOctet;      // which octet (0-3)
  uint8_t    _ipOctets[4];
  int8_t     _ipCursor;       // -1=done, 0-3=octet index

  // Confirm dialog state
  const char *_confirmPrompt;
  bool       (*_confirmCallback)(bool);

  // Display availability — false if OLED init failed (prevents I2C lockups)
  bool       _displayAvailable;

  // Dirty flag — only redraw when something changes
  bool       _dirty;

  // Long-press OK tracking
  uint32_t   _okPressStartMs;    // millis() when OK was pressed (0 = not pressed)
  bool       _okLongHandled;     // true once long-press was triggered

  // Idle timeout — return home after 15s inactivity
  uint32_t   _lastActivityMs;
  bool       _idleOverride;   // don't auto-return when editing

  // Brief message popup
  bool       _briefActive;
  char       _briefMsg[48];
  uint32_t   _briefStartMs;

  // ----- Internal button handling -----
  enum ButtonEvent { BTN_NONE, BTN_UP, BTN_DOWN, BTN_OK, BTN_BACK };
  ButtonEvent _readButtons();
  void        _handleEvent(ButtonEvent ev);

  // ----- Rendering -----
  void _render();
  void _drawHome();
  void _drawMainMenu();
  void _drawSubMenu(const char *title, const char * const *items, int8_t count);
  void _drawModeScreen();
  void _drawPlaybackScreen();
  void _drawFileList();
  void _drawRecordScreen();
  void _drawRecordTrigger();
  void _drawSettings();
  void _drawColorOrder();
  void _drawNetwork();
  void _drawEditIP();
  void _drawEditValue();
  void _drawConfirm();
  void _drawStatusBar();       // thin top bar showing mode icon + nickname
  void _drawScrollIndicators(int8_t total, int8_t visible);

  // ---- Helpers ----
  void _setScreen(MenuScreen s, int8_t cursor);
  void _returnToMain();
  void _saveEditValue();
  void _rebootApply();
};

#endif