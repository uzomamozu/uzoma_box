#include "ArtNetHandler.h"

// Timeout in ms before we consider ArtNet stream lost
#define ARTNET_TIMEOUT_MS   2000
// DMX channels per universe
#define DMX_PER_UNIVERSE    512

// Static DMAMEM frame buffer allocation
DMAMEM uint8_t ArtNetHandler::s_frameBuffer[FRAME_BUFFER_SIZE];

// ---------------------------------------------------------------------------
ArtNetHandler::ArtNetHandler()
  : _ledsPerStrip(512)
  , _totalPixels(512 * 16)
  , _receiving(false)
  , _lastPacketTime(0)
  , _universesPerStrip(3)
  , _frameCb(nullptr)
  , _allUpdated(false)
  , _frameReady(false)
  , _frameStarted(false)
  , _frameStartTime(0)
  , _syncReceived(false)
  , _waitingForSync(false)
{
  for (int i = 0; i < 16; i++) _outputActive[i] = true;
  memset(s_frameBuffer, 0, _totalPixels * 3);
  resetFrameState();
}

ArtNetHandler::~ArtNetHandler()
{
  // No heap memory to free — s_frameBuffer is static
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
  _totalPixels = n * 16;

  // Recalculate universes per strip: ceil(n * 3 / 512)
  _universesPerStrip = (uint8_t)((n * 3 + DMX_PER_UNIVERSE - 1) / DMX_PER_UNIVERSE);
  if (_universesPerStrip > MAX_UNIVERSES_PER_STRIP) {
    _universesPerStrip = MAX_UNIVERSES_PER_STRIP;
  }

  // Zero out the portion of the frame buffer we'll use (no heap reallocation)
  size_t usedBytes = _totalPixels * 3;
  if (usedBytes > FRAME_BUFFER_SIZE) usedBytes = FRAME_BUFFER_SIZE;
  memset(s_frameBuffer, 0, usedBytes);

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

  // Guard: if this sub starts at or past the end, it's empty
  if (offset >= _ledsPerStrip) {
    offset = _ledsPerStrip;
    count = 0;
    return;
  }

  if (sub < _universesPerStrip - 1) {
    count = ledsPerSub;
  } else {
    // Last sub-universe gets whatever is left (offset < _ledsPerStrip guaranteed)
    count = _ledsPerStrip - offset;
  }
}

// ---------------------------------------------------------------------------
void ArtNetHandler::resetFrameState()
{
  for (int i = 0; i < 16; i++) {
    _universeReceived[i] = false;
    for (int s = 0; s < MAX_UNIVERSES_PER_STRIP; s++) {
      _universeSubReceived[i][s] = false;
    }
  }
  _allUpdated     = false;
  _frameStarted   = false;
  _frameStartTime = 0;
  _syncReceived   = false;
  _waitingForSync = false;
  _hasNonZeroPixels = false;
}

// ---------------------------------------------------------------------------
// Called on frame assembly timeout — fills drawing memory via callback
// and signals that show() should be called from main loop.
void ArtNetHandler::flushFrame()
{
  // Scan once to determine if this frame has any non-zero pixel
  _hasNonZeroPixels = false;
  uint32_t totalBytes = _totalPixels * 3;
  for (uint32_t i = 0; i < totalBytes; i++) {
    if (s_frameBuffer[i]) {
      _hasNonZeroPixels = true;
      break;
    }
  }

  if (_frameCb) {
    _frameCb(s_frameBuffer, _totalPixels);
  }
  _frameReady = true;
  resetFrameState();
}

