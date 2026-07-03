#include "PlaybackController.h"

DMAMEM uint8_t PlaybackController::s_recBuffer[REC_BUFFER_COUNT][MAX_FRAME_SIZE];

PlaybackController::PlaybackController()
  : _playing(false), _recording(false), _playlistCount(0), _playlistIndex(0)
  , _framesPlayed(0), _lastFrameTime(0), _speedMult(1.0f), _recordStartMs(0)
  , _framePending(false), _pendingPixCount(0), _pendingFrameTime(0), _recBufIdx(0)
{
  _currentFile[0] = 0;
  _recBufDirty[0] = false; _recBufDirty[1] = false;
  _recBufLen[0] = 0; _recBufLen[1] = 0;
}

PlaybackController::~PlaybackController() {}

// ---- Playback ----

bool PlaybackController::playFile(const char *filename) {
  stop();
  if (!sdFileOpen(filename, FILE_READ)) return false;
  strncpy(_currentFile, filename, sizeof(_currentFile) - 1);
  _currentFile[sizeof(_currentFile) - 1] = 0;
  _playing = true; _framesPlayed = 0; _framePending = false;
  _lastFrameTime = micros(); _playlistCount = 0;
  return true;
}

int PlaybackController::playSequence() {
  stop();
  _playlistCount = sdListBinFiles(_playlist, 64);
  if (_playlistCount == 0) return 0;
  _playlistIndex = 0;
  return openNextFile() ? _playlistCount : 0;
}

bool PlaybackController::openNextFile() {
  while (_playlistIndex < _playlistCount) {
    const char *fn = _playlist[_playlistIndex++];
    if (sdFileOpen(fn, FILE_READ)) {
      strncpy(_currentFile, fn, sizeof(_currentFile) - 1);
      _currentFile[sizeof(_currentFile) - 1] = 0;
      _playing = true; _framesPlayed = 0; _framePending = false;
      _lastFrameTime = micros();
      return true;
    }
  }
  stop(); return false;
}

bool PlaybackController::playNextFrame(uint8_t *dest, uint32_t *frameTimeUs, uint16_t *pixelCount) {
  if (!_playing) return false;
  uint32_t now; int32_t elapsed;

  // If we have a pending frame, check timing without file access
  if (_framePending) {
    now = micros();
    elapsed = (int32_t)(now - _lastFrameTime);
    uint32_t scaledTime = (uint32_t)((float)_pendingFrameTime / _speedMult);
    if (elapsed < (int32_t)scaledTime) return false;
    // Ready — copy data from pending buffer to dest
    memcpy(dest, _pendingBuf + BIN_FRAME_HEADER_LEN_V2, _pendingPixCount * 3);
    _framePending = false;
    _lastFrameTime = now;
    _framesPlayed++;
    if (frameTimeUs) *frameTimeUs = _pendingFrameTime;
    if (pixelCount) *pixelCount = _pendingPixCount;
    return true;
  }

  // Read next frame from file
  uint8_t typeByte;
  while (true) {
    if (!sdCardRead(&typeByte, 1)) {
      sdFileClose();
      if (_playlistCount > 0) { _playlistIndex = 0; if (openNextFile()) continue; }
      else { const char *fn = _currentFile; if (sdFileOpen(fn, FILE_READ)) {
        _lastFrameTime = micros(); _framesPlayed = 0; continue; } }
      stop(); return false;
    }

    if (typeByte == BIN_HEADER_VIDEO || typeByte == BIN_HEADER_VIDEO_V2) {
      uint8_t extra[6];
      uint8_t extraLen = (typeByte == BIN_HEADER_VIDEO) ? 4 : 6;
      if (!sdCardRead(extra, extraLen)) { sdFileClose(); stop(); return false; }

      uint16_t pixCount = extra[0] | (extra[1] << 8);
      uint32_t frameTime;
      if (typeByte == BIN_HEADER_VIDEO)
        frameTime = extra[2] | (extra[3] << 8);
      else
        frameTime = (uint32_t)extra[2] | ((uint32_t)extra[3] << 8) |
                    ((uint32_t)extra[4] << 16) | ((uint32_t)extra[5] << 24);

      // Read the entire frame (header + pixel data) into pending buffer
      _pendingBuf[0] = typeByte;
      memcpy(_pendingBuf + 1, extra, extraLen);
      if (!sdCardRead(_pendingBuf + 1 + extraLen, pixCount * 3)) {
        sdFileClose(); stop(); return false;
      }
      _pendingPixCount = pixCount;
      _pendingFrameTime = frameTime;

      // Check timing now
      now = micros();
      elapsed = (int32_t)(now - _lastFrameTime);
      uint32_t scaledTime = (uint32_t)((float)frameTime / _speedMult);
      if (elapsed < (int32_t)scaledTime) {
        _framePending = true;
        return false;  // will return the pending data next call
      }

      // Copy pixel portion to dest
      memcpy(dest, _pendingBuf + BIN_FRAME_HEADER_LEN_V2, pixCount * 3);
      _framePending = false;
      _lastFrameTime = now;
      _framesPlayed++;
      if (frameTimeUs) *frameTimeUs = frameTime;
      if (pixelCount) *pixelCount = pixCount;
      return true;

    } else if (typeByte == BIN_HEADER_AUDIO) {
      uint8_t sz[2];
      if (!sdCardRead(sz, 2)) { sdFileClose(); stop(); return false; }
      sdCardSkip((sz[0] | (sz[1] << 8)) * 2);
      continue;
    } else {
      continue;
    }
  }
}

