#include "LEDController.h"
#include "Pins.h"
#include <string.h>

// ---------------------------------------------------------------------------
// MAX_LEDS_PER_STRIP is now defined in Pins.h based on ACTIVE_OUTPUTS.
// ---------------------------------------------------------------------------

// DMAMEM must be statically allocated on Teensy.
// Single contiguous buffer for all 16 strips.
DMAMEM static int s_displayMemory[MAX_LEDS_PER_STRIP * 16];
DMAMEM static int s_drawingMemory[MAX_LEDS_PER_STRIP * 16];

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
  : _leds(MAX_LEDS_PER_STRIP, s_displayMemory, s_drawingMemory, WS2811_800kHz, MAX_OUTPUTS, ledPins)
  , _ledsPerStrip(0)
  , _colorOrder(ORDER_RGB)
{
  for (int i = 0; i < MAX_OUTPUTS; i++) _outputActive[i] = (i < ACTIVE_OUTPUTS);
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

  // Initialise OctoWS2811 with all 16 pins
  // Must use the full-parameter begin() to set the custom pin list
  _leds.begin(MAX_LEDS_PER_STRIP, s_displayMemory, s_drawingMemory, WS2811_800kHz, MAX_OUTPUTS, ledPins);
  _leds.show();

  // All black initially
  clear();
  show();
}

// ---------------------------------------------------------------------------
void LEDController::show()
{
  _leds.show();
}

// ---------------------------------------------------------------------------
void LEDController::setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
  if (strip >= MAX_OUTPUTS) return;
  if (index >= _ledsPerStrip) return;
  if (!_outputActive[strip]) return;

  // OctoWS2811 stores pixels per-strip contiguously:
  //   strip 0: indices 0..ledsPerStrip-1
  //   strip 1: indices ledsPerStrip..2*ledsPerStrip-1
  // Each LED uses 3 ints (RGB). The setPixel API takes an overall LED index.
  const uint8_t *perm = colorOrderPerm[_colorOrder];
  uint8_t rgb[3] = {r, g, b};
  uint8_t ro = rgb[perm[0]];
  uint8_t go = rgb[perm[1]];
  uint8_t bo = rgb[perm[2]];
  _leds.setPixel(strip * _ledsPerStrip + index, ro, go, bo);
}

