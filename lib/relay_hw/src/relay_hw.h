// Capa de hardware del relé: pin GPIO + temporizador de apagado (esp_timer)
// independiente del loop(). Así el pulso se corta a tiempo aunque el programa
// principal esté ocupado esperando al lector.
//
// Con salida deshabilitada (simulación) el pin queda fijo en reposo y solo se
// registra el pulso que se habría dado.
#pragma once

#include <stdint.h>

namespace relayhw {

// Llamar lo antes posible en setup(): deja el pin en reposo.
void init(int pin, bool outputEnabled, int activeLevel);

bool outputEnabled();
int pin();
int activeLevel();
int restLevel();

// Activa durante 'ms' y se apaga sola. Ignorado si ya hay un pulso activo.
void activate(uint32_t ms);
void off();
bool isActive();
uint32_t activations();
// Nivel lógico que el ESP32 está ordenando en el pin.
int commandedLevel();
// Nivel leído en el pad (si difiere de lo ordenado: corto o carga excesiva).
int padLevel();

// Diagnóstico con multímetro (solo en simulación): fija el pin y lo devuelve
// a reposo solo tras 'autoRestMs'. Devuelve false si la salida real está habilitada.
bool diagSet(int level, uint32_t autoRestMs);
bool diagActive();

}  // namespace relayhw
