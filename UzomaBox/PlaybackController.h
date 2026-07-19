#ifndef PlaybackController_h
#define PlaybackController_h

#include <Arduino.h>
#include "Pins.h"
#include "SDManager.h"

// .BIN file header constants
#define BIN_HEADER_VIDEO    '*'   // v1 (legacy): 1B type + 2B pixelCount + 2B frameTime = 5B header
#define BIN_HEADER_VIDEO_V2 '+'   // v2: 1B type + 2B pixelCount + 4B frameTime = 7B header
#define BIN_HEADER_AUDIO    '%'

// Frame header lengths
#define BIN_FRAME_HEADER_LEN    5   // v1
#define BIN_FRAME_HEADER_LEN_V2 7   // v2

// Maximum frame size: 7B header + MAX_LEDS_PER_STRIP × ACTIVE_OUTPUTS × 3
#define MAX_FRAME_SIZE          (BIN_FRAME_HEADER_LEN_V2 + MAX_LEDS_PER_STRIP * ACTIVE_OUTPUTS * 3)

// Double-buffer count for SD write buffering during recording
#define REC_BUFFER_COUNT  2

class PlaybackController {
public:
  PlaybackController();
  ~PlaybackController();

  // ---- Playback -----------------------------------------------------------
  bool playFile(const char *filename);
  int playSequence();

  // Advance one frame.  Returns true if a frame was displayed.
  // When true, pixel data has been copied into dest (pixelCount * 3 bytes).
  // frameTimeUs is filled with frame duration, pixelCount with pixel count.
  bool playNextFrame(uint8_t *dest, uint32_t *frameTimeUs, uint16_t *pixelCount);

  void stop();
  bool isPlaying() const { return _playing; }

  // ---- Recording ----------------------------------------------------------
  bool startRecording();
  bool writeFrame(const uint8_t *rgbData, uint16_t totalPixels, uint32_t frameTimeUs);
  bool stopRecording();
  bool isRecording() const { return _recording; }

  // ---- Recording time (elapsed seconds) ----------------------------------
  uint32_t getRecordTime() const {
    return _recording ? ((millis() - _recordStartMs) / 1000) : 0;
  }

  // ---- Speed control ------------------------------------------------------
  void setSpeed(float mult) { _speedMult = constrain(mult, 0.05f, 5.0f); }
  float getSpeed() const { return _speedMult; }

  // ---- Utility ------------------------------------------------------------
  const char *currentFilename() const { return _currentFile; }
  uint32_t framesPlayed() const { return _framesPlayed; }
  void resetFrameCount() { _framesPlayed = 0; }
  uint32_t filePosition() const { return sdFilePosition(); }
  uint32_t fileSize() const { return sdFileSize(); }

private:
  bool openNextFile();
  bool _flushRecBuffer();

  // Internal state
  bool          _playing;
  bool          _recording;
  char          _currentFile[32];
  char          _playlist[64][16];
  int           _playlistCount;
  int           _playlistIndex;
  uint32_t      _framesPlayed;
  uint32_t      _lastFrameTime;         // micros() at last shown frame
  float         _speedMult;

  // Pending frame buffer — stores one full frame (header + pixels) when
  // timing check fails, so we don't need to seek back in the SD file.
  uint8_t       _pendingBuf[MAX_FRAME_SIZE];
  bool          _framePending;          // true when _pendingBuf has data
  uint16_t      _pendingPixCount;       // pixel count of pending frame
  uint32_t      _pendingFrameTime;      // frameTimeUs of pending frame

  uint32_t      _recordStartMs;

  // Recording double-buffer state
  uint8_t       _recBufIdx;
  bool          _recBufDirty[REC_BUFFER_COUNT];
  uint16_t      _recBufLen[REC_BUFFER_COUNT];

  DMAMEM static uint8_t s_recBuffer[REC_BUFFER_COUNT][MAX_FRAME_SIZE];
};

#endif