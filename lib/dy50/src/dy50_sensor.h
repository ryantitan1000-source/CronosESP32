// Driver del lector DY50 sobre una UART del ESP32 (protocolo EF01, ver fp_proto.h).
#pragma once

#include <Arduino.h>

#include "finger_port.h"
#include "fp_proto.h"

class Dy50Sensor : public FingerPort {
 public:
  // Resultado a nivel de enlace (independiente del código de confirmación).
  enum class Link { kOk, kTimeout, kBadChecksum, kBadFrame, kNotAck };

  struct Stats {
    uint32_t txFrames = 0;
    uint32_t rxFrames = 0;
    uint32_t timeouts = 0;
    uint32_t badChecksum = 0;
    uint32_t badFrames = 0;
    uint32_t garbageBytes = 0;
  };

  using IdleHook = void (*)();

  // Tiempos de espera de respuesta (ms)
  static constexpr uint32_t kTimeoutDefault = 1000;
  static constexpr uint32_t kTimeoutSearch = 3000;
  static constexpr uint32_t kTimeoutFlash = 3000;  // Store / Delete / Empty
  static constexpr uint32_t kTimeoutPing = 400;

  Dy50Sensor(HardwareSerial& uart, int8_t rxPin, int8_t txPin,
             uint32_t address = fp::kDefaultAddress, uint32_t password = fp::kDefaultPassword);

  void begin(uint32_t baud);
  void setBaud(uint32_t baud);
  uint32_t baud() const { return baud_; }

  // Se llama mientras se espera una respuesta (p. ej. para alimentar el watchdog).
  void setIdleHook(IdleHook hook) { idleHook_ = hook; }
  // Si no es nullptr, imprime cada trama enviada/recibida en hexadecimal.
  void setTrace(Print* out) { trace_ = out; }
  bool tracing() const { return trace_ != nullptr; }

  // Cantidad de posiciones de la base (se lee con ReadSysPara).
  void setLibrarySize(uint16_t n) { librarySize_ = n; }
  uint16_t librarySize() const { return librarySize_; }

  // Envía un comando y espera la respuesta. Si devuelve kOk, lastAck() es válido
  // hasta el próximo comando.
  Link run(const fp::Command& cmd, uint32_t timeoutMs = kTimeoutDefault);
  const fp::Ack& lastAck() const { return lastAck_; }
  uint8_t lastCode() const { return lastAck_.code; }

  // Busca a qué velocidad responde el lector. Devuelve los baudios o 0.
  uint32_t findBaud(Print* log);

  Link readSysParams(fp::SysParams& out);
  // Lista de posiciones ocupadas. 'n' = cantidad escrita en ids.
  Link usedIds(uint16_t* ids, size_t maxIds, size_t& n);

  // Envía bytes crudos y devuelve lo recibido durante 'waitMs' (diagnóstico).
  size_t rawExchange(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxCap, uint32_t waitMs);

  // FingerPort
  Status capture() override;
  Status extract(uint8_t bufferId) override;
  Status searchAll(uint16_t& pageId, uint16_t& score) override;
  bool ping() override;  // true si llega cualquier respuesta válida

  const Stats& stats() const { return stats_; }
  static const char* linkText(Link l);

 private:
  void dump(const char* tag, const uint8_t* data, size_t n);
  void idle();

  HardwareSerial& uart_;
  int8_t rxPin_;
  int8_t txPin_;
  uint32_t address_;
  uint32_t password_;
  uint32_t baud_ = 0;
  uint16_t librarySize_ = 0;
  IdleHook idleHook_ = nullptr;
  Print* trace_ = nullptr;
  fp::FrameParser parser_;
  fp::Frame lastFrame_{};
  fp::Ack lastAck_{};
  Stats stats_;
};