void PlaybackController::stop() {
  if (_recording) _flushRecBuffer();
  _playing = false; _recording = false; _framePending = false;
  sdFileClose();
  _playlistCount = 0; _playlistIndex = 0;
  _recBufDirty[0] = false; _recBufDirty[1] = false;
}

// ---- Recording ----

bool PlaybackController::startRecording() {
  if (_playing) stop();
  char fn[16]; sdNextRecordFilename(fn, sizeof(fn));
  if (!sdFileOpen(fn, O_WRITE | O_CREAT)) return false;
  strncpy(_currentFile, fn, sizeof(_currentFile) - 1);
  _currentFile[sizeof(_currentFile) - 1] = 0;
  _recording = true; _framesPlayed = 0; _recordStartMs = millis();
  _recBufIdx = 0; _recBufDirty[0] = false; _recBufDirty[1] = false;
  _recBufLen[0] = 0; _recBufLen[1] = 0;
  return true;
}

bool PlaybackController::_flushRecBuffer() {
  uint8_t fi = _recBufIdx ^ 1;
  if (!_recBufDirty[fi]) return true;
  if (!sdFileWrite(s_recBuffer[fi], _recBufLen[fi])) { stopRecording(); return false; }
  _recBufDirty[fi] = false; _recBufLen[fi] = 0;
  return true;
}

bool PlaybackController::writeFrame(const uint8_t *d, uint16_t tp, uint32_t ft) {
  if (!_recording) return false;
  uint8_t *b = s_recBuffer[_recBufIdx];
  b[0] = BIN_HEADER_VIDEO_V2;
  b[1] = tp & 0xFF; b[2] = (tp >> 8) & 0xFF;
  b[3] = ft & 0xFF; b[4] = (ft >> 8) & 0xFF;
  b[5] = (ft >> 16) & 0xFF; b[6] = (ft >> 24) & 0xFF;
  uint16_t pb = tp * 3;
  memcpy(b + BIN_FRAME_HEADER_LEN_V2, d, pb);
  _recBufLen[_recBufIdx] = BIN_FRAME_HEADER_LEN_V2 + pb;
  _recBufDirty[_recBufIdx] = true;
  if (!_flushRecBuffer()) return false;
  _recBufIdx ^= 1; _framesPlayed++;
  return true;
}

bool PlaybackController::stopRecording() {
  if (!_recording) return false;
  _flushRecBuffer(); _recBufIdx ^= 1; _flushRecBuffer();
  _recording = false; sdFileClose();
  _recBufDirty[0] = false; _recBufDirty[1] = false;
  return true;
}