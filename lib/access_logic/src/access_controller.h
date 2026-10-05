// Máquina de estados del control de acceso (sin hardware).
//
//   ESPERA ──dedo──> captura → plantilla → búsqueda
//      │                 ├─ registrada y puntaje >= mínimo → CONCEDIDO → ESPERA_RETIRO
//      │                 └─ no registrada / puntaje bajo → DENEGADO → ESPERA_RETIRO
//      │                                       (N fallos seguidos → BLOQUEO temporal)
//   ESPERA_RETIRO ──K lecturas seguidas sin dedo──> ESPERA
//   cualquier estado ──errores de comunicación seguidos──> DESCONECTADO
//   DESCONECTADO ──ping OK──> ESPERA_RETIRO
//
// ESPERA_RETIRO evita pulsos repetidos mientras el dedo sigue apoyado.
#pragma once

#include <stdint.h>

#include "finger_port.h"

struct AccessConfig {
  uint16_t minScore = 0;          // 0 = acepta el criterio del propio lector
  uint8_t releasePolls = 3;       // lecturas seguidas sin dedo para considerarlo retirado
  uint8_t maxFails = 5;           // denegaciones seguidas antes del bloqueo
  uint32_t lockoutMs = 30000;     // duración del bloqueo
  uint8_t commErrorsOffline = 3;  // errores de comunicación seguidos => desconectado
  uint32_t reconnectMs = 3000;    // intervalo entre intentos de reconexión
};

enum class AccessEventType {
  kNone,
  kGranted,         // huella autorizada: abrir
  kDenied,          // huella no registrada o puntaje insuficiente
  kImageError,      // imagen mala, se reintentará
  kFingerReleased,  // el dedo se retiró; listo para otra lectura
  kCommError,       // error de comunicación aislado
  kReaderOffline,   // demasiados errores: lector desconectado
  kReaderOnline,    // el lector volvió a responder
  kLockoutEnded,
};

struct AccessEvent {
  AccessEventType type = AccessEventType::kNone;
  uint16_t id = 0;
  uint16_t score = 0;
  bool lowScore = false;      // kDenied por puntaje bajo (la huella existe)
  bool lockoutStarted = false;  // kDenied que además inició un bloqueo
};

class AccessController {
 public:
  enum class State { kIdle, kWaitRelease, kLockout, kOffline };

  AccessController(FingerPort& port, const AccessConfig& cfg);

  // Un paso: hace como máximo una secuencia de lectura. Llamar periódicamente.
  AccessEvent step(uint32_t nowMs);

  // Obliga a esperar que se retire el dedo antes de evaluar otro.
  void requireRelease();
  // Fuerza el estado desconectado (p. ej. si otra parte detectó la falla).
  void markOffline(uint32_t nowMs);
  // El lector volvió a responder por otra vía: salir de DESCONECTADO
  // (pasa a esperar retiro del dedo, nunca evalúa directamente).
  void markOnline();

  State state() const { return state_; }
  uint8_t failCount() const { return fails_; }
  uint32_t lockoutRemainingMs(uint32_t nowMs) const;
  AccessConfig& config() { return cfg_; }
  const AccessConfig& config() const { return cfg_; }

 private:
  AccessEvent onCommError(uint32_t nowMs);
  AccessEvent evaluate(uint32_t nowMs);

  FingerPort& port_;
  AccessConfig cfg_;
  State state_ = State::kIdle;
  uint8_t fails_ = 0;
  uint8_t commErrors_ = 0;
  uint8_t releaseCount_ = 0;
  uint32_t lockoutStart_ = 0;
  uint32_t lastReconnect_ = 0;
};

const char* accessStateName(AccessController::State s);
