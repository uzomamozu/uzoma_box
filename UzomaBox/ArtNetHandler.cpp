#include "ArtNetHandler.h"

// Timeout in ms before we consider ArtNet stream lost
#define ARTNET_TIMEOUT_MS   2000
// DMX channels per universe
#define DMX_PER_UNIVERSE    512

// ---------------------------------------------------------------------------
ArtNetHandler::ArtNetHandler()
  : _ledsPerStrip(512)
  , _totalPixels(512 * 8)
  , _frameBuffer(nullptr)
  , _receiving(false)
  , _lastPacketTime(0)
  , _frameCb(nullptr)
  , _allUpdated(false)
  , _frameStarted(false)
  , _frameStartTime(0)
  , _universesPerStrip(3)
{
  _frameBuffer = new uint8_t[_totalPixels * 3];
  memset(_frameBuffer, 0, _totalPixels * 3);
  resetFrameState();
}

ArtNetHandler::~ArtNetHandler()
{
  delete[] _frameBuffer;
}

// ---------------------------------------------------------------------------
void ArtNetHandler::begin()
{
  _udp.begin(ARTNET_PORT);
}

// ---------------------------------------------------------------------------
void ArtNetHandler::setLedsPerStrip(uint16_t n)
{
  _ledsPerStrip = n;
  _totalPixels = n * 8;

  // Recalculate universes per strip: ceil(n * 3 / 512)
  _universesPerStrip = (uint8_t)((n * 3 + DMX_PER_UNIVERSE - 1) / DMX_PER_UNIVERSE);
  if (_universesPerStrip > MAX_UNIVERSES_PER_STRIP) {
    _universesPerStrip = MAX_UNIVERSES_PER_STRIP;
  }

  // Reallocate frame buffer
  delete[] _frameBuffer;
  _frameBuffer = new uint8_t[_totalPixels * 3];
  memset(_frameBuffer, 0, _totalPixels * 3);

  resetFrameState();
}

// ---------------------------------------------------------------------------
void ArtNetHandler::subUniverseRange(uint8_t sub, uint16_t &offset, uint16_t &count) const
{
  // First sub: LEDs 0..169 (510 DMX channels)
  // Second sub: LEDs 170..339 (510 DMX channels)
  // Third sub: LEDs 340..511 (516 DMX channels -> 172 LEDs)
  // For custom widths, we compute from first principles:
  //   Each sub-universe carries 170 LEDs (510 DMX channels) except possibly the last
  uint16_t ledsPerSub = DMX_PER_UNIVERSE / 3;   // 170
  offset = sub * ledsPerSub;

  if (sub < _universesPerStrip - 1) {
    count = ledsPerSub;
  } else {
    // Last sub-universe gets whatever is left
    count = _ledsPerStrip - offset;
  }

  // Clamp: if offset exceeds strip length, this sub is empty
  if (offset >= _ledsPerStrip) {
    offset = _ledsPerStrip;
    count = 0;
  }
}

// ---------------------------------------------------------------------------
void ArtNetHandler::resetFrameState()
{
  for (int i = 0; i < 8; i++) {
    _universeReceived[i] = false;
    for (int s = 0; s < MAX_UNIVERSES_PER_STRIP; s++) {
      _universeSubReceived[i][s] = false;
    }
  }
  _allUpdated   = false;
  _frameStarted = false;
  _frameStartTime = 0;
}

// ---------------------------------------------------------------------------
void ArtNetHandler::flushFrame()
{
  if (_frameCb) {
    _frameCb(_frameBuffer, _totalPixels);
  }
  resetFrameState();
}

