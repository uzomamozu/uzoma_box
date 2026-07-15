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
#define NUM_OUTPUTS  16

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
#define PIN_BTN_BACK    19

// ========================  OLED DISPLAY (SSD1306, I2C)  ======================
#define OLED_SDA        18   // Teensy 4.1 I2C0 SDA (Wire)
#define OLED_SCL        17   // Teensy 4.1 I2C0 SCL (Wire)
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1   // sin pin de reset
#define OLED_ADDR      0x3C

#endif