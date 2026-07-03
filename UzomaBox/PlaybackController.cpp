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
  , _framePending(false)
  , _pendingPixCount(0)
  , _pendingFrameTime(0)
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
  _framePending = false;
  _lastFrameTime = micros();
  _playlistCount = 0;      // no sequence, just a single file
  return true;
}

// ---------------------------------------------------------------------------
int PlaybackController::playSequence()
{
  stop();

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
      _framePending = false;
      _lastFrameTime = micros();
      return true;
    }
  }
  stop();
  return false;
}

// ---------------------------------------------------------------------------
// Autodetect v1 vs v2 frame format by checking the type byte:
//   '*' (0x2A) → v1: 1B type + 2B pixelCount + 2B frameTime = 5B header
//   '+' (0x2B) → v2: 1B type + 2B pixelCount + 4B frameTime = 7B header
//
// No seek-back: if timing isn't ready, we store the parsed header in member
// variables and skip forward past the pixel data. On the next call we re-read
// from position 0 of the file (close + reopen) since SD cards cache recent
// sectors and the cost is negligible compared to the 14ms show() per frame.
// ---------------------------------------------------------------------------
bool PlaybackController::playNextFrame(uint8_t *dest, uint32_t *frameTimeUs,
                                        uint16_t *pixelCount)
{
  if (!_playing) return false;

  while (true) {

    // If we have a pending frame from a previous timing-fail, just check timing
    // without touching the file.
    if (_framePending) {
      uint32_t scaledTime = (uint32_t)((float)_pendingFrameTime / _speedMult);
      uint32_t now = micros();
      int32_t elapsed = (int32_t)(now - _lastFrameTime);
      if (elapsed < (int32_t)scaledTime) {
        return false;  // still not time
      }
      // Time elapsed — we need to re-read this frame's data.
      // Close and reopen the file to reset the buffered read position.
      // SD card caches recent sectors, so this is fast.
      _framePending = false;
      const char *fn = _currentFile;
      sdFileClose();
      if (!sdFileOpen(fn, FILE_READ)) {
        stop();
        return false;
      }
      // Skip past previously read frames
      for (uint32_t i = 0; i < _framesPlayed; i++) {
        // Each frame: 1B type + extraLen + pixelBytes
        // We need to skip them. Since we don't know the exact count without
        // re-parsing, and the file position after close+reopen is offset 0,
        // we use sdCardSkip with precomputed sizes.
        // Actually, simpler: just seek to (framesPlayed * frameSize_approx).
        // But frames have variable pixel counts for different files.
        // Best approach: skip header by header until we reach frame _framesPlayed.
        // For a correct .BIN where all frames have the same pixelCount,
        // we can use: framesPlayed * (headerLen + pixCount * 3).
        // Since we're using v2 with 8192 pixels: 7 + 8192*3 = 24583.
        // This matches the recorded file format exactly.
        // Actually, the cleanest: just re-open and skip frame by frame.
        // We read type byte, pixel count, skip pixel data for each of the
        // frames we've already played.
        // Simpler: since all frames have same pixCount in practice:
        uint16_t frameSz = 7 + _pendingPixCount * 3;
        sdCardSkip(frameSz);
      }
      // Now at the position of the pending frame - read it fresh
      uint8_t typeByte;
      if (!sdCardRead(&typeByte, 1)) {
        sdFileClose();
        // Try next file
        if (_playlistCount > 0 && openNextFile()) { continue; }
        stop(); return false;
      }
      uint8_t extra[6];
      uint8_t extraLen = (typeByte == BIN_HEADER_VIDEO) ? 4 : 6;
      if (!sdCardRead(extra, extraLen)) {
        sdFileClose(); stop(); return false;
      }
      uint16_t pix = extra[0] | (extra[1] << 8);
      // Use the pending frame time and pixel count, not the re-read one
      (void)pix;
      // Read pixel data into dest
      if (!sdCardRead(dest, _pendingPixCount * 3)) {
        sdFileClose(); stop(); return false;
      }
      _lastFrameTime = micros();
      _framesPlayed++;
      if (frameTimeUs) *frameTimeUs = _pendingFrameTime;
      if (pixelCount)  *pixelCount  = _pendingPixCount;
      return true;
    }

    // No pending frame — read a new one from the file
    uint8_t typeByte;
    if (!sdCardRead(&typeByte, 1)) {
      sdFileClose();
      if (_playlistCount > 0) {
        _playlistIndex = 0;
        if (openNextFile()) { continue; }
      } else {
        const char *fn = _currentFile;
        if (sdFileOpen(fn, FILE_READ)) {
          _lastFrameTime = micros();
          _framesPlayed = 0;
          continue;
        }
      }
      stop();
      return false;
    }

    if (typeByte == BIN_HEADER_VIDEO || typeByte == BIN_HEADER_VIDEO_V2) {
      uint8_t extra[6];
      uint8_t extraLen = (typeByte == BIN_HEADER_VIDEO) ? 4 : 6;
      if (!sdCardRead(extra, extraLen)) {
        sdFileClose(); stop(); return false;
      }

      uint16_t pixCount = extra[0] | (extra[1] << 8);
      uint32_t frameTime;

      if (typeByte == BIN_HEADER_VIDEO) {
        frameTime = extra[2] | (extra[3] << 8);
      } else {
        frameTime = (uint32_t)extra[2] | ((uint32_t)extra[3] << 8) |
                    ((uint32_t)extra[4] << 16) | ((uint32_t)extra[5] << 24);
      }

      uint32_t scaledTime = (uint32_t)((float)frameTime / _speedMult);
      uint32_t now = micros();
      int32_t elapsed = (int32_t)(now - _lastFrameTime);

      if (elapsed < (int32_t)scaledTime) {
        // Not yet time — skip pixel data and store as pending
        sdCardSkip(pixCount * 3);
        _framePending = true;
        _pendingPixCount = pixCount;
        _pendingFrameTime = frameTime;
        return false;
      }

      // Read pixel data into dest
      if (!sdCardRead(dest, pixCount * 3)) {
        sdFileClose(); stop(); return false;
      }

      _lastFrameTime = now;
      _framesPlayed++;
      if (frameTimeUs) *frameTimeUs = frameTime;
      if (pixelCount)  *pixelCount  = pixCount;
      return true;

    } else if (typeByte == BIN_HEADER_AUDIO) {
      uint8_t audioSizeBuf[2];
      if (!sdCardRead(audioSizeBuf, 2)) { sdFileClose(); stop(); return false; }
      sdCardSkip(audioSizeBuf[0] | (audioSizeBuf[1] << 8) * 2);
      continue;
    } else {
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
void PlaybackController::stop()
{
  if (_recording) { _flushRecBuffer(); }
  _playing = false;
  _recording = false;
  _framePending = false;
  sdFileClose();
  _playlistCount = 0;
  _playlistIndex = 0;
  _recBufDirty[0] = false;
  _recBufDirty[1] = false;
}

// ============================  RECORDING  =================================

bool PlaybackController::startRecording()
{
  if (_playing) stop();
  char filename[16];
  sdNextRecordFilename(filename, sizeof(filename));
  if (!sdFileOpen(filename, O_WRITE | O_CREAT)) { return false; }
  strncpy(_currentFile, filename, sizeof(_currentFile) - 1);
  _currentFile[sizeof(_currentFile) - 1] = 0;
  _recording = true;
  _framesPlayed = 0;
  _recordStartMs = millis();
  _recBufIdx = 0;
  _recBufDirty[0] = false; _recBufDirty[1] = false;
  _recBufLen[0] = 0; _recBufLen[1] = 0;
  return true;
}

bool PlaybackController::_flushRecBuffer()
{
  uint8_t flushIdx = _recBufIdx ^ 1;
  if (!_recBufDirty[flushIdx]) return true;
  if (!sdFileWrite(s_recBuffer[flushIdx], _recBufLen[flushIdx])) {
    stopRecording(); return false;
  }
  _recBufDirty[flushIdx] = false;
  _recBufLen[flushIdx] = 0;
  return true;
}

bool PlaybackController::writeFrame(const uint8_t *rgbData, uint16_t totalPixels, uint32_t frameTimeUs)
{
  if (!_recording) return false;
  uint8_t *buf = s_recBuffer[_recBufIdx];
  buf[0] = BIN_HEADER_VIDEO_V2;
  buf[1] = totalPixels & 0xFF;
  buf[2] = (totalPixels >> 8) & 0xFF;
  buf[3] = frameTimeUs & 0xFF;
  buf[4] = (frameTimeUs >> 8) & 0xFF;
  buf[5] = (frameTimeUs >> 16) & 0xFF;
  buf[6] = (frameTimeUs >> 24) & 0xFF;
  uint16_t pixelBytes = totalPixels * 3;
  memcpy(buf + BIN_FRAME_HEADER_LEN_V2, rgbData, pixelBytes);
  _recBufLen[_recBufIdx] = BIN_FRAME_HEADER_LEN_V2 + pixelBytes;
  _recBufDirty[_recBufIdx] = true;
  if (!_flushRecBuffer()) return false;
  _recBufIdx ^= 1;
  _framesPlayed++;
  return true;
}

bool PlaybackController::stopRecording()
{
  if (!_recording) return false;
  _flushRecBuffer();
  _recBufIdx ^= 1;
  _flushRecBuffer();
  _recording = false;
  sdFileClose();
  _recBufDirty[0] = false; _recBufDirty[1] = false;
  return true;
}