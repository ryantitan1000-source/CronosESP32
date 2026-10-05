// Interfaz mínima del lector que necesita la lógica de acceso.
// El driver real (Dy50Sensor) la implementa; las pruebas usan un lector falso.
#pragma once

#include <stdint.h>

class FingerPort {
 public:
  enum class Status {
    kOk,          // operación correcta (dedo capturado / plantilla generada / coincidencia)
    kNoFinger,    // no hay dedo apoyado
    kNotFound,    // huella no registrada
    kImageError,  // imagen mala: reintentar
    kCommError,   // sin respuesta o trama corrupta: posible desconexión
    kOtherError,  // el lector respondió con otro código de error
  };

  virtual ~FingerPort() = default;
  virtual Status capture() = 0;                                // GenImg
  virtual Status extract(uint8_t bufferId) = 0;                // Img2Tz
  virtual Status searchAll(uint16_t& pageId, uint16_t& score) = 0;  // Search buffer 1
  virtual bool ping() = 0;                                     // verificación de enlace
};
