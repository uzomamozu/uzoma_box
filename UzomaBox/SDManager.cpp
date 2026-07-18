#include "SDManager.h"
#include <stdarg.h>

static File g_file;
static bool  g_opened = false;

// Buffered read state (same technique as original VideoSDcard.ino)
static uint8_t  g_buf[SD_BUFFER_SIZE];
static unsigned int g_bufPos = 0;
static unsigned int g_bufLen = 0;

// ---- Buffered read ---------------------------------------------------------
bool sdCardRead(void *ptr, unsigned int len)
{
  uint8_t *dest = (uint8_t *)ptr;

  while (len > 0) {
    if (g_bufLen == 0) {
      int n = g_file.read(g_buf, SD_BUFFER_SIZE);
      if (n <= 0) return false;
      g_bufLen = (unsigned int)n;
      g_bufPos = 0;
    }
    unsigned int n = g_bufLen;
    if (n > len) n = len;
    memcpy(dest, g_buf + g_bufPos, n);
    dest   += n;
    g_bufPos += n;
    g_bufLen -= n;
    len    -= n;
  }
  return true;
}

// ---- Skip bytes (discard) — consumes from buffer first, then seeks ---------
void sdCardSkip(unsigned int len)
{
  if (!g_opened) return;
  // Consume any bytes already buffered by read-ahead to keep position in sync
  unsigned int inBuf = g_bufLen;
  if (len <= inBuf) {
    g_bufPos += len;
    g_bufLen -= len;
  } else {
    len -= inBuf;
    g_bufPos = g_bufLen = 0;                // invalidate buffer
    sdFileSeekRelative((long)len);           // seek only the remainder
  }
}

// ---- File open -------------------------------------------------------------
bool sdFileOpen(const char *filename, int mode)
{
  if (g_opened) sdFileClose();
  g_file = SD.open(filename, mode);
  g_opened = (g_file != 0);
  g_bufPos = g_bufLen = 0;
  return g_opened;
}

// ---- File close ------------------------------------------------------------
void sdFileClose(void)
{
  if (g_opened) {
    g_file.close();
    g_opened = false;
  }
  g_bufPos = g_bufLen = 0;
}

// ---- Line read (for CONFIG.TXT) -------------------------------------------
bool sdFileReadLine(char *buf, unsigned int bufSize)
{
  if (!g_opened) return false;

  int c;
  unsigned int pos = 0;
  while (pos < bufSize - 1) {
    c = g_file.read();
    if (c < 0) break;               // EOF
    buf[pos++] = (char)c;
    if (c == '\n') break;           // end-of-line
  }
  buf[pos] = 0;
  return (pos > 0);
}

// ---- Formatted print (for CONFIG.TXT) --------------------------------------
void sdFilePrintf(const char *fmt, ...)
{
  if (!g_opened) return;

  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  g_file.print(tmp);
}

// ---- Raw write (for .BIN recording) ----------------------------------------
bool sdFileWrite(const uint8_t *data, unsigned int len)
{
  if (!g_opened) return false;
  size_t written = g_file.write(data, len);
  return (written == len);
}

// ---- Raw read (for .BIN playback, direct, not buffered) --------------------
bool sdFileRead(uint8_t *data, unsigned int len)
{
  if (!g_opened) return false;
  size_t n = g_file.read(data, len);
  return (n == len);
}

// ---- Directory listing: find all .BIN files --------------------------------
int sdListBinFiles(char names[][16], int maxCount)
{
  File root = SD.open("/");
  if (!root) return 0;

  int count = 0;
  File entry;
  while ((entry = root.openNextFile()) && count < maxCount) {
    if (!entry.isDirectory()) {
      const char *fn = entry.name();
      // Check for .BIN extension (case-insensitive)
      size_t len = strlen(fn);
      if (len >= 4 && fn[len-4] == '.' &&
          toupper(fn[len-3]) == 'B' &&
          toupper(fn[len-2]) == 'I' &&
          toupper(fn[len-1]) == 'N') {
        strncpy(names[count], fn, 15);
        names[count][15] = 0;
        count++;
      }
    }
    entry.close();
  }
  root.close();
  return count;
}

// ---- Next recording filename -----------------------------------------------
void sdNextRecordFilename(char *buf, unsigned int bufSize)
{
  // Scan existing REC_XXX.BIN files and pick the next number
  int highest = 0;
  File root = SD.open("/");
  if (root) {
    File entry;
    while ((entry = root.openNextFile())) {
      const char *fn = entry.name();
      int num;
      if (sscanf(fn, "REC_%d.BIN", &num) == 1) {
        if (num > highest) highest = num;
      }
      entry.close();
    }
    root.close();
  }
  snprintf(buf, bufSize, "REC_%03d.BIN", highest + 1);
}

// ---- File size -------------------------------------------------------------
unsigned long sdFileSize(void)
{
  return g_opened ? g_file.size() : 0;
}

// ---- File position ----------------------------------------------------------
unsigned long sdFilePosition(void)
{
  if (!g_opened) return 0;
  return g_file.position();
}

// ---- Seek (for re-playing) ------------------------------------------------
bool sdFileSeek(unsigned long pos)
{
  if (!g_opened) return false;
  g_bufPos = g_bufLen = 0;            // invalidate buffer
  return g_file.seek(pos);
}

// ---- Seek relative to current position (for fast skip) --------------------
bool sdFileSeekRelative(long delta)
{
  if (!g_opened) return false;
  unsigned long currentPos = g_file.position();
  // If delta would go negative, clamp to 0
  if ((long)currentPos + delta < 0) {
    currentPos = 0;
  } else {
    currentPos = (unsigned long)((long)currentPos + delta);
  }
  g_bufPos = g_bufLen = 0;            // invalidate buffer
  return g_file.seek(currentPos);
}

// ---- Delete a file ---------------------------------------------------------
bool sdFileDelete(const char *filename)
{
  sdFileClose();   // make sure no file is open
  return SD.remove(filename);
}

// ---- Initialise SD card (Teensy 4.1 built-in microSD slot) -----------------
bool sdInit(void)
{
  return SD.begin(BUILTIN_SDCARD);
}