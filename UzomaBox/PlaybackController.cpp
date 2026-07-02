#include "PlaybackController.h"

// Static DMAMEM recording buffers
DMAMEM uint8_t PlaybackController::s_recBuffer[REC_BUFFER_COUNT][MAX_FRAME_SIZE];

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
  , _recBufIdx(0)
{
  _currentFile[0] = 0;
  _recBufDirty[0] = false;
  _recBufDirty[1] = false;
  _recBufLen[0]   = 0;
  _recBufLen[1]   = 0;
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
  strncpy(_currentFile, filename, sizeof(_currentFile) - 1);
  _currentFile[sizeof(_currentFile) - 1] = 0;
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
      strncpy(_currentFile, fn, sizeof(_currentFile) - 1);
      _currentFile[sizeof(_currentFile) - 1] = 0;
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
// Autodetect v1 vs v2 frame format by checking the type byte:
//   '*' (0x2A) → v1: 1B type + 2B pixelCount + 2B frameTime = 5B header
//   '+' (0x2B) → v2: 1B type + 2B pixelCount + 4B frameTime = 7B header
// ---------------------------------------------------------------------------
bool PlaybackController::playNextFrame(uint32_t *frameTimeUs, uint16_t *pixelCount)
{
  if (!_playing) return false;

  // Use an iterative loop instead of recursion to prevent stack overflow
  // when looping files or skipping many audio frames.
  while (true) {

    // Read just the first byte to determine format
    uint8_t typeByte;
    if (!sdCardRead(&typeByte, 1)) {
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

    // Autodetect format
    if (typeByte == BIN_HEADER_VIDEO || typeByte == BIN_HEADER_VIDEO_V2) {
      // Read the rest of the header
      uint8_t extra[6];
      uint8_t extraLen = (typeByte == BIN_HEADER_VIDEO) ? 4 : 6;  // v1: 4 more bytes, v2: 6 more bytes
      if (!sdCardRead(extra, extraLen)) {
        sdFileClose();
        stop();
        return false;
      }

      uint16_t pixCount = extra[0] | (extra[1] << 8);   // number of pixels (2B, same for both)
      uint32_t frameTime;

      if (typeByte == BIN_HEADER_VIDEO) {
        // v1: 16-bit frameTime
        frameTime = extra[2] | (extra[3] << 8);
      } else {
        // v2: 32-bit frameTime
        frameTime = (uint32_t)extra[2] | ((uint32_t)extra[3] << 8) |
                    ((uint32_t)extra[4] << 16) | ((uint32_t)extra[5] << 24);
      }

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
        // Seek back so we re-read this frame next time
        sdFileSeekRelative(-1 - (int32_t)extraLen);
        return false;  // not yet time — caller should keep _playing = true
      }
      _lastFrameTime = now;
      _framesPlayed++;

      if (frameTimeUs) *frameTimeUs = frameTime;
      if (pixelCount)  *pixelCount  = pixCount;
      return true;

    } else if (typeByte == BIN_HEADER_AUDIO) {
      // Audio frame – skip it (we don't play audio)
      uint8_t audioSizeBuf[2];
      if (!sdCardRead(audioSizeBuf, 2)) {
        sdFileClose();
        stop();
        return false;
      }
      uint16_t audioSize = audioSizeBuf[0] | (audioSizeBuf[1] << 8);
      sdCardSkip(audioSize * 2);
      continue;   // try next frame

    } else {
      // Unknown header – skip 1 byte and retry (should not happen)
      continue;   // try next frame (byte already consumed by typeByte read)
    }
  }
}

// ---------------------------------------------------------------------------
void PlaybackController::stop()
{
  // Flush any pending recording buffer before closing
  if (_recording) {
    _flushRecBuffer();
  }
  _playing = false;
  _recording = false;
  sdFileClose();
  _playlistCount = 0;
  _playlistIndex = 0;
  _recBufDirty[0] = false;
  _recBufDirty[1] = false;
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
  strncpy(_currentFile, filename, sizeof(_currentFile) - 1);
  _currentFile[sizeof(_currentFile) - 1] = 0;
  _recording = true;
  _framesPlayed = 0;
  _recordStartMs = millis();

  // Initialise double-buffer state
  _recBufIdx = 0;
  _recBufDirty[0] = false;
  _recBufDirty[1] = false;
  _recBufLen[0]   = 0;
  _recBufLen[1]   = 0;

  return true;
}

// ---------------------------------------------------------------------------
// Flush the non-current buffer (the one we're NOT writing to) to SD.
// This is called after writing into the current buffer, so the previous
// frame's data is written to disk while we capture the next frame.
// ---------------------------------------------------------------------------
bool PlaybackController::_flushRecBuffer()
{
  // Determine which buffer to flush: the one we're NOT currently writing
  uint8_t flushIdx = _recBufIdx ^ 1;

  if (!_recBufDirty[flushIdx]) {
    return true;  // nothing to flush
  }

  if (!sdFileWrite(s_recBuffer[flushIdx], _recBufLen[flushIdx])) {
    // SD write failed — stop recording
    stopRecording();
    return false;
  }

  _recBufDirty[flushIdx] = false;
  _recBufLen[flushIdx] = 0;
  return true;
}

// ---------------------------------------------------------------------------
bool PlaybackController::writeFrame(const uint8_t *rgbData, uint16_t totalPixels,
                                    uint32_t frameTimeUs)
{
  if (!_recording) return false;

  // Build v2 frame in current buffer: '+' + pixelCount(2B LE) + frameTime(4B LE)
  uint8_t *buf = s_recBuffer[_recBufIdx];
  buf[0] = BIN_HEADER_VIDEO_V2;              // '+' = v2 marker
  buf[1] = totalPixels & 0xFF;               // pixel count low
  buf[2] = (totalPixels >> 8) & 0xFF;        // pixel count high
  buf[3] = frameTimeUs & 0xFF;               // time byte 0
  buf[4] = (frameTimeUs >> 8) & 0xFF;        // time byte 1
  buf[5] = (frameTimeUs >> 16) & 0xFF;       // time byte 2
  buf[6] = (frameTimeUs >> 24) & 0xFF;       // time byte 3

  // Copy pixel data after header
  uint16_t pixelBytes = totalPixels * 3;
  memcpy(buf + BIN_FRAME_HEADER_LEN_V2, rgbData, pixelBytes);

  // Mark current buffer as dirty
  _recBufLen[_recBufIdx] = BIN_FRAME_HEADER_LEN_V2 + pixelBytes;
  _recBufDirty[_recBufIdx] = true;

  // Flush the OTHER buffer to SD (the previous frame's data)
  if (!_flushRecBuffer()) {
    return false;
  }

  // Toggle buffer index for next frame
  _recBufIdx ^= 1;

  _framesPlayed++;
  return true;
}

// ---------------------------------------------------------------------------
bool PlaybackController::stopRecording()
{
  if (!_recording) return false;

  // Flush any remaining dirty buffers before closing
  // Flush current buffer first, then the other one
  _flushRecBuffer();  // flushes _recBufIdx ^ 1

  // Now flush the current buffer (it has this frame's data)
  _recBufIdx ^= 1;  // toggle to make current buffer the "other" one
  _flushRecBuffer();

  _recording = false;
  sdFileClose();
  _recBufDirty[0] = false;
  _recBufDirty[1] = false;
  return true;
}