// ---------------------------------------------------------------------------
int ArtNetHandler::poll()
{
  int parsed = 0;
  int packetSize = _udp.parsePacket();

  while (packetSize > 0) {
    if (packetSize > (int)sizeof(_packetBuffer)) {
      packetSize = sizeof(_packetBuffer);
    }
    int n = _udp.read(_packetBuffer, packetSize);
    if (n >= ARTNET_HEADER_LEN + 1) {
      processPacket(_packetBuffer, n);
      parsed++;
    }
    packetSize = _udp.parsePacket();
  }

  // ---- Check receiving timeout ----
  if (_receiving && (millis() - _lastPacketTime > ARTNET_TIMEOUT_MS)) {
    _receiving = false;
  }

  // ---- Frame assembly timeout ----
  if (_frameStarted && (millis() - _frameStartTime > FRAME_TIMEOUT_MS)) {
    flushFrame();
  }

  // ---- Full frame ready ----
  if (_allUpdated) {
    _allUpdated = false;
    if (_frameCb) {
      _frameCb(_frameBuffer, _totalPixels);
    }
    resetFrameState();
  }

  return parsed;
}

// ---------------------------------------------------------------------------
void ArtNetHandler::processPacket(const uint8_t *packet, int len)
{
  // Check Art-Net header magic
  if (packet[0] != 'A' || packet[1] != 'r' || packet[2] != 't' ||
      packet[3] != '-' || packet[4] != 'N' || packet[5] != 'e' ||
      packet[6] != 't' || packet[7] != 0) {
    return;
  }

  // Check OpCode (ArtDMX = 0x5000 little-endian)
  uint16_t opCode = packet[8] | (packet[9] << 8);
  if (opCode != ARTNET_OP_DMX) return;

  // Extract universe
  uint16_t universe = packet[14] | (packet[15] << 8);

  // DMX data starts at offset 18
  const uint8_t *dmxData = packet + ARTNET_HEADER_LEN;
  int dmxLen = len - ARTNET_HEADER_LEN;
  if (dmxLen > ARTNET_DMX_LEN) dmxLen = ARTNET_DMX_LEN;

  uint8_t ups = _universesPerStrip;

  // Find which strip and which sub-universe this universe belongs to
  for (uint8_t strip = 0; strip < 8; strip++) {
    uint16_t base = _startUniverse[strip];
    for (uint8_t sub = 0; sub < ups; sub++) {
      if (universe == base + sub) {

        // ---- Mark this sub-universe as received ----
        if (!_universeSubReceived[strip][sub]) {
          _universeSubReceived[strip][sub] = true;
          if (!_frameStarted) {
            _frameStarted   = true;
            _frameStartTime = millis();
          }
        }

        // ---- Map DMX data into _frameBuffer ----
        uint16_t ledOffset, ledCount;
        subUniverseRange(sub, ledOffset, ledCount);

        uint16_t absoluteOffset = strip * _ledsPerStrip + ledOffset;
        uint16_t channels = dmxLen;
        if (channels > ledCount * 3) channels = ledCount * 3;

        for (uint16_t i = 0; i < channels / 3; i++) {
          uint16_t pixelIdx = absoluteOffset + i;
          if (pixelIdx >= _totalPixels) break;
          _frameBuffer[pixelIdx * 3 + 0] = dmxData[i * 3 + 0];  // R
          _frameBuffer[pixelIdx * 3 + 1] = dmxData[i * 3 + 1];  // G
          _frameBuffer[pixelIdx * 3 + 2] = dmxData[i * 3 + 2];  // B
        }

        // ---- Check if this strip now has ALL its sub-universes ----
        bool stripComplete = true;
        for (uint8_t s = 0; s < ups; s++) {
          if (!_universeSubReceived[strip][s]) {
            stripComplete = false;
            break;
          }
        }
        _universeReceived[strip] = stripComplete;

        // ---- Check if ALL strips are now complete ----
        bool allDone = true;
        for (uint8_t s = 0; s < 8; s++) {
          if (!_universeReceived[s]) { allDone = false; break; }
        }
        _allUpdated = allDone;

        _receiving = true;
        _lastPacketTime = millis();
        return;
      }
    }
  }
}

// ---------------------------------------------------------------------------
void ArtNetHandler::setFrameCallback(FrameCallback cb)
{
  _frameCb = cb;
}

// ---------------------------------------------------------------------------
void ArtNetHandler::resetTimeout()
{
  _lastPacketTime = millis();
  _receiving = true;
}

// ---------------------------------------------------------------------------
void ArtNetHandler::setUniverseMapping(const uint16_t startUniv[8])
{
  memcpy(_startUniverse, startUniv, sizeof(_startUniverse));
  resetFrameState();
}