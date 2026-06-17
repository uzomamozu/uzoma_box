#include "ArtNetHandler.h"

// Timeout in ms before we consider ArtNet stream lost
#define ARTNET_TIMEOUT_MS   2000

// Offsets within a strip when mapping 3 universes
// Universe 0: LEDs 0-169  (510 DMX channels)
// Universe 1: LEDs 170-339 (510 DMX channels)
// Universe 2: LEDs 340-511 (516 DMX channels → 172 LEDs)
static const uint16_t kUniverseOffsets[3] = { 0, 170, 340 };
static const uint16_t kUniverseLengths[3] = { 170, 170, 172 };

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
void ArtNetHandler::resetFrameState()
{
  for (int i = 0; i < 8; i++) {
    _universeReceived[i] = false;
    for (int s = 0; s < UNIVERSES_PER_STRIP; s++) {
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
  // Push whatever we have in _frameBuffer to the LED callback
  if (_frameCb) {
    _frameCb(_frameBuffer, _totalPixels);
  }
  // Prepare for the next frame (keep existing pixel data in buffer)
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
  // If a frame was started but not completed within FRAME_TIMEOUT_MS,
  // flush the partial data so LEDs don't freeze.
  if (_frameStarted && (millis() - _frameStartTime > FRAME_TIMEOUT_MS)) {
    // Serial.println("ArtNet: frame timeout - flushing partial data");
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
  // ArtNet header: 12 bytes ID "Art-Net\0" + 2 bytes OpCode + 2 bytes ProtVerHi/Lo
  // + 1 byte Sequence + 1 byte Physical + 2 bytes Universe + 2 bytes NetLength
  // = 18 bytes before DMX data (ArtDMX)
  //
  // Check Art-Net header magic
  if (packet[0] != 'A' || packet[1] != 'r' || packet[2] != 't' ||
      packet[3] != '-' || packet[4] != 'N' || packet[5] != 'e' ||
      packet[6] != 't' || packet[7] != 0) {
    return;  // Not Art-Net
  }

  // Check OpCode (ArtDMX = 0x5000 little-endian)
  uint16_t opCode = packet[8] | (packet[9] << 8);
  if (opCode != ARTNET_OP_DMX) return;

  // Extract universe (14 bytes = Universe low, 15 = Universe high)
  uint16_t universe = packet[14] | (packet[15] << 8);

  // DMX data starts at offset 18
  const uint8_t *dmxData = packet + ARTNET_HEADER_LEN;
  int dmxLen = len - ARTNET_HEADER_LEN;
  if (dmxLen > ARTNET_DMX_LEN) dmxLen = ARTNET_DMX_LEN;

  // Find which strip and which sub-universe (0/1/2) this universe belongs to
  for (uint8_t strip = 0; strip < 8; strip++) {
    uint16_t base = _startUniverse[strip];
    for (uint8_t sub = 0; sub < UNIVERSES_PER_STRIP; sub++) {
      if (universe == base + sub) {

        // ---- Mark this sub-universe as received ----
        if (!_universeSubReceived[strip][sub]) {
          _universeSubReceived[strip][sub] = true;

          // If this is the first sub of a new frame, start the timer
          if (!_frameStarted) {
            _frameStarted   = true;
            _frameStartTime = millis();
          }
        }

        // ---- Map DMX data into _frameBuffer ----
        uint16_t ledOffset = strip * _ledsPerStrip + kUniverseOffsets[sub];
        uint16_t ledCount  = kUniverseLengths[sub];
        if (ledCount > _ledsPerStrip - kUniverseOffsets[sub]) {
          ledCount = _ledsPerStrip - kUniverseOffsets[sub];
        }
        // DMX channel 1 (index 0) = Red, channel 2 = Green, channel 3 = Blue
        for (uint16_t i = 0; i < ledCount && i < (uint16_t)dmxLen / 3; i++) {
          uint16_t pixelIdx = ledOffset + i;
          if (pixelIdx >= _totalPixels) break;
          _frameBuffer[pixelIdx * 3 + 0] = dmxData[i * 3 + 0];  // R
          _frameBuffer[pixelIdx * 3 + 1] = dmxData[i * 3 + 1];  // G
          _frameBuffer[pixelIdx * 3 + 2] = dmxData[i * 3 + 2];  // B
        }

        // ---- Check if this strip now has all 3 sub-universes ----
        bool stripComplete = true;
        for (uint8_t s = 0; s < UNIVERSES_PER_STRIP; s++) {
          if (!_universeSubReceived[strip][s]) {
            stripComplete = false;
            break;
          }
        }
        _universeReceived[strip] = stripComplete;

        // ---- Check if ALL strips are now complete ----
        bool allDone = true;
        for (int s = 0; s < 8; s++) {
          if (!_universeReceived[s]) { allDone = false; break; }
        }
        _allUpdated = allDone;

        _receiving = true;
        _lastPacketTime = millis();
        return;   // processed
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