// Lista de IDs de huella bloqueados (lógica pura, sin hardware).
// Una huella bloqueada sigue guardada en el lector pero NO abre el molinete; se
// puede volver a permitir sin registrarla de nuevo. Por defecto no hay ninguna
// bloqueada, así que las huellas ya registradas siguen funcionando igual.
#pragma once

#include <stddef.h>
#include <stdint.h>

class BlockList {
 public:
  static constexpr uint16_t kMaxIds = 1024;
  static constexpr size_t kBytes = kMaxIds / 8;

  bool isBlocked(uint16_t id) const;
  // Devuelve false (sin cambiar nada) si el ID está fuera de rango.
  bool set(uint16_t id, bool blocked);
  void clearAll();
  uint16_t count() const;
  // Escribe hasta 'max' IDs bloqueados en 'out' (en orden ascendente); devuelve cuántos.
  size_t list(uint16_t* out, size_t max) const;

  // Para guardar/restaurar (p. ej. en la memoria no volátil).
  const uint8_t* data() const { return bits_; }
  // Devuelve false (y deja la lista vacía) si el largo no coincide.
  bool load(const uint8_t* data, size_t len);

 private:
  uint8_t bits_[kBytes] = {0};
};
