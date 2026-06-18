#ifndef PlaybackController_h
#define PlaybackController_h

#include <Arduino.h>
#include "SDManager.h"

// .BIN file header constants
#define BIN_HEADER_VIDEO   '*'
#define BIN_HEADER_AUDIO   '%'

// Frame header:  1 byte type + 2 bytes pixel count (LE) + 2 bytes frame time µs (LE)
#define BIN_FRAME_HEADER_LEN  5

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
  // rgbData: RGB byte-triplets for all 8 strips (totalPixels * 3 bytes)
  // frameTimeUs: frame duration in microseconds (use 0 for live capture)
  bool writeFrame(const uint8_t *rgbData, uint16_t totalPixels, uint32_t frameTimeUs);

  // Stop recording and close the file
  bool stopRecording();

  // Check if recording is active
  bool isRecording() const { return _recording; }

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

  // Internal state
  bool          _playing;
  bool          _recording;
  char          _currentFile[16];       // current playback filename
  char          _playlist[64][16];      // sequence of .BIN files
  int           _playlistCount;
  int           _playlistIndex;
  uint32_t      _framesPlayed;
  uint32_t      _lastFrameTime;         // micros() at last frame
  float         _speedMult;             // playback speed multiplier (0.05-5.0)
};

#endif