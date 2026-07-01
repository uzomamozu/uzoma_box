#include "UdpDiscovery.h"

// ---------------------------------------------------------------------------
UdpDiscovery::UdpDiscovery()
{
}

UdpDiscovery::~UdpDiscovery()
{
}

// ---------------------------------------------------------------------------
void UdpDiscovery::begin()
{
  _udp.begin(DISCOVERY_PORT);
}

// ---------------------------------------------------------------------------
void UdpDiscovery::poll(const char *nickname, const char *model, const char *fwVersion,
                         const IPAddress &localIp, int temperature)
{
  int packetSize = _udp.parsePacket();
  if (packetSize > 0) {
    int len = _udp.read(_buffer, sizeof(_buffer) - 1);
    if (len > 0) {
      _buffer[len] = 0;
      // Check if it's a discovery request
      if (strcmp((const char *)_buffer, DISCOVERY_REQUEST) == 0) {
        // Build response: comma-separated key=value pairs
        char response[256];
        snprintf(response, sizeof(response),
          "MODEL=%.31s,NICK=%.31s,IP=%d.%d.%d.%d,FW=%.15s,TEMP=%d",
          model,
          nickname,
          localIp[0], localIp[1], localIp[2], localIp[3],
          fwVersion,
          temperature
        );
        // Send response back to the requester
        _udp.beginPacket(_udp.remoteIP(), _udp.remotePort());
        _udp.write((const uint8_t *)response, strlen(response));
        _udp.endPacket();
      }
    }
  }
}