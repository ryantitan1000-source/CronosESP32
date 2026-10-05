// ATENCIÓN: esta suite PONE GPIO4 EN ALTO (hasta ~260 ms). Con el transistor
// conectado, el relé hace clic. Ejecutar solo con los cables del molinete
// desconectados:  pio test -e hwtest -f test_hw_gpio4
#include <Arduino.h>
#include <unity.h>

#include "config.h"
#include "relay_hw.h"

void setUp() {}
void tearDown() { relayhw::off(); }

void test_gpio4_diag_high_low_readback() {
  // El pad debe seguir al nivel ordenado (detecta cortos o cargas excesivas).
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
  RUN_TEST(test_gpio4_diag_high_low_readback);
  RUN_TEST(test_gpio4_diag_auto_returns_to_rest);
  RUN_TEST(test_pulse_cancels_manual_diag);
  UNITY_END();
  relayhw::init(PIN_RELAY, false, 1);  // dejar el pin en reposo
}

void loop() {}
