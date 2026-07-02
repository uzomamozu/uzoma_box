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

// Number of LED strips (dual OctoWS2811 = 16 outputs)
#define NUM_STRIPS  16

class LEDController {
public:
  LEDController();
  ~LEDController();

  // Initialise dual OctoWS2811 with given number of LEDs per strip
  void begin(uint16_t ledsPerStrip);

  // Push the current drawing memory to the strips (both Octo instances)
  void show();

  // Set a single pixel (strip 0-15, index 0..ledsPerStrip-1, RGB)
  // Honors the current color order: r, g, b are reordered before writing.
  void setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b);

  // Fill the drawing memory from a raw RGB frame buffer (size = ledsPerStrip * 16 * 3)
  // Output mask: if outputActive[i] is false, that strip stays black.
  // Honors the current color order.
  // Uses setPixel() per-pixel (slower, legacy).
  void fillFrame(const uint8_t *rgbData, uint16_t totalPixels);

  // Optimised fill: writes directly to drawingMemory as ints.
  // NO function calls per pixel — just a single int store per LED.
  // Honors outputActive mask and color order.
  // ~10-50x faster than fillFrame() for full-frame writes.
  void fillFrameDirect(const uint8_t *rgbData, uint16_t totalPixels);

  // Directly copy data into drawingMemory (for .BIN playback)
  // Honors the current color order.
  void fillFromBin(const uint8_t *data, uint16_t len);

  // Zero out all pixels
  void clear();

  // Update the output-active mask (array of 16 bools)
  void setOutputMask(const bool active[16]);

  // Access drawingMemory pointer (for direct manipulation)
  uint8_t *getDrawingMemory();
  uint16_t getLedsPerStrip() const { return _ledsPerStrip; }

  // Total pixels across all 16 strips
  uint16_t totalPixels() const { return _ledsPerStrip * NUM_STRIPS; }

  // ---- Color order ----
  void setColorOrder(ColorOrder order) { _colorOrder = order; }
  ColorOrder getColorOrder() const { return _colorOrder; }

private:
  // Two OctoWS2811 instances: strips 0-7 on instance #1, strips 8-15 on instance #2
  OctoWS2811  _leds;      // first 8 outputs (default pins)
  OctoWS2811  _leds2;     // second 8 outputs (alternate pins)

  uint16_t    _ledsPerStrip;
  bool        _outputActive[NUM_STRIPS];   // 16 strips
  ColorOrder  _colorOrder;
  int        *_displayMemory[2];   // pointers to DMAMEM blocks
  int        *_drawingMemory[2];   // pointers to regular RAM blocks

  // Internal: write a pixel to a specific OctoWS2811 instance
  void _setPixelInternal(OctoWS2811 &octo, uint16_t globalIdx, uint8_t r, uint8_t g, uint8_t b);
};

#endif