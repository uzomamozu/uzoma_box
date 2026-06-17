#ifndef TCPHandler_h
#define TCPHandler_h

#include <Arduino.h>
#include <NativeEthernet.h>

// TCP server port for desktop app communication
#define TCP_PORT  8888

// Maximum command line length
#define CMD_BUFFER_SIZE  256

class TCPHandler {
public:
  TCPHandler();
  ~TCPHandler();

  // Start the TCP server
  void begin();

  // Poll for incoming connections and commands.
  // Returns a command token: 0 = no command, other values = command type.
  // cmdBuffer is filled with the full command string.
  int poll(char *cmdBuffer, unsigned int bufSize);

  // Send a response to the client
  void sendResponse(const char *msg);

  // Check if a client is connected
  bool isConnected() const { return _connected; }

  // Close the current client connection
  void disconnect();

private:
  EthernetServer _server;
  EthernetClient _client;
  bool           _connected;
  char           _lineBuffer[CMD_BUFFER_SIZE];
  unsigned int   _linePos;
};

// Command tokens returned by poll()
enum TCPCommand {
  CMD_NONE        = 0,
  CMD_MODE_ARTNET,
  CMD_MODE_PLAYBACK,
  CMD_MODE_RECORD,
  CMD_REC_START,
  CMD_REC_STOP,
  CMD_CONFIG,
  CMD_PLAY_FILE,
  CMD_PLAY_SEQUENCE,
  CMD_STOP,
  CMD_STATUS,
  CMD_UNKNOWN
};

// Parse a command string into a TCPCommand token
int parseCommand(const char *cmd);

#endif
