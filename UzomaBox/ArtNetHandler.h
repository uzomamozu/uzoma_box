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

// Maximum pixel capacity (512 LEDs/strip × 16 strips)
#define MAX_TOTAL_PIXELS     (512 * 16)
#define FRAME_BUFFER_SIZE    (MAX_TOTAL_PIXELS * 3)  // 24576 bytes

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
  // rgbData contains all 16 strips interleaved in strip-major order.
  typedef void (*FrameCallback)(const uint8_t *, uint16_t);

  void setFrameCallback(FrameCallback cb);

  // Get the raw RGB buffer for recording (totalPixels * 3 bytes)
  const uint8_t *getFrameBuffer() const { return s_frameBuffer; }
  uint16_t       getFrameSize()   const { return _totalPixels * 3; }

  // Check if we are actively receiving ArtNet
  bool isReceiving() const { return _receiving; }

  // Reset receiving timeout
  void resetTimeout();

  // Update start universe mapping (array of 16 entries)
  void setUniverseMapping(const uint16_t startUniv[16]);

  // Number of LEDs per strip (needs to be set before begin or on config change)
  void setLedsPerStrip(uint16_t n);

private:
  // Static DMAMEM frame buffer — no heap allocation
  // Shared across all instances; only one ArtNetHandler should exist.
  static DMAMEM uint8_t s_frameBuffer[FRAME_BUFFER_SIZE];

  // Reset all per-frame flags and state
  void resetFrameState();

  // Parse a single ArtDMX packet and write DMX data into the frame buffer
  void processPacket(const uint8_t *packet, int len);

  // Fire the frame callback and reset for next frame
  void flushFrame();

  // Universes per strip for current ledWidth: ceil(ledsPerStrip * 3 / 512)
  uint8_t universesPerStrip() const { return _universesPerStrip; }

  // Compute LED offset and count for a given sub-universe index within a strip
  void subUniverseRange(uint8_t sub, uint16_t &offset, uint16_t &count) const;

  EthernetUDP   _udp;
  uint8_t       _packetBuffer[ARTNET_HEADER_LEN + ARTNET_DMX_LEN + 4];

  uint16_t      _startUniverse[16];               // first universe for each of the 16 strips
  uint16_t      _ledsPerStrip;
  uint16_t      _totalPixels;
  // _frameBuffer replaced by static s_frameBuffer above
  bool          _receiving;
  uint32_t      _lastPacketTime;                 // ms
  uint8_t       _universesPerStrip;

  FrameCallback _frameCb;

  // Per-frame assembly tracking
  bool          _universeReceived[16];            // true when strip has all sub-universes
  bool          _universeSubReceived[16][MAX_UNIVERSES_PER_STRIP]; // per sub-universe flag
  bool          _allUpdated;                     // true if all strips have data
  bool          _frameStarted;                   // true once at least 1 sub-universe arrived
  uint32_t      _frameStartTime;                 // ms when first sub-universe of this frame arrived
};

#endif