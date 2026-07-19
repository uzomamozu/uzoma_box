/**
 * DMXOutput.cpp — Implementación de salida DMX512
 * =================================================
 * Hardware: Teensy 4.1 + 6N137 + MAX485
 * UART: Serial3 (UART3) — 250000 baud, 8N2
 *
 * DMX512 Frame Structure:
 *   1. BREAK   → espacio lógico bajo (88-100 µs mínimo)
 *   2. MAB     → mark después del break (8-12 µs)
 *   3. SC      → Start Code (0x00)
 *   4. Slots 1-512 → datos de canal
 *
 * Total: ~22.7 ms por frame (~44 FPS max)
 */

#include "DMXOutput.h"
#include "Pins.h"
#include <HardwareSerial.h>

// ============================================================================
// CONSTRUCTOR
// ============================================================================
DMXOutput::DMXOutput()
  : _state(DMX_STATE_IDLE)
  , _enabled(false)
  , _initialized(false)
  , _frameCount(0)
  , _errorCount(0)
{
  memset(_dmxBuffer, 0, DMX_SLOT_COUNT);
  memset(_lastError, 0, sizeof(_lastError));
}

// ============================================================================
// begin() — Inicializar UART3 y pines
// ============================================================================
bool DMXOutput::begin()
{
  if (_initialized) return true;

  // Configurar pines
  pinMode(PIN_DMX_DIR, OUTPUT);
  pinMode(PIN_DMX_TX, OUTPUT);

  // Iniciar en modo receptor (seguro)
  _setDirection(false);

  // Iniciar Serial3: 250000 baud, 8N2
  Serial3.begin(DMX_BAUD_RATE, DMX_CONFIG);
  delay(10);

  // Limpiar buffers de entrada/salida
  while (Serial3.available()) {
    Serial3.read();
  }

  _state = DMX_STATE_READY;
  _initialized = true;
  _enabled = true;

  return true;
}

// ============================================================================
// end() — Detener y liberar
// ============================================================================
void DMXOutput::end()
{
  if (!_initialized) return;
  _setDirection(false);
  Serial3.end();
  _state = DMX_STATE_IDLE;
  _initialized = false;
  _enabled = false;
}

// ============================================================================
// sendFrame() — Enviar frame DMX completo
// ============================================================================
bool DMXOutput::sendFrame(const uint8_t* data)
{
  if (!_initialized || !_enabled || data == nullptr) {
    _updateError("invalid state or NULL");
    return false;
  }

  _state = DMX_STATE_TRANSMIT;
  _setDirection(true);

  // --- BREAK: bajar temporalmente baud rate para generar espacio largo ---
  uint32_t oldBaud = Serial3.baud();
  Serial3.begin(9600, SERIAL_8N2);
  Serial3.write(0x00);           // 0x00 a 9600 baud ≈ 833 µs de BREAK
  Serial3.flush();
  Serial3.begin(DMX_BAUD_RATE, DMX_CONFIG);  // restaurar baud

  // --- MAB (Mark After Break) ---
  // delayMicroseconds es aceptable aquí (solo 8µs, no bloqueante significativo)

  // --- Start Code + 512 slots ---
  Serial3.write(DMX_START_CODE);
  for (uint16_t i = 0; i < DMX_SLOT_COUNT; i++) {
    Serial3.write(data[i]);
  }
  Serial3.flush();  // esperar que termine la transmisión

  // Volver a modo receptor
  _setDirection(false);

  _frameCount++;
  _state = DMX_STATE_READY;

  return true;
}

// ============================================================================
// setDMXData() — Copiar datos DMX al buffer interno
// ============================================================================
void DMXOutput::setDMXData(const uint8_t* dmxData, uint16_t length)
{
  if (dmxData == nullptr) return;
  uint16_t copyLen = (length < DMX_SLOT_COUNT) ? length : DMX_SLOT_COUNT;
  memcpy(_dmxBuffer, dmxData, copyLen);
  if (copyLen < DMX_SLOT_COUNT) {
    memset(_dmxBuffer + copyLen, 0, DMX_SLOT_COUNT - copyLen);
  }
}

// ============================================================================
// MÉTODOS PRIVADOS
// ============================================================================

void DMXOutput::_setDirection(bool tx)
{
  // MAX485: DE y RE conectados juntos
  // HIGH = Transmisor habilitado (TX), LOW = Receptor habilitado (RX)
  digitalWrite(PIN_DMX_DIR, tx ? HIGH : LOW);
}

void DMXOutput::_updateError(const char* error)
{
  if (error == nullptr) return;
  strncpy(_lastError, error, sizeof(_lastError) - 1);
  _lastError[sizeof(_lastError) - 1] = '\0';
  _errorCount++;
  _state = DMX_STATE_ERROR;
  // Auto-recuperación inmediata
  if (_initialized) {
    _state = DMX_STATE_READY;
  }
}