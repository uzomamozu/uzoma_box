#ifndef ArtNetHandler_h
#define ArtNetHandler_h

#include <Arduino.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>

// ArtNet constants
#define ARTNET_PORT         6454
#define ARTNET_HEADER_LEN   18   // ArtDMX packet header before DMX data
#define ARTNET_DMX_LEN      512  // DMX channels per universe
#define ARTNET_OP_DMX       0x5000

// Maximum universes per strip (safety for up to 682 LEDs: 682*3=2046, ceil/512=4)
#define MAX_UNIVERSES_PER_STRIP  4

// Maximum time (ms) to wait for a complete frame before flushing partial data
// Prevents LED freeze when one universe is lost
#define FRAME_TIMEOUT_MS     50

class ArtNetHandler {
public:
  ArtNetHandler();
  ~ArtNetHandler();

  // Initialise UDP socket
  void begin();

  // Poll for incoming ArtNet packets.
  // Returns the number of complete universes received this call.
  int poll();

  // Callback: user-provided function called when a new frame is assembled.
  // Parameters: (const uint8_t *rgbData, uint16_t totalPixels)
  // rgbData contains all 8 strips interleaved in strip-major order.
  typedef void (*FrameCallback)(const uint8_t *, uint16_t);

  void setFrameCallback(FrameCallback cb);

  // Get the raw RGB buffer for recording (totalPixels * 3 bytes)
  const uint8_t *getFrameBuffer() const { return _frameBuffer; }
  uint16_t       getFrameSize()   const { return _totalPixels * 3; }

  // Check if we are actively receiving ArtNet
  bool isReceiving() const { return _receiving; }

  // Reset receiving timeout
  void resetTimeout();

  // Update start universe mapping
  void setUniverseMapping(const uint16_t startUniv[8]);

  // Number of LEDs per strip (needs to be set before begin or on config change)
  void setLedsPerStrip(uint16_t n);

private:
  // Reset all per-frame flags and state
  void resetFrameState();

  // Parse a single ArtDMX packet and write DMX data into _frameBuffer
  void processPacket(const uint8_t *packet, int len);

  // Fire the frame callback and reset for next frame
  void flushFrame();

  // Universes per strip for current ledWidth: ceil(ledsPerStrip * 3 / 512)
  uint8_t universesPerStrip() const { return _universesPerStrip; }

  // Compute LED offset and count for a given sub-universe index within a strip
  void subUniverseRange(uint8_t sub, uint16_t &offset, uint16_t &count) const;

  EthernetUDP   _udp;
  uint8_t       _packetBuffer[ARTNET_HEADER_LEN + ARTNET_DMX_LEN + 4];

  uint16_t      _startUniverse[8];               // first universe for each of the 8 strips
  uint16_t      _ledsPerStrip;
  uint16_t      _totalPixels;
  uint8_t      *_frameBuffer;                    // RGB interleaved, totalPixels * 3 bytes
  bool          _receiving;
  uint32_t      _lastPacketTime;                 // ms
  uint8_t       _universesPerStrip;

  FrameCallback _frameCb;

  // Per-frame assembly tracking
  bool          _universeReceived[8];            // true when strip has all sub-universes
  bool          _universeSubReceived[8][MAX_UNIVERSES_PER_STRIP]; // per sub-universe flag
  bool          _allUpdated;                     // true if all strips have data
  bool          _frameStarted;                   // true once at least 1 sub-universe arrived
  uint32_t      _frameStartTime;                 // ms when first sub-universe of this frame arrived
};

#endif