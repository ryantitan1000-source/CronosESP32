// Pruebas en la placa sin periféricos: temporizador de apagado del relé
// (en SIMULACIÓN: GPIO4 queda en reposo 0 V todo el tiempo) y lectura del pin.
#include <Arduino.h>
#include <unity.h>

#include "config.h"
#include "relay_hw.h"

void setUp() {}
void tearDown() { relayhw::off(); }

void test_relay_sim_pin_stays_at_rest() {
  relayhw::init(PIN_RELAY, false, 1);
  TEST_ASSERT_FALSE(relayhw::outputEnabled());
  TEST_ASSERT_EQUAL_INT(0, relayhw::commandedLevel());
  TEST_ASSERT_EQUAL_INT(0, relayhw::padLevel());
  relayhw::activate(200);
  TEST_ASSERT_TRUE(relayhw::isActive());
  TEST_ASSERT_EQUAL_INT(0, relayhw::commandedLevel());  // simulación: el pin no se mueve
  TEST_ASSERT_EQUAL_INT(0, relayhw::padLevel());
}

void test_relay_timer_turns_off_while_loop_is_blocked() {
  relayhw::init(PIN_RELAY, false, 1);
  const uint32_t before = relayhw::activations();
  relayhw::activate(300);
  delay(250);  // bloqueo: el apagado no depende de loop()
  TEST_ASSERT_TRUE(relayhw::isActive());
  delay(100);
  TEST_ASSERT_FALSE(relayhw::isActive());
  TEST_ASSERT_EQUAL_UINT32(before + 1, relayhw::activations());
}

void test_relay_ignores_retrigger_while_active() {
  relayhw::init(PIN_RELAY, false, 1);
  const uint32_t before = relayhw::activations();
  relayhw::activate(300);
  relayhw::activate(5000);  // ignorado
  delay(350);
  TEST_ASSERT_FALSE(relayhw::isActive());
  TEST_ASSERT_EQUAL_UINT32(before + 1, relayhw::activations());
}

void test_gpio4_diag_high_low_readback() {
  // Prueba eléctrica interna: el pad debe seguir al nivel ordenado.
  // (Requiere IN del relé DESCONECTADO.)
  relayhw::init(PIN_RELAY, false, 1);
  TEST_ASSERT_TRUE(relayhw::diagSet(1, 1000));
  delay(5);
  TEST_ASSERT_EQUAL_INT(1, relayhw::padLevel());
  TEST_ASSERT_TRUE(relayhw::diagActive());
  TEST_ASSERT_TRUE(relayhw::diagSet(0, 0));
  delay(5);
  TEST_ASSERT_EQUAL_INT(0, relayhw::padLevel());
  TEST_ASSERT_FALSE(relayhw::diagActive());
}

void test_gpio4_diag_auto_returns_to_rest() {
  relayhw::init(PIN_RELAY, false, 1);
  relayhw::diagSet(1, 200);
  delay(260);
  TEST_ASSERT_EQUAL_INT(0, relayhw::commandedLevel());
  TEST_ASSERT_EQUAL_INT(0, relayhw::padLevel());
}

void test_pulse_cancels_manual_diag() {
  relayhw::init(PIN_RELAY, false, 1);
  relayhw::diagSet(1, 60000);
  relayhw::activate(100);
  TEST_ASSERT_EQUAL_INT(0, relayhw::commandedLevel());
  TEST_ASSERT_FALSE(relayhw::diagActive());
}

void setup() {
  delay(2500);
  UNITY_BEGIN();
  RUN_TEST(test_relay_sim_pin_stays_at_rest);
  RUN_TEST(test_relay_timer_turns_off_while_loop_is_blocked);
  RUN_TEST(test_relay_ignores_retrigger_while_active);
  RUN_TEST(test_gpio4_diag_high_low_readback);
  RUN_TEST(test_gpio4_diag_auto_returns_to_rest);
  RUN_TEST(test_pulse_cancels_manual_diag);
  UNITY_END();
  relayhw::init(PIN_RELAY, false, 1);  // dejar el pin en reposo
}

void loop() {}
