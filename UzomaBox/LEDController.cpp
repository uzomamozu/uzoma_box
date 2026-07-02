#include "LEDController.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Maximum compile-time dimension so we can statically allocate DMAMEM memory.
// The user can set up to MAX_LEDS_PER_STRIP in config.
// Adjust if you need more LEDs.
// ---------------------------------------------------------------------------
#define MAX_LEDS_PER_STRIP  512

// ---------------------------------------------------------------------------
// Pin assignments for the two OctoWS2811 instances
// Instance #1: strips 0-7  (default OctoWS2811 pins)
// Instance #2: strips 8-15 (alternate pins)
// ---------------------------------------------------------------------------
const uint8_t octoPins1[] = {2, 14, 7, 8, 6, 20, 21, 5};
const uint8_t octoPins2[] = {28, 29, 24, 25, 10, 11, 12, 4};

// DMAMEM must be statically allocated on Teensy.
// We allocate for the maximum possible size for each Octo instance.
DMAMEM static int s_displayMemory1[MAX_LEDS_PER_STRIP * 8];
DMAMEM static int s_drawingMemory1[MAX_LEDS_PER_STRIP * 8];
DMAMEM static int s_displayMemory2[MAX_LEDS_PER_STRIP * 8];
DMAMEM static int s_drawingMemory2[MAX_LEDS_PER_STRIP * 8];

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
  : _leds(MAX_LEDS_PER_STRIP, s_displayMemory1, s_drawingMemory1, WS2811_800kHz)
  , _leds2(MAX_LEDS_PER_STRIP, s_displayMemory2, s_drawingMemory2, WS2811_800kHz | WS2811_ALT_PINS)
  , _ledsPerStrip(0)
  , _colorOrder(ORDER_RGB)
{
  for (int i = 0; i < NUM_STRIPS; i++) _outputActive[i] = true;
  _displayMemory[0] = s_displayMemory1;
  _drawingMemory[0] = s_drawingMemory1;
  _displayMemory[1] = s_displayMemory2;
  _drawingMemory[1] = s_drawingMemory2;
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

  // Initialise first OctoWS2811 (strips 0-7)
  _leds.begin();
  _leds.show();

  // Initialise second OctoWS2811 (strips 8-15)
  _leds2.begin();
  _leds2.show();

  // All black initially
  clear();
  show();
}

// ---------------------------------------------------------------------------
void LEDController::show()
{
  _leds.show();
  _leds2.show();
}

// ---------------------------------------------------------------------------
// Internal helper: set a pixel on a given OctoWS2811 instance.
// globalIdx is the combined index across all strips of that instance.
// ---------------------------------------------------------------------------
void LEDController::_setPixelInternal(OctoWS2811 &octo, uint16_t globalIdx,
                                       uint8_t r, uint8_t g, uint8_t b)
{
  const uint8_t *perm = colorOrderPerm[_colorOrder];
  uint8_t rgb[3] = {r, g, b};
  uint8_t ro = rgb[perm[0]];
  uint8_t go = rgb[perm[1]];
  uint8_t bo = rgb[perm[2]];
  octo.setPixel(globalIdx, ro, go, bo);
}