// ---------------------------------------------------------------------------
void LEDController::fillFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // totalPixels = number of pixels across all active strips
  uint16_t stripPixels = totalPixels / ACTIVE_OUTPUTS;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;

  const uint8_t *perm = colorOrderPerm[_colorOrder];

  for (uint8_t s = 0; s < ACTIVE_OUTPUTS; s++) {
    if (!_outputActive[s]) continue;

    const uint8_t *src = rgbData + s * stripPixels * 3;
    for (uint16_t i = 0; i < stripPixels; i++) {
      uint8_t rgb[3];
      rgb[0] = *src++;
      rgb[1] = *src++;
      rgb[2] = *src++;
      _leds.setPixel(s * _ledsPerStrip + i,
                     rgb[perm[0]], rgb[perm[1]], rgb[perm[2]]);
    }
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFrameDirect(const uint8_t *rgbData, uint16_t totalPixels)
{
  uint16_t stripPixels = totalPixels / ACTIVE_OUTPUTS;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;
  uint16_t stripBytes = stripPixels * 3;

  // OctoWS2811 drawing memory is a uint8_t buffer, 3 bytes per pixel,
  // arranged strip-major.
  uint8_t *draw = (uint8_t *)s_drawingMemory;

  // Fast path for ORDER_RGB: single memcpy per active strip
  if (_colorOrder == ORDER_RGB) {
    for (uint8_t s = 0; s < ACTIVE_OUTPUTS; s++) {
      uint8_t *dst = draw + s * _ledsPerStrip * 3;
      if (_outputActive[s]) {
        memcpy(dst, rgbData + s * stripBytes, stripBytes);
      } else {
        memset(dst, 0, _ledsPerStrip * 3);
      }
    }
    return;
  }

  // Fast path for ORDER_BGR: swap R↔B per pixel, 3 bytes per pixel
  if (_colorOrder == ORDER_BGR) {
    for (uint8_t s = 0; s < ACTIVE_OUTPUTS; s++) {
      uint8_t *dst = draw + s * _ledsPerStrip * 3;
      if (!_outputActive[s]) {
        memset(dst, 0, _ledsPerStrip * 3);
        continue;
      }
      const uint8_t *src = rgbData + s * stripBytes;
      for (uint16_t i = 0; i < stripPixels; i++) {
        dst[i*3 + 0] = src[i*3 + 2];  // B
        dst[i*3 + 1] = src[i*3 + 1];  // G
        dst[i*3 + 2] = src[i*3 + 0];  // R
      }
      memset(dst + stripBytes, 0, (_ledsPerStrip - stripPixels) * 3);
    }
    return;
  }

  // Fallback for all other color orders: use permutation lookup
  const uint8_t *perm = colorOrderPerm[_colorOrder];
  for (uint8_t s = 0; s < ACTIVE_OUTPUTS; s++) {
    uint8_t *dst = draw + s * _ledsPerStrip * 3;
    if (!_outputActive[s]) {
      memset(dst, 0, _ledsPerStrip * 3);
      continue;
    }
    const uint8_t *src = rgbData + s * stripBytes;
    for (uint16_t i = 0; i < stripPixels; i++) {
      dst[i*3 + 0] = src[i*3 + perm[0]];
      dst[i*3 + 1] = src[i*3 + perm[1]];
      dst[i*3 + 2] = src[i*3 + perm[2]];
    }
    memset(dst + stripBytes, 0, (_ledsPerStrip - stripPixels) * 3);
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFromBin(const uint8_t *data, uint16_t len)
{
  uint16_t maxBytes = _ledsPerStrip * ACTIVE_OUTPUTS * 3;
  if (len > maxBytes) len = maxBytes;
  uint16_t pixelCount = len / 3;
  if (pixelCount > _ledsPerStrip * ACTIVE_OUTPUTS) pixelCount = _ledsPerStrip * ACTIVE_OUTPUTS;
  uint8_t *draw = (uint8_t *)s_drawingMemory;

  if (_colorOrder == ORDER_RGB) {
    memcpy(draw, data, pixelCount * 3);
    if (pixelCount * 3 < maxBytes) {
      memset(draw + pixelCount * 3, 0, maxBytes - pixelCount * 3);
    }
    return;
  }

  if (_colorOrder == ORDER_BGR) {
    for (uint16_t i = 0; i < pixelCount; i++) {
      draw[i*3 + 0] = data[i*3 + 2];
      draw[i*3 + 1] = data[i*3 + 1];
      draw[i*3 + 2] = data[i*3 + 0];
    }
    if (pixelCount * 3 < maxBytes) {
      memset(draw + pixelCount * 3, 0, maxBytes - pixelCount * 3);
    }
    return;
  }

  const uint8_t *perm = colorOrderPerm[_colorOrder];
  for (uint16_t i = 0; i < pixelCount; i++) {
    draw[i*3 + 0] = data[i*3 + perm[0]];
    draw[i*3 + 1] = data[i*3 + perm[1]];
    draw[i*3 + 2] = data[i*3 + perm[2]];
  }
  if (pixelCount * 3 < maxBytes) {
    memset(draw + pixelCount * 3, 0, maxBytes - pixelCount * 3);
  }
}

// ---------------------------------------------------------------------------
void LEDController::clear()
{
  memset((uint8_t *)s_drawingMemory, 0, MAX_LEDS_PER_STRIP * MAX_OUTPUTS * 3);
}

// ---------------------------------------------------------------------------
void LEDController::setOutputMask(const bool active[16])
{
  memcpy(_outputActive, active, sizeof(_outputActive));
}

// ---------------------------------------------------------------------------
uint8_t *LEDController::getDrawingMemory()
{
  return (uint8_t *)s_drawingMemory;
}