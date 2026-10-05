#include "access_controller.h"

using Status = FingerPort::Status;

namespace {
AccessEvent ev(AccessEventType t) {
  AccessEvent e;
  e.type = t;
  return e;
}
}  // namespace

AccessController::AccessController(FingerPort& port, const AccessConfig& cfg)
    : port_(port), cfg_(cfg) {}

AccessEvent AccessController::step(uint32_t nowMs) {
  switch (state_) {
    case State::kOffline: {
      if (static_cast<uint32_t>(nowMs - lastReconnect_) < cfg_.reconnectMs) return ev(AccessEventType::kNone);
      lastReconnect_ = nowMs;
      if (port_.ping()) {
        commErrors_ = 0;
        releaseCount_ = 0;
        state_ = State::kWaitRelease;  // no evaluar un dedo que quedó apoyado
        return ev(AccessEventType::kReaderOnline);
      }
      return ev(AccessEventType::kNone);
    }

    case State::kLockout: {
      if (static_cast<uint32_t>(nowMs - lockoutStart_) < cfg_.lockoutMs) return ev(AccessEventType::kNone);
      fails_ = 0;
      releaseCount_ = 0;
      state_ = State::kWaitRelease;
      return ev(AccessEventType::kLockoutEnded);
    }

    case State::kWaitRelease: {
      const Status s = port_.capture();
      if (s == Status::kCommError) return onCommError(nowMs);
      commErrors_ = 0;
      if (s == Status::kNoFinger) {
        const uint8_t needed = cfg_.releasePolls ? cfg_.releasePolls : 1;
        if (++releaseCount_ >= needed) {
          releaseCount_ = 0;
          state_ = State::kIdle;
          return ev(AccessEventType::kFingerReleased);
        }
      } else {
        releaseCount_ = 0;  // sigue habiendo dedo (o imagen dudosa): seguir esperando
      }
      return ev(AccessEventType::kNone);
    }

    case State::kIdle:
      return evaluate(nowMs);
  }
  return ev(AccessEventType::kNone);
}

AccessEvent AccessController::evaluate(uint32_t nowMs) {
  Status s = port_.capture();
  if (s == Status::kCommError) return onCommError(nowMs);
  commErrors_ = 0;
  if (s == Status::kNoFinger) return ev(AccessEventType::kNone);
  if (s != Status::kOk) return ev(AccessEventType::kImageError);

  s = port_.extract(1);
  if (s == Status::kCommError) return onCommError(nowMs);
  if (s != Status::kOk) return ev(AccessEventType::kImageError);

  uint16_t id = 0;
  uint16_t score = 0;
  s = port_.searchAll(id, score);
  if (s == Status::kCommError) return onCommError(nowMs);

  if (s == Status::kOk && score >= cfg_.minScore) {
    fails_ = 0;
    releaseCount_ = 0;
    state_ = State::kWaitRelease;
    AccessEvent e = ev(AccessEventType::kGranted);
    e.id = id;
    e.score = score;
    return e;
  }

  if (s == Status::kOk || s == Status::kNotFound) {
    AccessEvent e = ev(AccessEventType::kDenied);
    if (s == Status::kOk) {  // existe pero con puntaje bajo el mínimo
      e.id = id;
      e.score = score;
      e.lowScore = true;
    }
    if (fails_ < 255) ++fails_;
    releaseCount_ = 0;
    state_ = State::kWaitRelease;
    if (cfg_.maxFails && fails_ >= cfg_.maxFails) {
      state_ = State::kLockout;
      lockoutStart_ = nowMs;
      e.lockoutStarted = true;
    }
    return e;
  }

  return ev(AccessEventType::kImageError);  // otro código del lector: reintentar
}

AccessEvent AccessController::onCommError(uint32_t nowMs) {
  const uint8_t limit = cfg_.commErrorsOffline ? cfg_.commErrorsOffline : 1;
  if (++commErrors_ >= limit) {
    commErrors_ = 0;
    state_ = State::kOffline;
    lastReconnect_ = nowMs;
    return ev(AccessEventType::kReaderOffline);
  }
  return ev(AccessEventType::kCommError);
}

void AccessController::requireRelease() {
  if (state_ == State::kIdle) {
    state_ = State::kWaitRelease;
    releaseCount_ = 0;
  }
}

void AccessController::markOffline(uint32_t nowMs) {
  state_ = State::kOffline;
  commErrors_ = 0;
  lastReconnect_ = nowMs;
}

void AccessController::markOnline() {
  if (state_ == State::kOffline) {
    state_ = State::kWaitRelease;
    commErrors_ = 0;
    releaseCount_ = 0;
  }
}

uint32_t AccessController::lockoutRemainingMs(uint32_t nowMs) const {
  if (state_ != State::kLockout) return 0;
  const uint32_t elapsed = nowMs - lockoutStart_;
  return elapsed >= cfg_.lockoutMs ? 0 : cfg_.lockoutMs - elapsed;
}

const char* accessStateName(AccessController::State s) {
  switch (s) {
    case AccessController::State::kIdle: return "ESPERANDO DEDO";
    case AccessController::State::kWaitRelease: return "ESPERANDO RETIRO DEL DEDO";
    case AccessController::State::kLockout: return "BLOQUEADO (demasiados intentos)";
    case AccessController::State::kOffline: return "LECTOR DESCONECTADO";
  }
  return "?";
}
