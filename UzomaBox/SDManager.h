#ifndef SDManager_h
#define SDManager_h

#include <Arduino.h>
#include <SD.h>

// Buffered I/O constants
#define SD_BUFFER_SIZE 512

// Wrapper around SD library with buffered reads and line-oriented access.
// Uses the same 512-byte block read optimisation as the original VideoSDcard.ino.

// ---- Low-level buffered read (like the original sd_card_read) --------------
bool sdCardRead(void *ptr, unsigned int len);

// ---- Skip bytes (like the original sd_card_skip) ---------------------------
void sdCardSkip(unsigned int len);

// ---- File open / close -----------------------------------------------------
//   mode: FILE_READ or FILE_WRITE / FILE_WRITE_BEGIN
//   FILE_WRITE_BEGIN = O_WRITE | O_CREAT (writes from start)
bool sdFileOpen(const char *filename, uint8_t mode);
void sdFileClose(void);

// ---- Line reading (for CONFIG.TXT) ----------------------------------------
// Reads one line (including \n) into buf, null-terminated.
// Returns false at EOF.
bool sdFileReadLine(char *buf, unsigned int bufSize);

// ---- Formatted print (for writing CONFIG.TXT) -----------------------------
void sdFilePrintf(const char *fmt, ...);

// ---- Raw write (for .BIN recording) ---------------------------------------
bool sdFileWrite(const uint8_t *data, unsigned int len);

// ---- Raw read (for .BIN playback) -----------------------------------------
bool sdFileRead(uint8_t *data, unsigned int len);

// ---- Directory listing ----------------------------------------------------
// Returns number of .BIN files found.
int sdListBinFiles(char names[][16], int maxCount);

// ---- Generate next sequential recording filename --------------------------
void sdNextRecordFilename(char *buf, unsigned int bufSize);

// ---- File size ------------------------------------------------------------
unsigned long sdFileSize(void);

// ---- Seek (for re-playing) ------------------------------------------------
bool sdFileSeek(unsigned long pos);

// ---- Initialise SD card ---------------------------------------------------
bool sdInit(void);

#endif