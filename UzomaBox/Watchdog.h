// Watchdog.h
// Teensy 4.1 (IMXRT1062) hardware watchdog via WDOG1.
// Uses the internal low-frequency oscillator (~32 kHz).
//
// With WT=255 and prescaler 2048: timeout ≈ 255 / (32768/2048) ≈ 16.0 seconds.
//
// The heartbeat serial is disabled in normal operation.
// Uncomment DIAG_HEARTBEAT to re-enable it for diagnosis.

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <Arduino.h>

// ========================  DIAGNOSTIC FLAGS  =================================
// All diagnostics are disabled for normal operation.
// Uncomment any flag to diagnose specific subsystems.

// #define DIAG_NO_SD
// #define DIAG_NO_SHOW
// #define DIAG_NO_ETHERNET
#define DIAG_HEARTBEAT

// ========================  HEARTBEAT (optional, for diagnosis)  ==============
static uint32_t g_watchdogLastHeartbeat = 0;

static inline void watchdog_heartbeat(void)
{
  #ifdef DIAG_HEARTBEAT
  uint32_t now = millis();
  if (now - g_watchdogLastHeartbeat >= 1000) {
    g_watchdogLastHeartbeat = now;
    Serial.print(".");
    Serial.print(now / 1000);
    Serial.print("s ");
  }
  #endif
}

// ========================  WATCHDOG ENABLE  ==================================
// Hardware watchdog enabled with ~16 second timeout.
static inline void watchdog_enable(void)
{
  // Feed to unlock the configuration register
  WDOG1_WSR = 0x5555;
  WDOG1_WSR = 0xAAAA;

  // WCR: WDE=1 (enable), WDT=1 (prescaler 2048), SRS=1 (assert reset),
  //      WDA=1 (allow updates), WT=255 (timeout ~16s at 32kHz/2048)
  WDOG1_WCR = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (255 << 8);

  Serial.println("WD: watchdog enabled (~16s timeout)");
}

// ========================  WATCHDOG FEED  ====================================
static inline void watchdog_feed(void)
{
  // Refresh sequence: write 0x5555 then 0xAAAA to the service register
  WDOG1_WSR = 0x5555;
  WDOG1_WSR = 0xAAAA;
}

#endif // WATCHDOG_H