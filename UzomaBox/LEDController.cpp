#include "LEDController.h"
#include <string.h>

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
// Color-order permutation lookup table
// For each ColorOrder value, gives the indices of {R, G, B} in the 3-byte triplet.
// ---------------------------------------------------------------------------
const uint8_t colorOrderPerm[COLOR_ORDER_COUNT][3] = {
  {0, 1, 2},   // ORDER_RGB  -> R=byte[0], G=byte[1], B=byte[2]
  {0, 2, 1},   // ORDER_RBG  -> R=byte[0], G=byte[2], B=byte[1]
  {1, 0, 2},   // ORDER_GRB  -> R=byte[1], G=byte[0], B=byte[2]
  {1, 2, 0},   // ORDER_GBR  -> R=byte[1], G=byte[2], B=byte[0]
  {2, 0, 1},   // ORDER_BRG  -> R=byte[2], G=byte[0], B=byte[1]
  {2, 1, 0},   // ORDER_BGR  -> R=byte[2], G=byte[1], B=byte[0]
};

// ---------------------------------------------------------------------------
// Parse a color-order string ("rgb", "grb", etc.) into a ColorOrder enum.
// Returns ORDER_RGB on unrecognised input.
// ---------------------------------------------------------------------------
ColorOrder parseColorOrder(const char *str)
{
  if (!str) return ORDER_RGB;
  // Case-insensitive comparison
  char buf[4];
  for (int i = 0; i < 3 && str[i]; i++) buf[i] = tolower((unsigned char)str[i]);
  buf[3] = 0;

  if      (!strcmp(buf, "rgb")) return ORDER_RGB;
  else if (!strcmp(buf, "rbg")) return ORDER_RBG;
  else if (!strcmp(buf, "grb")) return ORDER_GRB;
  else if (!strcmp(buf, "gbr")) return ORDER_GBR;
  else if (!strcmp(buf, "brg")) return ORDER_BRG;
  else if (!strcmp(buf, "bgr")) return ORDER_BGR;
  else                         return ORDER_RGB;
}

// ---------------------------------------------------------------------------
// Convert a ColorOrder enum to its string representation.
// ---------------------------------------------------------------------------
const char *colorOrderStr(ColorOrder order)
{
  switch (order) {
    case ORDER_RGB: return "rgb";
    case ORDER_RBG: return "rbg";
    case ORDER_GRB: return "grb";
    case ORDER_GBR: return "gbr";
    case ORDER_BRG: return "brg";
    case ORDER_BGR: return "bgr";
    default:        return "rgb";
  }
}

// ---------------------------------------------------------------------------
LEDController::LEDController()
  : _leds(MAX_LEDS_PER_STRIP, s_displayMemory, s_drawingMemory, WS2811_800kHz)
  , _ledsPerStrip(0)
  , _colorOrder(ORDER_RGB)
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

  // Apply color-order reordering
  const uint8_t *perm = colorOrderPerm[_colorOrder];
  uint8_t rgb[3] = {r, g, b};
  uint8_t ro = rgb[perm[0]];
  uint8_t go = rgb[perm[1]];
  uint8_t bo = rgb[perm[2]];

  // OctoWS2811 stores pixels per-strip contiguously:
  //   strip 0: indices 0..ledsPerStrip-1
  //   strip 1: indices ledsPerStrip..2*ledsPerStrip-1
  // Each LED uses 3 ints (RGB).  The setPixel API takes an overall LED index.
  _leds.setPixel(strip * _ledsPerStrip + index, ro, go, bo);
}

// ---------------------------------------------------------------------------
void LEDController::fillFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // totalPixels = number of pixels across all 8 strips = ledsPerStrip * 8
  // Data is arranged as strip-major: strip0_pixel0..strip0_pixelN, strip1_pixel0...
  uint16_t stripPixels = totalPixels / 8;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;

  const uint8_t *perm = colorOrderPerm[_colorOrder];

  for (uint8_t s = 0; s < 8; s++) {
    if (!_outputActive[s]) continue;   // skip inactive strips

    const uint8_t *src = rgbData + s * stripPixels * 3;
    for (uint16_t i = 0; i < stripPixels; i++) {
      uint8_t rgb[3];
      rgb[0] = *src++;
      rgb[1] = *src++;
      rgb[2] = *src++;
      _leds.setPixel(s * _ledsPerStrip + i, rgb[perm[0]], rgb[perm[1]], rgb[perm[2]]);
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

  const uint8_t *perm = colorOrderPerm[_colorOrder];
  uint16_t pixelCount = len / 3;
  for (uint16_t i = 0; i < pixelCount && i < _ledsPerStrip * 8; i++) {
    uint8_t rgb[3];
    rgb[0] = data[i * 3 + 0];
    rgb[1] = data[i * 3 + 1];
    rgb[2] = data[i * 3 + 2];
    _leds.setPixel(i, rgb[perm[0]], rgb[perm[1]], rgb[perm[2]]);
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