// ---------------------------------------------------------------------------
void LEDController::setPixel(uint8_t strip, uint16_t index, uint8_t r, uint8_t g, uint8_t b)
{
  if (strip >= NUM_STRIPS) return;
  if (index >= _ledsPerStrip) return;
  if (!_outputActive[strip]) return;

  if (strip < 8) {
    // Strips 0-7 → first OctoWS2811
    _setPixelInternal(_leds, strip * _ledsPerStrip + index, r, g, b);
  } else {
    // Strips 8-15 → second OctoWS2811
    _setPixelInternal(_leds2, (strip - 8) * _ledsPerStrip + index, r, g, b);
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFrame(const uint8_t *rgbData, uint16_t totalPixels)
{
  // totalPixels = number of pixels across all 16 strips = ledsPerStrip * 16
  uint16_t stripPixels = totalPixels / NUM_STRIPS;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;

  const uint8_t *perm = colorOrderPerm[_colorOrder];

  for (uint8_t s = 0; s < NUM_STRIPS; s++) {
    if (!_outputActive[s]) continue;

    const uint8_t *src = rgbData + s * stripPixels * 3;
    OctoWS2811 &octo = (s < 8) ? _leds : _leds2;
    uint8_t octoStrip = (s < 8) ? s : (s - 8);

    for (uint16_t i = 0; i < stripPixels; i++) {
      uint8_t rgb[3];
      rgb[0] = *src++;
      rgb[1] = *src++;
      rgb[2] = *src++;
      octo.setPixel(octoStrip * _ledsPerStrip + i,
                    rgb[perm[0]], rgb[perm[1]], rgb[perm[2]]);
    }
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFrameDirect(const uint8_t *rgbData, uint16_t totalPixels)
{
  uint16_t stripPixels = totalPixels / NUM_STRIPS;
  if (stripPixels > _ledsPerStrip) stripPixels = _ledsPerStrip;
  uint16_t stripBytes = stripPixels * 3;

  // Each Octo handles 8 strips worth of data
  for (int octoIdx = 0; octoIdx < 2; octoIdx++) {
    OctoWS2811 &octo = (octoIdx == 0) ? _leds : _leds2;
    uint8_t *draw = (uint8_t *)_drawingMemory[octoIdx];
    int stripBase = octoIdx * 8;  // 0 or 8

    if (_colorOrder == ORDER_RGB) {
      for (uint8_t s = 0; s < 8; s++) {
        uint8_t *dst = draw + s * _ledsPerStrip * 3;
        int globalStrip = stripBase + s;
        if (_outputActive[globalStrip]) {
          memcpy(dst, rgbData + globalStrip * stripBytes, stripBytes);
        } else {
          memset(dst, 0, _ledsPerStrip * 3);
        }
      }
    } else if (_colorOrder == ORDER_BGR) {
      for (uint8_t s = 0; s < 8; s++) {
        uint8_t *dst = draw + s * _ledsPerStrip * 3;
        int globalStrip = stripBase + s;
        if (!_outputActive[globalStrip]) {
          memset(dst, 0, _ledsPerStrip * 3);
          continue;
        }
        const uint8_t *src = rgbData + globalStrip * stripBytes;
        for (uint16_t i = 0; i < stripPixels; i++) {
          dst[i*3 + 0] = src[i*3 + 2];  // B
          dst[i*3 + 1] = src[i*3 + 1];  // G
          dst[i*3 + 2] = src[i*3 + 0];  // R
        }
        memset(dst + stripBytes, 0, (_ledsPerStrip - stripPixels) * 3);
      }
    } else {
      const uint8_t *perm = colorOrderPerm[_colorOrder];
      for (uint8_t s = 0; s < 8; s++) {
        uint8_t *dst = draw + s * _ledsPerStrip * 3;
        int globalStrip = stripBase + s;
        if (!_outputActive[globalStrip]) {
          memset(dst, 0, _ledsPerStrip * 3);
          continue;
        }
        const uint8_t *src = rgbData + globalStrip * stripBytes;
        for (uint16_t i = 0; i < stripPixels; i++) {
          dst[i*3 + 0] = src[i*3 + perm[0]];
          dst[i*3 + 1] = src[i*3 + perm[1]];
          dst[i*3 + 2] = src[i*3 + perm[2]];
        }
        memset(dst + stripBytes, 0, (_ledsPerStrip - stripPixels) * 3);
      }
    }
  }
}

// ---------------------------------------------------------------------------
void LEDController::fillFromBin(const uint8_t *data, uint16_t len)
{
  // memcpy-based optimisation: copy raw bytes directly into drawing memory
  uint16_t maxBytes = _ledsPerStrip * NUM_STRIPS * 3;
  if (len > maxBytes) len = maxBytes;
  uint16_t pixelCount = len / 3;
  if (pixelCount > _ledsPerStrip * NUM_STRIPS) pixelCount = _ledsPerStrip * NUM_STRIPS;

  // Split the data across both Octo drawing memories
  // Strips 0-7 → drawingMemory[0], strips 8-15 → drawingMemory[1]
  uint8_t *draw0 = (uint8_t *)_drawingMemory[0];
  uint8_t *draw1 = (uint8_t *)_drawingMemory[1];
  uint16_t octoStripBytes = _ledsPerStrip * 8 * 3;  // bytes per Octo instance

  if (_colorOrder == ORDER_RGB) {
    // Copy strips 0-7
    uint16_t copyLen0 = pixelCount * 3;
    if (copyLen0 > octoStripBytes) copyLen0 = octoStripBytes;
    memcpy(draw0, data, copyLen0);
    if (copyLen0 < octoStripBytes) {
      memset(draw0 + copyLen0, 0, octoStripBytes - copyLen0);
    }
    // Copy strips 8-15
    if (pixelCount > _ledsPerStrip * 8) {
      uint16_t pix8 = _ledsPerStrip * 8;
      uint16_t remainPixels = pixelCount - pix8;
      memcpy(draw1, data + pix8 * 3, remainPixels * 3);
      if (remainPixels * 3 < octoStripBytes) {
        memset(draw1 + remainPixels * 3, 0, octoStripBytes - remainPixels * 3);
      }
    } else {
      memset(draw1, 0, octoStripBytes);
    }
    return;
  }

  // For non-RGB orders, process pixel by pixel across all strips
  // Zero both drawing areas first
  memset(draw0, 0, octoStripBytes);
  memset(draw1, 0, octoStripBytes);

  const uint8_t *perm = colorOrderPerm[_colorOrder];
  for (uint16_t i = 0; i < pixelCount; i++) {
    if (i < _ledsPerStrip * 8) {
      draw0[i*3 + 0] = data[i*3 + perm[0]];
      draw0[i*3 + 1] = data[i*3 + perm[1]];
      draw0[i*3 + 2] = data[i*3 + perm[2]];
    } else {
      uint16_t idx = i - _ledsPerStrip * 8;
      draw1[idx*3 + 0] = data[i*3 + perm[0]];
      draw1[idx*3 + 1] = data[i*3 + perm[1]];
      draw1[idx*3 + 2] = data[i*3 + perm[2]];
    }
  }
}

// ---------------------------------------------------------------------------
void LEDController::clear()
{
  for (uint16_t i = 0; i < _ledsPerStrip * 8; i++) {
    _leds.setPixel(i, 0, 0, 0);
  }
  for (uint16_t i = 0; i < _ledsPerStrip * 8; i++) {
    _leds2.setPixel(i, 0, 0, 0);
  }
}

// ---------------------------------------------------------------------------
void LEDController::setOutputMask(const bool active[16])
{
  memcpy(_outputActive, active, sizeof(_outputActive));
}

// ---------------------------------------------------------------------------
uint8_t *LEDController::getDrawingMemory()
{
  // Return a pointer to the unified virtual drawing memory.
  // For direct manipulation, we return a RAM buffer that spans all 16 strips.
  // We use _drawingMemory[0] which covers strips 0-7.
  // Note: strips 8-15 are in _drawingMemory[1].
  // For efficiency, the caller should use getDrawingMemoryForStrip().
  return (uint8_t *)_drawingMemory[0];
}