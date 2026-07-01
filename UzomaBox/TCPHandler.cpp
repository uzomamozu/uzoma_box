#include "TCPHandler.h"
#include <string.h>

// ---------------------------------------------------------------------------
TCPHandler::TCPHandler()
  : _server(TCP_PORT)
  , _connected(false)
  , _linePos(0)
  , _queueHead(0)
  , _queueTail(0)
  , _queueCount(0)
{
  _lineBuffer[0] = 0;
}

TCPHandler::~TCPHandler()
{
}

// ---------------------------------------------------------------------------
void TCPHandler::begin()
{
  _server.begin();
}

// ---------------------------------------------------------------------------
int TCPHandler::poll(char *cmdBuffer, unsigned int bufSize)
{
  // Accept new client if none connected
  if (!_connected) {
    _client = _server.available();
    if (_client) {
      _connected = true;
      _linePos = 0;
      _lineBuffer[0] = 0;
      _queueHead = _queueTail = _queueCount = 0;
      sendResponse("OK:connected");
    }
    cmdBuffer[0] = 0;
    return CMD_NONE;
  }

  // Check if client disconnected
  if (!_client.connected()) {
    _client.stop();
    _connected = false;
    _linePos = 0;
    _queueHead = _queueTail = _queueCount = 0;
    cmdBuffer[0] = 0;
    return CMD_NONE;
  }

  // Attempt to flush any queued responses before reading new commands
  _flushQueue();

  // Read available data
  while (_client.available()) {
    char c = (char)_client.read();

    if (c == '\n' || c == '\r') {
      // End of line – process if we have data
      if (_linePos > 0) {
        _lineBuffer[_linePos] = 0;
        _linePos = 0;

        // Copy command to output buffer
        strncpy(cmdBuffer, _lineBuffer, bufSize - 1);
        cmdBuffer[bufSize - 1] = 0;

        // Parse and return command token
        return parseCommand(_lineBuffer);
      }
    } else if (_linePos < CMD_BUFFER_SIZE - 1) {
      _lineBuffer[_linePos++] = c;
    }
  }

  cmdBuffer[0] = 0;
  return CMD_NONE;
}

// ---------------------------------------------------------------------------
void TCPHandler::sendResponse(const char *msg)
{
  if (!_connected || !_client.connected()) return;

  int len = strlen(msg);

  // Fast path: send immediately if no backlog and buffer has room
  if (_queueCount == 0 && _client.availableForWrite() >= (unsigned int)(len + 2)) {
    _client.println(msg);
    return;
  }

  // Slow path: queue for later if room in the ring buffer
  if (_queueCount < RESP_QUEUE_COUNT) {
    strncpy(_respQueue[_queueHead], msg, RESP_QUEUE_LEN - 1);
    _respQueue[_queueHead][RESP_QUEUE_LEN - 1] = 0;
    _queueHead = (_queueHead + 1) % RESP_QUEUE_COUNT;
    _queueCount++;
  }
  // If queue is full, drop the message (unlikely with 8-slot buffer)
}

// ---------------------------------------------------------------------------
void TCPHandler::_flushQueue()
{
  while (_queueCount > 0 && _client.availableForWrite() >= 64) {
    _client.println(_respQueue[_queueTail]);
    _queueTail = (_queueTail + 1) % RESP_QUEUE_COUNT;
    _queueCount--;
  }
}

// ---------------------------------------------------------------------------
void TCPHandler::disconnect()
{
  if (_client) {
    _client.stop();
  }
  _connected = false;
  _linePos = 0;
  _queueHead = _queueTail = _queueCount = 0;
}

// ---------------------------------------------------------------------------
// Parse a command string into a TCPCommand token
// ---------------------------------------------------------------------------
int parseCommand(const char *cmd)
{
  // Skip leading whitespace
  while (*cmd == ' ' || *cmd == '\t') cmd++;

  if      (!strcmp(cmd, "MODE:artnet"))    return CMD_MODE_ARTNET;
  else if (!strcmp(cmd, "MODE:playback"))  return CMD_MODE_PLAYBACK;
  else if (!strcmp(cmd, "MODE:record"))    return CMD_MODE_RECORD;
  else if (!strcmp(cmd, "MODE:test"))      return CMD_MODE_TEST;
  else if (!strcmp(cmd, "REC:START"))      return CMD_REC_START;
  else if (!strcmp(cmd, "REC:STOP"))       return CMD_REC_STOP;
  else if (!strncmp(cmd, "CONFIG:", 7))    return CMD_CONFIG;
  else if (!strncmp(cmd, "PLAY:", 5))      return CMD_PLAY_FILE;
  else if (!strcmp(cmd, "PLAY:SEQUENCE"))  return CMD_PLAY_SEQUENCE;
  else if (!strcmp(cmd, "STOP"))           return CMD_STOP;
  else if (!strcmp(cmd, "STATUS"))         return CMD_STATUS;
  else if (!strncmp(cmd, "SPEED:", 6))    return CMD_SPEED;
  else if (!strcmp(cmd, "LIST"))           return CMD_LIST;
  else if (!strncmp(cmd, "DELETE:", 7))    return CMD_DELETE;
  else if (!strcmp(cmd, "IDENTIFY"))       return CMD_IDENTIFY;
  else if (!strcmp(cmd, "PING"))           return CMD_PING;
  else if (!strncmp(cmd, "COMMAND:TEST_PATTERN=", 20)) return CMD_TEST_PATTERN;
  else if (!strncmp(cmd, "COMMAND:TEST_OUTPUT=", 20))  return CMD_TEST_OUTPUT;
  else if (!strncmp(cmd, "REC:START_MODE=", 15))      return CMD_REC_START_MODE;
  else if (!strncmp(cmd, "REC:STOP_MODE=", 14))       return CMD_REC_STOP_MODE;
  else if (!strncmp(cmd, "REC:TRIGGER_UNIV=", 17))    return CMD_REC_TRIGGER_UNIV;
  else if (!strncmp(cmd, "REC:TRIGGER_CH=", 15))      return CMD_REC_TRIGGER_CH;
  else if (!strncmp(cmd, "REC:STOP_SECS=", 14))       return CMD_REC_STOP_SECS;
  else if (!strcmp(cmd, "REC:ARM"))                    return CMD_REC_ARM;
  else                                     return CMD_UNKNOWN;
}