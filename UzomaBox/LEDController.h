#ifndef LEDController_h
#define LEDController_h

#include <Arduino.h>
#include <OctoWS2811.h>

class LEDController {
public:
  LEDController();
  ~LEDController();

  // Initialise OctoWS2811 with given number of LEDs per strip
  void begin(uint16_t ledsPerStrip);

  // Push the current drawing memory to the strips
  void show();

  // Set a single pixel (strip 0-7, index 0..ledsPerStrip-1, RGB)
  void setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b);

  // Fill the drawing memory from a raw RGB frame buffer (size = ledsPerStrip * 8 * 3)
  // Output mask: if outputActive[i] is false, that strip stays black.
  void fillFrame(const uint8_t *rgbData, uint16_t totalPixels);

  // Directly copy data into drawingMemory (for .BIN playback)
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

private:
  OctoWS2811  _leds;
  uint16_t    _ledsPerStrip;
  bool        _outputActive[8];
  int        *_displayMemory;   // DMAMEM allocated in begin()
  int        *_drawingMemory;   // regular RAM

  // DMA-allocated storage is handled inside OctoWS2811;
  // we allocate displayMemory and drawingMemory ourselves based on strip count.
};

#endif