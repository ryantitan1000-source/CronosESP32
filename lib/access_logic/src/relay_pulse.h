// Lógica pura del pulso de apertura (sin hardware). Decide cuándo el relé
// debe estar activo; quien la usa aplica ese estado al pin.
// Todas las comparaciones de tiempo usan resta sin signo: funcionan aunque
// millis() dé la vuelta (cada ~49,7 días).
#pragma once

#include <stdint.h>

class RelayPulse {
 public:
  enum class Request { kStarted, kBusy, kCooldown };

  RelayPulse(uint32_t pulseMs, uint32_t cooldownMs, uint32_t minMs, uint32_t maxMs);

  // Ajusta la duración (se limita a [min, max]). Devuelve el valor aplicado.
  uint32_t setPulseMs(uint32_t ms);
  uint32_t pulseMs() const { return pulseMs_; }
  uint32_t cooldownMs() const { return cooldownMs_; }

  // Pide un pulso. Solo arranca si no hay uno en curso y pasó el enfriamiento.
  Request request(uint32_t nowMs);

  // Llamar seguido. Apaga al cumplirse la duración. Devuelve si debe estar activo.
  bool update(uint32_t nowMs);

  // Apagado inmediato (error, lector desconectado, reinicio).
  void forceOff(uint32_t nowMs);

  bool active() const { return active_; }
  uint32_t remainingMs(uint32_t nowMs) const;
  uint32_t pulsesStarted() const { return pulses_; }

 private:
  uint32_t pulseMs_;
  uint32_t cooldownMs_;
  uint32_t minMs_;
  uint32_t maxMs_;
  bool active_ = false;
  bool everPulsed_ = false;
  uint32_t startMs_ = 0;
  uint32_t endMs_ = 0;
  uint32_t pulses_ = 0;
};
