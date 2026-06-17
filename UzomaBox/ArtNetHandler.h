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
  void setLedsPerStrip(uint16_t n) { _ledsPerStrip = n; _totalPixels = n * 8; }

private:
  // Parse a single ArtDMX packet and write DMX data into _frameBuffer
  void processPacket(const uint8_t *packet, int len);

  EthernetUDP   _udp;
  uint8_t       _packetBuffer[ARTNET_HEADER_LEN + ARTNET_DMX_LEN + 4];

  uint16_t      _startUniverse[8];  // first universe for each of the 8 strips
  uint16_t      _ledsPerStrip;
  uint16_t      _totalPixels;
  uint8_t      *_frameBuffer;       // RGB interleaved, totalPixels * 3 bytes
  bool          _receiving;
  uint32_t      _lastPacketTime;    // ms

  FrameCallback _frameCb;
  bool          _universeReceived[8]; // per-strip flag
  bool          _allUpdated;           // true if all active strips received data
};

#endif