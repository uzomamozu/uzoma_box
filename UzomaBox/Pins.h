#ifndef Pins_h
#define Pins_h

#include <Arduino.h>

// ===========================================================================
//  PIN DEFINITIONS (HARDWARE)
//  ¡Edita aquí si cambia el cableado!
// ===========================================================================
//
//  All constants and the ledPins array are concentrated here so that hardware
//  changes can be made in a single place without hunting through the codebase.
//
//  Teensy 4.1  –  16 output strips via dual OctoWS2811
//  OLED 128×64 –  I2C (SSD1306)
//  5 buttons   –  active LOW with internal pull-up

// ========================  LED OUTPUT STRIPS  ================================
// Change ACTIVE_OUTPUTS to 8 for the 8-output hardware variant before flashing.
// MAX_OUTPUTS is fixed at 16 (physical hardware capacity) — do not change.
#define ACTIVE_OUTPUTS  8     // ← change to 8 for 8-output version
#define MAX_OUTPUTS     16     // physical maximum, never change

// MAX_LEDS_PER_STRIP depends on ACTIVE_OUTPUTS to keep total pixel buffer ≤ 8192
//   16 outputs × 512 LEDs = 8192 pixels  → 24,576 bytes frame buffer
//    8 outputs × 1024 LEDs = 8192 pixels → 24,576 bytes frame buffer
#if ACTIVE_OUTPUTS == 8
  #define MAX_LEDS_PER_STRIP  1024
#else
  #define MAX_LEDS_PER_STRIP  512
#endif

// Hard FPS cap — WS2811 at 800 kHz (~30 µs/LED)
//   8×1024: 1024×30µs ≈ 30.7ms → ~32 FPS theoretical max
//   16×512: 512×30µs  ≈ 15.4ms → ~65 FPS theoretical max
// Both capped at 30 FPS for safety margin
#define MAX_FRAME_FPS         30
#define MIN_FRAME_INTERVAL_US (1000000UL / MAX_FRAME_FPS)  // 33333 µs

/*
 * DMAMEM usage for 8 outputs × 1024 LEDs (worst case):
 *   s_displayMemory:  1024 × 16 × 4 bytes =  65,536 B
 *   s_drawingMemory:  1024 × 16 × 4 bytes =  65,536 B
 *   s_frameBuffer:    8192 pixels × 3      =  24,576 B
 *   g_playbackBuffer: 8192 pixels × 3      =  24,576 B
 *   s_recBuffer[2]:   2 × 24,583           =  49,166 B
 *   ─────────────────────────────────────────────────
 *   Total:                                  ~229 KB of 512 KB DMAMEM (44.7%)
 *   Available:                              ~283 KB for future expansion
 *
 * For 16 × 512:  ~163 KB (31.8%)
 */

// Teensy 4.1 OctoWS2811 pins (16 outputs in order):
//   NOTE: Pin 4 is ENET_RST (Ethernet PHY reset) — output #5 uses pin 8 instead!
//   Outputs  1- 8: 0,  1,  2,  3,  8,  5,  6,  7
//   Outputs  9-16: 9, 10, 11, 12, 24, 25, 28, 29
extern const uint8_t ledPins[16];

// ========================  BUTTONS  ==========================================
#define PIN_BTN_RED     23   // botón rojo de test
#define PIN_BTN_UP      22
#define PIN_BTN_DOWN    21
#define PIN_BTN_SELECT  20
#define PIN_BTN_BACK    17

// ========================  DMX512 OUTPUT (optocoupler 6N137 + MAX485)  =======
// Habilitar (1) solo cuando el hardware DMX esté presente en pines 14 y 15.
#define DMX_OUTPUT_ENABLED   0       // 1=Habilitado, 0=Deshabilitado

#if DMX_OUTPUT_ENABLED
  #define PIN_DMX_TX           14    // Serial3 TX → 6N137 → MAX485 DI
  #define PIN_DMX_DIR          15    // GPIO → MAX485 DE/RE (HIGH=TX, LOW=RX)
#endif

// ========================  OLED DISPLAY (SSD1306, I2C)  ======================
#define OLED_SDA        18   // Teensy 4.1 I2C0 SDA (Wire)
#define OLED_SCL        19   // Teensy 4.1 I2C0 SCL (Wire)
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1   // sin pin de reset
#define OLED_ADDR      0x3C

#endif