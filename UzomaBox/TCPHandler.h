#ifndef TCPHandler_h
#define TCPHandler_h

#include <Arduino.h>
#include <NativeEthernet.h>

// TCP server port for desktop app communication
#define TCP_PORT  8888

// Maximum command line length
#define CMD_BUFFER_SIZE  256

// Response queue: up to 8 pending responses, 128 bytes each
#define RESP_QUEUE_COUNT  8
#define RESP_QUEUE_LEN    128

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

  // Send a response to the client (non-blocking — queues if buffer full)
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

  // Response ring buffer — prevents silently dropped messages
  char           _respQueue[RESP_QUEUE_COUNT][RESP_QUEUE_LEN];
  uint8_t        _queueHead;   // write index
  uint8_t        _queueTail;   // read index
  uint8_t        _queueCount;  // items pending

  // Flush queued responses to the TCP socket (call from poll())
  void _flushQueue();
};

// Command tokens returned by poll()
enum TCPCommand {
  CMD_NONE        = 0,
  CMD_MODE_ARTNET,
  CMD_MODE_PLAYBACK,
  CMD_MODE_RECORD,
  CMD_MODE_TEST,
  CMD_REC_START,
  CMD_REC_STOP,
  CMD_CONFIG,
  CMD_PLAY_FILE,
  CMD_PLAY_SEQUENCE,
  CMD_STOP,
  CMD_SPEED,
  CMD_LIST,
  CMD_DELETE,
  CMD_IDENTIFY,
  CMD_STATUS,
  CMD_PING,
  CMD_TEST_PATTERN,
  CMD_TEST_OUTPUT,
  CMD_REC_START_MODE,
  CMD_REC_STOP_MODE,
  CMD_REC_TRIGGER_UNIV,
  CMD_REC_TRIGGER_CH,
  CMD_REC_STOP_SECS,
  CMD_REC_ARM,
  CMD_UNKNOWN
};

// Parse a command string into a TCPCommand token
int parseCommand(const char *cmd);

#endif