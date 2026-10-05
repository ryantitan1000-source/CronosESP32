// Prueba de integración con el lector DY50 REAL conectado (GPIO20/21).
// Ejecutar solo con el lector cableado:  pio test -e hwtest
#include <Arduino.h>
#include <unity.h>

#include "config.h"
#include "dy50_sensor.h"

static Dy50Sensor sensor(FP_UART, PIN_FP_RX, PIN_FP_TX);
static uint32_t foundBaud = 0;

void setUp() {}
void tearDown() {}

void test_reader_answers() {
  sensor.begin(FP_DEFAULT_BAUD);
  delay(300);
  if (sensor.ping() || sensor.ping()) {
    foundBaud = FP_DEFAULT_BAUD;
  } else {
    foundBaud = sensor.findBaud(nullptr);
  }
  char msg[64];
  snprintf(msg, sizeof msg, "sin respuesta a ninguna velocidad");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0, foundBaud, msg);
  TEST_MESSAGE(foundBaud == FP_DEFAULT_BAUD ? "responde a 57600 (valor de fabrica)" : "responde a otra velocidad");
}

void test_password_verification_ok() {
  TEST_ASSERT_TRUE(sensor.run(fp::verifyPassword(0)) == Dy50Sensor::Link::kOk);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(fp::cc::OK, sensor.lastCode(), "VfyPwd con clave 0 rechazada");
}

void test_sys_params_are_consistent() {
  fp::SysParams p{};
  TEST_ASSERT_TRUE(sensor.readSysParams(p) == Dy50Sensor::Link::kOk);
  TEST_ASSERT_EQUAL_HEX8(fp::cc::OK, sensor.lastCode());
  char msg[160];
  snprintf(msg, sizeof msg, "capacidad=%u seguridad=%u paquete=%u N=%u sysid=0x%04X estado=0x%04X",
           p.librarySize, p.securityLevel, p.packetSizeCode, p.baudMultiplier, p.systemId, p.statusRegister);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(p.librarySize >= 1 && p.librarySize <= 3000);
  TEST_ASSERT_TRUE(p.securityLevel >= 1 && p.securityLevel <= 5);
  TEST_ASSERT_TRUE(p.packetSizeCode <= 3);
  TEST_ASSERT_EQUAL_UINT32(sensor.baud(), p.baudRate());
}

void test_template_count_matches_index_table() {
  uint16_t count = 0;
  TEST_ASSERT_TRUE(sensor.run(fp::templateNum()) == Dy50Sensor::Link::kOk);
  TEST_ASSERT_TRUE(fp::parseTemplateCount(sensor.lastAck(), count));
  static uint16_t ids[1024];
  size_t n = 0;
  TEST_ASSERT_TRUE(sensor.usedIds(ids, 1024, n) == Dy50Sensor::Link::kOk);
  TEST_ASSERT_EQUAL_HEX8(fp::cc::OK, sensor.lastCode());
  char msg[64];
  snprintf(msg, sizeof msg, "TemplateNum=%u, tabla=%u", count, static_cast<unsigned>(n));
  TEST_MESSAGE(msg);
  TEST_ASSERT_EQUAL_UINT32(count, n);
}

void test_capture_answers_without_comm_error() {
  // Sin dedo debería responder "no hay dedo"; con dedo, OK. Nunca error de enlace.
  for (int i = 0; i < 10; ++i) {
    const FingerPort::Status s = sensor.capture();
    TEST_ASSERT_TRUE(s != FingerPort::Status::kCommError);
    delay(50);
  }
  const auto& st = sensor.stats();
  char msg[96];
  snprintf(msg, sizeof msg, "tx=%lu rx=%lu timeouts=%lu checksum=%lu basura=%lu",
           (unsigned long)st.txFrames, (unsigned long)st.rxFrames, (unsigned long)st.timeouts,
           (unsigned long)st.badChecksum, (unsigned long)st.garbageBytes);
  TEST_MESSAGE(msg);
}

void setup() {
  delay(2500);
  UNITY_BEGIN();
  RUN_TEST(test_reader_answers);
  if (foundBaud) {
    RUN_TEST(test_password_verification_ok);
    RUN_TEST(test_sys_params_are_consistent);
    RUN_TEST(test_template_count_matches_index_table);
    RUN_TEST(test_capture_answers_without_comm_error);
  }
  UNITY_END();
}

void loop() {}
