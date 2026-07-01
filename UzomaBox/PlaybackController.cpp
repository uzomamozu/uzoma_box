#include "PlaybackController.h"

// ---------------------------------------------------------------------------
PlaybackController::PlaybackController()
  : _playing(false)
  , _recording(false)
  , _playlistCount(0)
  , _playlistIndex(0)
  , _framesPlayed(0)
  , _lastFrameTime(0)
  , _speedMult(1.0f)
  , _recordStartMs(0)
{
  _currentFile[0] = 0;
}

PlaybackController::~PlaybackController()
{
}

// ============================  PLAYBACK  ==================================

// ---------------------------------------------------------------------------
bool PlaybackController::playFile(const char *filename)
{
  stop();

  if (!sdFileOpen(filename, FILE_READ)) {
    return false;
  }
  strncpy(_currentFile, filename, 15);
  _currentFile[15] = 0;
  _playing = true;
  _framesPlayed = 0;
  _lastFrameTime = micros();
  _playlistCount = 0;      // no sequence, just a single file
  return true;
}

// ---------------------------------------------------------------------------
int PlaybackController::playSequence()
{
  stop();

  // Discover all .BIN files on the SD card
  _playlistCount = sdListBinFiles(_playlist, 64);
  if (_playlistCount == 0) return 0;

  _playlistIndex = 0;
  if (!openNextFile()) return 0;

  return _playlistCount;
}

// ---------------------------------------------------------------------------
bool PlaybackController::openNextFile()
{
  while (_playlistIndex < _playlistCount) {
    const char *fn = _playlist[_playlistIndex++];
    if (sdFileOpen(fn, FILE_READ)) {
      strncpy(_currentFile, fn, 15);
      _currentFile[15] = 0;
      _playing = true;
      _framesPlayed = 0;
      _lastFrameTime = micros();
      return true;
    }
  }
  // Exhausted all files in the playlist
  stop();
  return false;
}

// ---------------------------------------------------------------------------
bool PlaybackController::playNextFrame(uint32_t *frameTimeUs, uint16_t *pixelCount)
{
  if (!_playing) return false;

  // Use an iterative loop instead of recursion to prevent stack overflow
  // when looping files or skipping many audio frames.
  while (true) {

    uint8_t header[BIN_FRAME_HEADER_LEN];
    if (!sdCardRead(header, BIN_FRAME_HEADER_LEN)) {
      // End of file – loop back to beginning
      sdFileClose();
      if (_playlistCount > 0) {
        // Sequence mode: rewind to first file
        _playlistIndex = 0;
        if (openNextFile()) {
          continue;   // try again from new file
        }
      } else {
        // Single file mode: reopen the same file
        const char *fn = _currentFile;
        if (sdFileOpen(fn, FILE_READ)) {
          _lastFrameTime = micros();
          _framesPlayed = 0;
          continue;   // try again from start of file
        }
      }
      stop();
      return false;
    }

    if (header[0] == BIN_HEADER_VIDEO) {
      uint16_t pixCount = header[1] | (header[2] << 8);   // number of pixels
      uint16_t frameTime = header[3] | (header[4] << 8);  // µs

      // Scale frame time by speed multiplier:
      //   _speedMult 0.05 → wait 20x longer (slow motion)
      //   _speedMult 1.0  → normal speed
      //   _speedMult 5.0  → wait 5x shorter (fast forward)
      uint32_t scaledTime = (uint32_t)((float)frameTime / _speedMult);

      // Non-blocking frame timing: if not enough time has elapsed,
      // return false immediately. The caller will try again next loop().
      uint32_t now = micros();
      int32_t elapsed = (int32_t)(now - _lastFrameTime);
      if (elapsed < (int32_t)scaledTime) {
        return false;  // not yet time — caller should keep _playing = true
      }
      _lastFrameTime = now;
      _framesPlayed++;

      if (frameTimeUs) *frameTimeUs = frameTime;
      if (pixelCount)  *pixelCount  = pixCount;
      return true;

    } else if (header[0] == BIN_HEADER_AUDIO) {
      // Audio frame – skip it (we don't play audio)
      uint16_t audioSize = (header[1] | (header[2] << 8)) * 2;
      sdCardSkip(audioSize);
      continue;   // try next frame

    } else {
      // Unknown header – skip 1 byte and retry (should not happen)
      // Use direct seek to avoid per-byte buffered-read overhead
      sdFileSeekRelative(1);
      continue;   // try next frame
    }
  }
}

// ---------------------------------------------------------------------------
void PlaybackController::stop()
{
  _playing = false;
  _recording = false;
  sdFileClose();
  _playlistCount = 0;
  _playlistIndex = 0;
}

// ============================  RECORDING  =================================

// ---------------------------------------------------------------------------
bool PlaybackController::startRecording()
{
  if (_playing) stop();

  char filename[16];
  sdNextRecordFilename(filename, sizeof(filename));

  // O_WRITE | O_CREAT = create new file for writing (overwrite if exists)
  if (!sdFileOpen(filename, O_WRITE | O_CREAT)) {
    return false;
  }
  strncpy(_currentFile, filename, 15);
  _currentFile[15] = 0;
  _recording = true;
  _framesPlayed = 0;
  _recordStartMs = millis();
  return true;
}

// ---------------------------------------------------------------------------
bool PlaybackController::writeFrame(const uint8_t *rgbData, uint16_t totalPixels,
                                    uint32_t frameTimeUs)
{
  if (!_recording) return false;

  // Write frame header
  uint8_t header[BIN_FRAME_HEADER_LEN];
  header[0] = BIN_HEADER_VIDEO;
  header[1] = totalPixels & 0xFF;        // pixel count low
  header[2] = (totalPixels >> 8) & 0xFF; // pixel count high
  header[3] = frameTimeUs & 0xFF;        // time low
  header[4] = (frameTimeUs >> 8) & 0xFF; // time high

  if (!sdFileWrite(header, BIN_FRAME_HEADER_LEN)) {
    stopRecording();
    return false;
  }

  // Write pixel data (RGB triplets)
  if (!sdFileWrite(rgbData, totalPixels * 3)) {
    stopRecording();
    return false;
  }

  _framesPlayed++;
  return true;
}

// ---------------------------------------------------------------------------
bool PlaybackController::stopRecording()
{
  if (!_recording) return false;
  _recording = false;
  sdFileClose();
  return true;
}