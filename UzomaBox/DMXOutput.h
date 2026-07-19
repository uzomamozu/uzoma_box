/**
 * DMXOutput.h — Salida DMX512 vía Art-Net forwarding
 * ====================================================
 * Hardware: Teensy 4.1 + 6N137 (optoaislador) + MAX485 (transceptor RS-485)
 * UART:     Serial3 (UART3) — Pin 14 (TX), Pin 15 (DE/RE GPIO)
 * Protocol: DMX512 (250 kbaud, 8N2) + BREAK signal generado por UART
 *
 * Compatible con: UzomaBox v2.0 (8×1024 LEDs)
 */

#ifndef DMXOutput_h
#define DMXOutput_h

#include <Arduino.h>
#include "Pins.h"

// ============================================================================
// CONFIGURACIÓN HARDWARE
// ============================================================================

// Configuración UART DMX512
#define DMX_BAUD_RATE    250000   // 250 kbaud — estándar DMX512
#define DMX_CONFIG       SERIAL_8N2  // 8 bits datos, sin paridad, 2 stop bits

// Protocolo DMX512
#define DMX_SLOT_COUNT   512      // 512 slots DMX (estándar)
#define DMX_START_CODE   0x00     // Break + Mark After Break (MAB)

// Timing DMX
#define DMX_BREAK_US     92       // Mínimo 88 µs (típico 92-100 µs)
#define DMX_MAB_US       8        // Mark After Break: 8-12 µs

// Intervalo mínimo entre frames DMX (~24ms a 44 FPS max)
#define DMX_FRAME_INTERVAL_US  24000

// ============================================================================
// ENUM: Estado del módulo DMX
// ============================================================================
enum DMXState {
  DMX_STATE_IDLE,      // Inactivo, esperando comandos
  DMX_STATE_READY,     // Inicializado, listo para transmitir
  DMX_STATE_TRANSMIT,  // Transmitiendo frame actual
  DMX_STATE_ERROR      // Error de comunicación UART
};

// ============================================================================
// CLASE: DMXOutput
// ============================================================================
class DMXOutput {
public:
  DMXOutput();

  // Inicializar UART3 y pines GPIO
  bool begin();

  // Detener UART y liberar recursos
  void end();

  // Enviar un frame DMX completo (BREAK → MAB → StartCode → 512 bytes)
  bool sendFrame(const uint8_t* data);

  // Copiar datos DMX al buffer interno
  void setDMXData(const uint8_t* dmxData, uint16_t length);

  // Establecer el nivel de un canal específico (1-512)
  inline void setChannel(uint16_t channel, uint8_t value) {
    if (channel >= 1 && channel <= DMX_SLOT_COUNT) {
      _dmxBuffer[channel - 1] = value;
    }
  }

  // Obtener el valor de un canal específico (1-512)
  inline uint8_t getChannel(uint16_t channel) const {
    if (channel >= 1 && channel <= DMX_SLOT_COUNT) {
      return _dmxBuffer[channel - 1];
    }
    return 0;
  }

  // Estado y diagnóstico
  inline DMXState getState() const { return _state; }
  inline bool isReady() const { return _state == DMX_STATE_READY; }
  inline bool isEnabled() const { return _enabled; }
  inline void setEnabled(bool enable) { _enabled = enable; }
  inline uint32_t getFrameCount() const { return _frameCount; }
  inline uint32_t getErrorCount() const { return _errorCount; }
  inline const char* getLastError() const { return _lastError; }

private:
  static constexpr size_t BUFFER_SIZE = DMX_SLOT_COUNT;
  uint8_t _dmxBuffer[BUFFER_SIZE];

  DMXState _state;
  bool     _enabled;
  bool     _initialized;

  uint32_t _frameCount;
  uint32_t _errorCount;
  char     _lastError[32];

  // Control de dirección MAX485
  void _setDirection(bool tx);

  // Registrar error
  void _updateError(const char* error);
};

#endif // DMXOutput_h