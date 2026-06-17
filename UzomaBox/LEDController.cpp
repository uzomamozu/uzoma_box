#include "LEDController.h"

// ---------------------------------------------------------------------------
// Maximum compile-time dimension so we can statically allocate DMAMEM memory.
// The user can set up to MAX_LEDS_PER_STRIP in config.
// Adjust if you need more LEDs.
// ---------------------------------------------------------------------------
#define MAX_LEDS_PER_STRIP  512

// DMAMEM must be statically allocated on Teensy.
// We allocate for the maximum possible size; the actual used amount is set in begin().
DMAMEM static int s_displayMemory[MAX_LEDS_PER_STRIP * 6];
static int s_drawingMemory[MAX_LEDS_PER_STRIP * 6];

// ---------------------------------------------------------------------------
LEDController::LEDController()
  : _leds(MAX_LEDS_PER_STRIP, s_displayMemory, s_drawingMemory, WS2811_800kHz)
  , _ledsPerStrip(0)
{
  for (int i = 0; i < 8; i++) _outputActive[i] = true;
}

LEDController::~LEDController()
{
}

// ---------------------------------------------------------------------------
void LEDController::begin(uint16_t ledsPerStrip)
{
  if (ledsPerStrip > MAX_LEDS_PER_STRIP) {
    ledsPerStrip = MAX_LEDS_PER_STRIP;
  }
  _ledsPerStrip = ledsPerStrip;

  // Re-initialise OctoWS2811 with the desired strip length.
  // We cannot change the internal pointers, but we can call begin() again.
  _leds.begin();
  _leds.show();    // all black initially
}

// ---------------------------------------------------------------------------
void LEDController::show()
{
  _leds.show();
}

// ---------------------------------------------------------------------------
void LEDController::setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
  if (strip >= 8) return;
  if (index >= _ledsPerStrip) return;
  if (!_outputActive[strip]) return;

  // OctoWS2811 stores pixels per-strip contiguously:
  //   strip 0: indices 0..ledsPerStrip-1
  //   strip 1: indices ledsPerStrip..2*ledsPerStrip-1
  // Each LED uses 3 ints (RGB).  The setPixel API takes an overall LED index.
  _leds.setPixel(strip * _ledsPerStrip + index, r, g, b);
}

// ---------------------------------------------------------------------------
void LEDController::fillFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // totalPixels = number of pixels across all 8 strips = ledsPerStrip * 8
  // Data is arranged as strip-major: strip0_pixel0..strip0_pixelN, strip1_pixel0...
  uint16_t stripPixels = totalPixels / 8;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;

  for (uint8_t s = 0; s < 8; s++) {
    if (!_outputActive[s]) continue;   // skip inactive strips

    const uint8_t *src = rgbData + s * stripPixels * 3;
    for (uint16_t i = 0; i < stripPixels; i++) {
      uint8_t r = *src++;
      uint8_t g = *src++;
      uint8_t b = *src++;
      _leds.setPixel(s * _ledsPerStrip + i, r, g, b);
    }
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFromBin(const uint8_t *data, uint16_t len)
{
  // The .BIN frame data is a raw RGB byte stream for ALL 8 strips.
  // We copy it directly into the OctoWS2811 drawing memory area.
  // The drawing memory layout is: each LED uses 3 ints (RGB).
  // data is byte-triplets RGBRGB...
  // We loop over the pixel count based on len.
  uint16_t maxBytes = _ledsPerStrip * 8 * 3;
  if (len > maxBytes) len = maxBytes;

  uint16_t pixelCount = len / 3;
  for (uint16_t i = 0; i < pixelCount && i < _ledsPerStrip * 8; i++) {
    uint8_t r = data[i * 3 + 0];
    uint8_t g = data[i * 3 + 1];
    uint8_t b = data[i * 3 + 2];
    _leds.setPixel(i, r, g, b);
  }
}

// ---------------------------------------------------------------------------
void LEDController::clear()
{
  for (uint16_t i = 0; i < _ledsPerStrip * 8; i++) {
    _leds.setPixel(i, 0, 0, 0);
  }
}

// ---------------------------------------------------------------------------
void LEDController::setOutputMask(const bool active[8])
{
  memcpy(_outputActive, active, sizeof(_outputActive));
}

// ---------------------------------------------------------------------------
uint8_t *LEDController::getDrawingMemory()
{
  return (uint8_t *)s_drawingMemory;
}