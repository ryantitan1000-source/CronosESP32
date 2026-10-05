#include "relay_pulse.h"

RelayPulse::RelayPulse(uint32_t pulseMs, uint32_t cooldownMs, uint32_t minMs, uint32_t maxMs)
    : pulseMs_(pulseMs), cooldownMs_(cooldownMs), minMs_(minMs), maxMs_(maxMs) {
  if (minMs_ > maxMs_) minMs_ = maxMs_;
  setPulseMs(pulseMs);
}

uint32_t RelayPulse::setPulseMs(uint32_t ms) {
  if (ms < minMs_) ms = minMs_;
  if (ms > maxMs_) ms = maxMs_;
  pulseMs_ = ms;
  return pulseMs_;
}

RelayPulse::Request RelayPulse::request(uint32_t nowMs) {
  update(nowMs);
  if (active_) return Request::kBusy;
  if (everPulsed_ && static_cast<uint32_t>(nowMs - endMs_) < cooldownMs_) {
    return Request::kCooldown;
  }
  active_ = true;
  everPulsed_ = true;
  startMs_ = nowMs;
  ++pulses_;
  return Request::kStarted;
}

bool RelayPulse::update(uint32_t nowMs) {
  // Un 'now' anterior al inicio (leído antes de request()) no debe apagar el pulso:
  // sin esta guarda la resta sin signo da la vuelta y lo cortaría al instante.
  if (active_ && static_cast<int32_t>(nowMs - startMs_) < 0) return active_;
  if (active_ && static_cast<uint32_t>(nowMs - startMs_) >= pulseMs_) {
    active_ = false;
    endMs_ = startMs_ + pulseMs_;
  }
  return active_;
}

void RelayPulse::forceOff(uint32_t nowMs) {
  if (active_) {
    active_ = false;
    endMs_ = nowMs;
  }
}

uint32_t RelayPulse::remainingMs(uint32_t nowMs) const {
  if (!active_) return 0;
  const uint32_t elapsed = nowMs - startMs_;
  return elapsed >= pulseMs_ ? 0 : pulseMs_ - elapsed;
}
