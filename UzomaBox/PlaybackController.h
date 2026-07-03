#ifndef PlaybackController_h
#define PlaybackController_h

#include <Arduino.h>
#include "SDManager.h"

// .BIN file header constants
#define BIN_HEADER_VIDEO    '*'   // v1 (legacy): 1B type + 2B pixelCount + 2B frameTime = 5B header
#define BIN_HEADER_VIDEO_V2 '+'   // v2: 1B type + 2B pixelCount + 4B frameTime = 7B header
#define BIN_HEADER_AUDIO    '%'

// Frame header lengths
#define BIN_FRAME_HEADER_LEN    5   // v1
#define BIN_FRAME_HEADER_LEN_V2 7   // v2

// Maximum frame size: 7B header + 512 LEDs/strip × 16 strips × 3 bytes = 24,576 + 7
#define MAX_FRAME_SIZE          (BIN_FRAME_HEADER_LEN_V2 + 512 * 16 * 3)

// Double-buffer count for SD write buffering during recording
#define REC_BUFFER_COUNT  2

class PlaybackController {
public:
  PlaybackController();
  ~PlaybackController();

  // ---- Playback -----------------------------------------------------------

  // Prepare to play back a single .BIN file
  bool playFile(const char *filename);

  // Prepare to play back all .BIN files sequentially
  int playSequence();

  // Advance one frame.  Returns true if a frame was displayed.
  // Returns false at EOF (file automatically advances to next in sequence).
  // On success, frameTimeUs is filled with frame duration, pixelCount with
  // the number of pixels in this frame (caller must read pixelCount*3 bytes
  // via sdCardRead() immediately after).
  bool playNextFrame(uint32_t *frameTimeUs = nullptr, uint16_t *pixelCount = nullptr);

  // Stop playback
  void stop();

  // Check if playback is active
  bool isPlaying() const { return _playing; }

  // ---- Recording ----------------------------------------------------------

  // Start recording: create a new .BIN file
  bool startRecording();

  // Write one video frame to the .BIN file.
  // rgbData: RGB byte-triplets for all 16 strips (totalPixels * 3 bytes)
  // frameTimeUs: frame duration in microseconds (use 0 for live capture)
  bool writeFrame(const uint8_t *rgbData, uint16_t totalPixels, uint32_t frameTimeUs);

  // Stop recording and close the file
  bool stopRecording();

  // Check if recording is active
  bool isRecording() const { return _recording; }

  // ---- Recording time (elapsed seconds) ----------------------------------
  uint32_t getRecordTime() const {
    return _recording ? ((millis() - _recordStartMs) / 1000) : 0;
  }

  // ---- Speed control ------------------------------------------------------

  // Set playback speed multiplier (0.05 = 1/20x, 1.0 = normal, 5.0 = 5x)
  void setSpeed(float mult) { _speedMult = constrain(mult, 0.05f, 5.0f); }
  float getSpeed() const { return _speedMult; }

  // ---- Utility ------------------------------------------------------------

  // Get the current playback filename
  const char *currentFilename() const { return _currentFile; }

  // Get total frames played so far in current file
  uint32_t framesPlayed() const { return _framesPlayed; }

  // Reset frame counter
  void resetFrameCount() { _framesPlayed = 0; }

  // File progress (bytes)
  uint32_t filePosition() const { return sdFilePosition(); }
  uint32_t fileSize() const { return sdFileSize(); }

private:
  // Open the next file in the sequence
  bool openNextFile();

  // Flush any pending recording buffer to SD
  bool _flushRecBuffer();

  // Internal state
  bool          _playing;
  bool          _recording;
  char          _currentFile[32];       // current playback filename
  char          _playlist[64][16];      // sequence of .BIN files
  int           _playlistCount;
  int           _playlistIndex;
  uint32_t      _framesPlayed;
  uint32_t      _lastFrameTime;         // micros() at last frame
  float         _speedMult;             // playback speed multiplier (0.05-5.0)
  uint32_t      _recordStartMs;         // millis() when recording started

  // Recording double-buffer state
  uint8_t       _recBufIdx;             // current write buffer index (0 or 1)
  bool          _recBufDirty[REC_BUFFER_COUNT];  // true when buffer has pending data
  uint16_t      _recBufLen[REC_BUFFER_COUNT];    // bytes pending per buffer

  // DMAMEM recording buffers (one for active capture, one for SD flush)
  DMAMEM static uint8_t s_recBuffer[REC_BUFFER_COUNT][MAX_FRAME_SIZE];
};

#endif