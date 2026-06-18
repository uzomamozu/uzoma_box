#ifndef UdpDiscovery_h
#define UdpDiscovery_h

#include <Arduino.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>

// UDP discovery port for the desktop assistant to find controllers
#define DISCOVERY_PORT     7777
#define DISCOVERY_REQUEST  "UZOMA:SEARCH"

class UdpDiscovery {
public:
  UdpDiscovery();
  ~UdpDiscovery();

  // Start listening on DISCOVERY_PORT
  void begin();

  // Poll for incoming discovery requests and respond.
  // Call this regularly from loop().
  // Parameters passed in for the response packet.
  void poll(const char *nickname, const char *model, const char *fwVersion, const IPAddress &localIp, int temperature);

private:
  EthernetUDP _udp;
  uint8_t     _buffer[256];
};

#endif