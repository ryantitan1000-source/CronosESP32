#include "relay_hw.h"

#include <driver/gpio.h>
#include <esp_timer.h>

namespace relayhw {
namespace {

gpio_num_t g_pin = GPIO_NUM_NC;
bool g_enabled = false;
int g_active = 1;
volatile bool g_isActive = false;
volatile bool g_diag = false;
volatile int g_commanded = 0;
uint32_t g_count = 0;
esp_timer_handle_t g_offTimer = nullptr;
esp_timer_handle_t g_diagTimer = nullptr;

void drive(int level) {
  g_commanded = level;
  if (g_pin != GPIO_NUM_NC) gpio_set_level(g_pin, level);
}

void offTimerCb(void*) {
  if (g_enabled) drive(1 - g_active);
  g_isActive = false;
}

void diagTimerCb(void*) {
  if (!g_enabled) drive(1 - g_active);
  g_diag = false;
}

}  // namespace

void init(int pinNum, bool outputEnabled, int activeLevel) {
  g_pin = static_cast<gpio_num_t>(pinNum);
  g_enabled = outputEnabled;
  g_active = activeLevel ? 1 : 0;
  const int rest = 1 - g_active;

  // Primero el nivel, después la dirección: el pin nunca pasa por "activo".
  gpio_set_level(g_pin, rest);
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pinNum;
  cfg.mode = GPIO_MODE_INPUT_OUTPUT;  // salida + lectura del pad para diagnóstico
  cfg.pull_up_en = rest ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = rest ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
  // Fuerza de salida mínima (~5 mA): alcanza para la base del transistor
  // (1 kΩ => ~2,5 mA) y protege el pin si la resistencia de base faltara.
  gpio_set_drive_capability(g_pin, GPIO_DRIVE_CAP_0);
  drive(rest);
  g_isActive = false;

  if (!g_offTimer) {
    esp_timer_create_args_t a = {};
    a.callback = &offTimerCb;
    a.name = "relay_off";
    esp_timer_create(&a, &g_offTimer);
  }
  if (!g_diagTimer) {
    esp_timer_create_args_t a = {};
    a.callback = &diagTimerCb;
    a.name = "gpio_diag";
    esp_timer_create(&a, &g_diagTimer);
  }
}

bool outputEnabled() { return g_enabled; }
int pin() { return static_cast<int>(g_pin); }
int activeLevel() { return g_active; }
int restLevel() { return 1 - g_active; }

void activate(uint32_t ms) {
  if (g_isActive || ms == 0) return;
  if (g_diag) {  // un pulso cancela cualquier prueba manual del pin
    esp_timer_stop(g_diagTimer);
    g_diag = false;
    drive(1 - g_active);
  }
  g_isActive = true;
  ++g_count;
  if (g_enabled) drive(g_active);
  esp_timer_start_once(g_offTimer, static_cast<uint64_t>(ms) * 1000ULL);
}

void off() {
  if (g_offTimer) esp_timer_stop(g_offTimer);  // si no estaba corriendo, no pasa nada
  if (g_enabled) drive(1 - g_active);
  g_isActive = false;
}

bool isActive() { return g_isActive; }
uint32_t activations() { return g_count; }
int commandedLevel() { return g_commanded; }
int padLevel() { return g_pin == GPIO_NUM_NC ? -1 : gpio_get_level(g_pin); }

bool diagSet(int level, uint32_t autoRestMs) {
  if (g_enabled) return false;
  esp_timer_stop(g_diagTimer);
  const int rest = 1 - g_active;
  drive(level ? 1 : 0);
  g_diag = ((level ? 1 : 0) != rest);
  if (g_diag && autoRestMs) esp_timer_start_once(g_diagTimer, static_cast<uint64_t>(autoRestMs) * 1000ULL);
  return true;
}

bool diagActive() { return g_diag; }

}  // namespace relayhw