// ---------------------------------------------------------------------------
int ArtNetHandler::poll()
{
  int parsed = 0;

  // Drain ALL packets from the UDP buffer (not just 1)
  int packetSize;
  while ((packetSize = _udp.parsePacket()) > 0) {
    // Read into a fixed-size buffer; reject packets larger than the buffer
    int readSize = packetSize;
    if (readSize > (int)sizeof(_packetBuffer)) {
      readSize = sizeof(_packetBuffer);
    }
    int n = _udp.read(_packetBuffer, readSize);
    if (n >= 14) {
      processPacket(_packetBuffer, n);
      parsed++;
    }
  }

  // ---- Check receiving timeout ----
  if (_receiving && (millis() - _lastPacketTime > ARTNET_TIMEOUT_MS)) {
    _receiving = false;
  }

  // ---- Frame assembly timeout ----
  if (_frameStarted && (millis() - _frameStartTime > FRAME_TIMEOUT_MS)) {
    // Defer the callback: just set the flag, show() happens in main loop
    flushFrame();
  }

  // ---- Sync received while waiting for it ----
  if (_waitingForSync && _syncReceived) {
    _waitingForSync = false;
    _syncReceived = false;
    flushFrame();
  }

  // ---- Full frame ready ----
  if (_allUpdated) {
    // If sync has already arrived, go ahead immediately
    if (_syncReceived) {
      _allUpdated = false;
      _syncReceived = false;
      flushFrame();
    } else {
      // Frame complete but no sync yet — wait briefly for Sync
      _waitingForSync = true;
      _allUpdated = false;
      // If sync never arrives, timeout below will flush this frame
    }
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

  // Must have at least OpCode to determine packet type
  if (len < 10) return;

  // Check OpCode
  uint16_t opCode = packet[8] | (packet[9] << 8);

  // ---- Handle ArtPoll (0x2000) ----
  if (opCode == ARTNET_OP_POLL) {
    sendArtPollReply();
    return;
  }

  // ---- Handle ArtSync (0x5200) ----
  if (opCode == ARTNET_OP_SYNC) {
    _syncReceived = true;
    return;
  }

  // ---- Handle ArtDMX (0x5000) ----
  if (opCode != ARTNET_OP_DMX) return;

  // Validate protocol version (Art-Net 4 = 0x000E big-endian at bytes 10-11)
  if (packet[10] != 0x00 || packet[11] != 0x0E) return;

  // ArtDMX requires at least header (18) + 1 DMX byte
  if (len < ARTNET_HEADER_LEN + 1) return;

  // Extract universe
  uint16_t universe = packet[14] | (packet[15] << 8);

  // DMX data starts at offset 18
  const uint8_t *dmxData = packet + ARTNET_HEADER_LEN;
  int dmxLen = len - ARTNET_HEADER_LEN;
  if (dmxLen > ARTNET_DMX_LEN) dmxLen = ARTNET_DMX_LEN;

  uint8_t ups = _universesPerStrip;

  // Find which strip and which sub-universe this universe belongs to
  for (uint8_t strip = 0; strip < 16; strip++) {
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

        // ---- Map DMX data into frame buffer ----
        uint16_t ledOffset, ledCount;
        subUniverseRange(sub, ledOffset, ledCount);

        uint16_t absoluteOffset = strip * _ledsPerStrip + ledOffset;
        uint16_t channels = dmxLen;
        if (channels > ledCount * 3) channels = ledCount * 3;

        for (uint16_t i = 0; i < channels / 3; i++) {
          uint16_t pixelIdx = absoluteOffset + i;
          if (pixelIdx >= _totalPixels) break;
          s_frameBuffer[pixelIdx * 3 + 0] = dmxData[i * 3 + 0];  // R
          s_frameBuffer[pixelIdx * 3 + 1] = dmxData[i * 3 + 1];  // G
          s_frameBuffer[pixelIdx * 3 + 2] = dmxData[i * 3 + 2];  // B
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

        // ---- Check if ALL ACTIVE strips are now complete ----
        bool allDone = true;
        for (uint8_t s = 0; s < 16; s++) {
          if (_outputActive[s] && !_universeReceived[s]) {
            allDone = false; break;
          }
        }
        _allUpdated = allDone;

        _receiving = true;
        _lastPacketTime = millis();
        // Continue to next strip — when all strips share the same
        // start universe (e.g. all set to 0), this single packet
        // provides data for every strip, not just the first match.
        break;
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
void ArtNetHandler::setUniverseMapping(const uint16_t startUniv[16])
{
  memcpy(_startUniverse, startUniv, sizeof(_startUniverse));
  resetFrameState();
}

// ---------------------------------------------------------------------------
void ArtNetHandler::setOutputActive(const bool active[16])
{
  memcpy(_outputActive, active, sizeof(_outputActive));
}

// ---------------------------------------------------------------------------
void ArtNetHandler::sendArtPollReply()
{
  uint8_t reply[ARTNET_POLL_REPLY_LEN];
  memset(reply, 0, sizeof(reply));

  // Art-Net header: "Art-Net\0"
  reply[0] = 'A'; reply[1] = 'r'; reply[2] = 't';
  reply[3] = '-'; reply[4] = 'N'; reply[5] = 'e';
  reply[6] = 't'; reply[7] = 0;

  // OpCode ArtPollReply = 0x2100 (little-endian)
  reply[8] = 0x00; reply[9] = 0x21;

  // IP address of this device (big-endian, bytes 10-13)
  IPAddress ip = Ethernet.localIP();
  reply[10] = ip[0]; reply[11] = ip[1];
  reply[12] = ip[2]; reply[13] = ip[3];

  // Port (Art-Net = 6454, little-endian at bytes 14-15)
  reply[14] = 0x36; reply[15] = 0x19;  // 6454 = 0x1936

  // Net switch (byte 18), SubSwitch (byte 20), SwIn (byte 21)
  reply[18] = 0;
  reply[20] = 0;
  reply[21] = 0;

  // Short name (18 chars + null, bytes 26-43)
  const char *name = "UzomaBox T4.1";
  for (int i = 0; i < 18 && name[i]; i++) reply[26 + i] = name[i];

  // Long name (64 chars, bytes 44-107)
  const char *longName = "UzomaBox T4.1 16-output LED controller";
  for (int i = 0; i < 64 && longName[i]; i++) reply[44 + i] = longName[i];

  // Node report (64 chars, bytes 108-171)
  const char *report = "OK";
  for (int i = 0; i < 64 && report[i]; i++) reply[108 + i] = report[i];

  // NumPorts = 16 (byte 172)
  reply[172] = 16;

  // Port types (bytes 173-188): 0x80 = DMX output
  for (int i = 0; i < 16 && i < 16; i++) reply[173 + i] = 0x80;

  // GoodInput (bytes 189-204): all 0
  // GoodOutput (bytes 205-220): bit 2 = output enabled
  for (int i = 0; i < 16; i++) reply[205 + i] = 0x04;

  // SwIn (bytes 221-236): default to 0
  // SwOut (bytes 237-252): universe index
  for (int i = 0; i < 16; i++) {
    reply[237 + i] = _startUniverse[i] & 0xFF;
  }

  // Style = 0x00 (Controller, Art-Net spec)
  reply[251] = 0x00;

  // MAC address (bytes 252-257)
  // Use IP octets as placeholder (MAC not available via NativeEthernet)
  reply[252] = ip[0]; reply[253] = ip[1];
  reply[254] = ip[2]; reply[255] = ip[3];
  reply[256] = 0x00; reply[257] = 0x00;

  // Bind IP (bytes 258-261) = same as device IP
  reply[258] = ip[0]; reply[259] = ip[1];
  reply[260] = ip[2]; reply[261] = ip[3];

  // Bind index (byte 262) = 0
  // Status (byte 263) = 0

  // Send reply back to whoever sent the ArtPoll
  _udp.beginPacket(_udp.remoteIP(), _udp.remotePort());
  _udp.write(reply, ARTNET_POLL_REPLY_LEN);
  _udp.endPacket();
}
