#ifndef LEDController_h
#define LEDController_h

#include <Arduino.h>
#include <OctoWS2811.h>

// Supported RGB color orders for the LEDs
// Maps incoming byte triplets to the physical LED ordering.
// The lookup table gives {R_index, G_index, B_index} in the 3-byte input.
enum ColorOrder {
  ORDER_RGB = 0,  // Red, Green, Blue (default)
  ORDER_RBG,      // Red, Blue, Green
  ORDER_GRB,      // Green, Red, Blue
  ORDER_GBR,      // Green, Blue, Red
  ORDER_BRG,      // Blue, Red, Green
  ORDER_BGR,      // Blue, Green, Red
  COLOR_ORDER_COUNT
};

// Lookup table: for each ColorOrder, the [R, G, B] indices into the 3-byte triplet
extern const uint8_t colorOrderPerm[COLOR_ORDER_COUNT][3];

// Convert a string like "rgb", "grb" to ColorOrder enum.
// Returns ORDER_RGB on unrecognised input.
ColorOrder parseColorOrder(const char *str);

// Convert a ColorOrder enum to its string representation (e.g. "grb")
const char *colorOrderStr(ColorOrder order);

class LEDController {
public:
  LEDController();
  ~LEDController();

  // Initialise OctoWS2811 with given number of LEDs per strip
  void begin(uint16_t ledsPerStrip);

  // Push the current drawing memory to the strips
  void show();

  // Set a single pixel (strip 0-7, index 0..ledsPerStrip-1, RGB)
  // Honors the current color order: r, g, b are reordered before writing.
  void setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b);

  // Fill the drawing memory from a raw RGB frame buffer (size = ledsPerStrip * 8 * 3)
  // Output mask: if outputActive[i] is false, that strip stays black.
  // Honors the current color order.
  void fillFrame(const uint8_t *rgbData, uint16_t totalPixels);

  // Directly copy data into drawingMemory (for .BIN playback)
  // Honors the current color order.
  void fillFromBin(const uint8_t *data, uint16_t len);

  // Zero out all pixels
  void clear();

  // Update the output-active mask
  void setOutputMask(const bool active[8]);

  // Access drawingMemory pointer (for direct manipulation)
  uint8_t *getDrawingMemory();
  uint16_t getLedsPerStrip() const { return _ledsPerStrip; }

  // Total pixels across all 8 strips
  uint16_t totalPixels() const { return _ledsPerStrip * 8; }

  // ---- Color order ----
  void setColorOrder(ColorOrder order) { _colorOrder = order; }
  ColorOrder getColorOrder() const { return _colorOrder; }

private:
  OctoWS2811  _leds;
  uint16_t    _ledsPerStrip;
  bool        _outputActive[8];
  ColorOrder  _colorOrder;
  int        *_displayMemory;   // DMAMEM allocated in begin()
  int        *_drawingMemory;   // regular RAM

  // DMA-allocated storage is handled inside OctoWS2811;
  // we allocate displayMemory and drawingMemory ourselves based on strip count.
};

#endif