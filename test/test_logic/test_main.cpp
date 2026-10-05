// Pruebas de la lógica del pulso del relé y del control de acceso, con un
// lector simulado y tiempo simulado (no tocan ningún pin).
#include <Arduino.h>
#include <unity.h>

#include "access_controller.h"
#include "relay_pulse.h"

using Status = FingerPort::Status;

// ------------------------------------------------------------- lector falso

class FakeFinger : public FingerPort {
 public:
  bool fingerOn = false;
  bool online = true;
  Status extractResult = Status::kOk;
  Status searchResult = Status::kOk;
  uint16_t id = 7;
  uint16_t score = 120;
  int captures = 0, extracts = 0, searches = 0, pings = 0;

  Status capture() override {
    ++captures;
    if (!online) return Status::kCommError;
    return fingerOn ? Status::kOk : Status::kNoFinger;
  }
  Status extract(uint8_t) override {
    ++extracts;
    return online ? extractResult : Status::kCommError;
  }
  Status searchAll(uint16_t& pid, uint16_t& sc) override {
    ++searches;
    if (!online) return Status::kCommError;
    pid = id;
    sc = score;
    return searchResult;
  }
  bool ping() override {
    ++pings;
    return online;
  }
};

static AccessConfig cfg() {
  AccessConfig c;
  c.minScore = 50;
  c.releasePolls = 3;
  c.maxFails = 3;
  c.lockoutMs = 10000;
  c.commErrorsOffline = 3;
  c.reconnectMs = 1000;
  return c;
}

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------- RelayPulse

void test_relay_pulse_turns_off_after_duration() {
  RelayPulse r(1000, 500, 100, 10000);
  TEST_ASSERT_EQUAL(RelayPulse::Request::kStarted, r.request(0));
  TEST_ASSERT_TRUE(r.update(999));
  TEST_ASSERT_EQUAL_UINT32(1, r.remainingMs(999));
  TEST_ASSERT_FALSE(r.update(1000));
  TEST_ASSERT_FALSE(r.active());
}

void test_relay_pulse_no_retrigger_while_active_and_cooldown() {
  RelayPulse r(1000, 500, 100, 10000);
  TEST_ASSERT_EQUAL(RelayPulse::Request::kStarted, r.request(0));
  TEST_ASSERT_EQUAL(RelayPulse::Request::kBusy, r.request(400));
  TEST_ASSERT_EQUAL(RelayPulse::Request::kCooldown, r.request(1200));  // terminó en 1000, enfriamiento hasta 1500
  TEST_ASSERT_EQUAL(RelayPulse::Request::kStarted, r.request(1500));
  TEST_ASSERT_EQUAL_UINT32(2, r.pulsesStarted());
}

void test_relay_pulse_clamps_duration() {
  RelayPulse r(1000, 0, 100, 10000);
  TEST_ASSERT_EQUAL_UINT32(100, r.setPulseMs(5));
  TEST_ASSERT_EQUAL_UINT32(10000, r.setPulseMs(60000));
  TEST_ASSERT_EQUAL_UINT32(2500, r.setPulseMs(2500));
}

void test_relay_pulse_survives_millis_wraparound() {
  RelayPulse r(1000, 500, 100, 10000);
  const uint32_t t0 = 0xFFFFFE00u;  // 512 ms antes de la vuelta
  TEST_ASSERT_EQUAL(RelayPulse::Request::kStarted, r.request(t0));
  TEST_ASSERT_TRUE(r.update(t0 + 900));   // ya dio la vuelta
  TEST_ASSERT_FALSE(r.update(t0 + 1000));
  TEST_ASSERT_EQUAL(RelayPulse::Request::kCooldown, r.request(t0 + 1200));
  TEST_ASSERT_EQUAL(RelayPulse::Request::kStarted, r.request(t0 + 1500));
}

void test_relay_force_off() {
  RelayPulse r(5000, 0, 100, 10000);
  r.request(0);
  r.forceOff(10);
  TEST_ASSERT_FALSE(r.active());
  TEST_ASSERT_FALSE(r.update(20));
}

// ------------------------------------------------------------- AccessController

void test_access_no_finger_does_nothing() {
  FakeFinger f;
  AccessController a(f, cfg());
  for (uint32_t t = 0; t < 1000; t += 100) {
    TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(t).type);
  }
  TEST_ASSERT_EQUAL(0, f.searches);
}

void test_access_authorized_grants_once_while_finger_held() {
  FakeFinger f;
  AccessController a(f, cfg());
  f.fingerOn = true;
  int grants = 0;
  for (uint32_t t = 0; t < 5000; t += 100) {
    const AccessEvent e = a.step(t);
    if (e.type == AccessEventType::kGranted) {
      ++grants;
      TEST_ASSERT_EQUAL_UINT16(7, e.id);
      TEST_ASSERT_EQUAL_UINT16(120, e.score);
    }
  }
  TEST_ASSERT_EQUAL(1, grants);  // dedo apoyado 5 s => un solo pulso
  TEST_ASSERT_EQUAL(AccessController::State::kWaitRelease, a.state());
}

void test_access_regrants_after_release() {
  FakeFinger f;
  AccessController a(f, cfg());
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(0).type);
  f.fingerOn = false;
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(100).type);
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(200).type);
  TEST_ASSERT_EQUAL(AccessEventType::kFingerReleased, a.step(300).type);
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(400).type);
}

void test_access_brief_lift_does_not_count_as_release() {
  FakeFinger f;
  AccessController a(f, cfg());
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(0).type);
  f.fingerOn = false;
  a.step(100);
  a.step(200);           // 2 lecturas sin dedo (se necesitan 3)
  f.fingerOn = true;     // vuelve a apoyar: reinicia la cuenta
  a.step(300);
  f.fingerOn = false;
  a.step(400);
  a.step(500);
  TEST_ASSERT_EQUAL(AccessController::State::kWaitRelease, a.state());
  TEST_ASSERT_EQUAL(AccessEventType::kFingerReleased, a.step(600).type);
}

void test_access_unknown_finger_denied_no_grant() {
  FakeFinger f;
  f.searchResult = Status::kNotFound;
  AccessController a(f, cfg());
  f.fingerOn = true;
  const AccessEvent e = a.step(0);
  TEST_ASSERT_EQUAL(AccessEventType::kDenied, e.type);
  TEST_ASSERT_FALSE(e.lowScore);
  for (uint32_t t = 100; t < 3000; t += 100) {
    TEST_ASSERT_NOT_EQUAL(AccessEventType::kGranted, a.step(t).type);
  }
}

void test_access_low_score_is_denied() {
  FakeFinger f;
  f.score = 49;  // mínimo 50
  AccessController a(f, cfg());
  f.fingerOn = true;
  const AccessEvent e = a.step(0);
  TEST_ASSERT_EQUAL(AccessEventType::kDenied, e.type);
  TEST_ASSERT_TRUE(e.lowScore);
  TEST_ASSERT_EQUAL_UINT16(7, e.id);
}

void test_access_image_error_retries_without_counting_fail() {
  FakeFinger f;
  f.extractResult = Status::kImageError;
  AccessController a(f, cfg());
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kImageError, a.step(0).type);
  TEST_ASSERT_EQUAL(0, a.failCount());
  f.extractResult = Status::kOk;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(100).type);
}

static void denyAndRelease(AccessController& a, FakeFinger& f, uint32_t& t) {
  f.fingerOn = true;
  a.step(t += 100);
  f.fingerOn = false;
  for (int i = 0; i < 3; ++i) a.step(t += 100);
}

void test_access_lockout_after_max_fails_then_recovers() {
  FakeFinger f;
  f.searchResult = Status::kNotFound;
  AccessController a(f, cfg());
  uint32_t t = 0;
  denyAndRelease(a, f, t);
  denyAndRelease(a, f, t);
  f.fingerOn = true;
  const AccessEvent e = a.step(t += 100);
  TEST_ASSERT_EQUAL(AccessEventType::kDenied, e.type);
  TEST_ASSERT_TRUE(e.lockoutStarted);
  TEST_ASSERT_EQUAL(AccessController::State::kLockout, a.state());

  // Durante el bloqueo, ni siquiera una huella válida abre.
  f.searchResult = Status::kOk;
  const int searchesBefore = f.searches;
  for (int i = 0; i < 50; ++i) TEST_ASSERT_NOT_EQUAL(AccessEventType::kGranted, a.step(t += 100).type);
  TEST_ASSERT_EQUAL(searchesBefore, f.searches);

  // Fin del bloqueo: hay que retirar el dedo antes de volver a evaluar.
  t += 10000;
  TEST_ASSERT_EQUAL(AccessEventType::kLockoutEnded, a.step(t).type);
  TEST_ASSERT_EQUAL(0, a.failCount());
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(t += 100).type);  // dedo sigue apoyado
  f.fingerOn = false;
  a.step(t += 100);
  a.step(t += 100);
  TEST_ASSERT_EQUAL(AccessEventType::kFingerReleased, a.step(t += 100).type);
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(t += 100).type);
}

void test_access_grant_resets_fail_count() {
  FakeFinger f;
  f.searchResult = Status::kNotFound;
  AccessController a(f, cfg());
  uint32_t t = 0;
  denyAndRelease(a, f, t);
  denyAndRelease(a, f, t);
  TEST_ASSERT_EQUAL(2, a.failCount());
  f.searchResult = Status::kOk;
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(t += 100).type);
  TEST_ASSERT_EQUAL(0, a.failCount());
}

void test_access_reader_disconnect_and_reconnect() {
  FakeFinger f;
  AccessController a(f, cfg());
  f.online = false;
  TEST_ASSERT_EQUAL(AccessEventType::kCommError, a.step(0).type);
  TEST_ASSERT_EQUAL(AccessEventType::kCommError, a.step(100).type);
  TEST_ASSERT_EQUAL(AccessEventType::kReaderOffline, a.step(200).type);
  TEST_ASSERT_EQUAL(AccessController::State::kOffline, a.state());

  // No reintenta antes de reconnectMs.
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(500).type);
  TEST_ASSERT_EQUAL(0, f.pings);
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(1200).type);  // intento fallido
  TEST_ASSERT_EQUAL(1, f.pings);

  // Vuelve con un dedo autorizado apoyado: NO debe abrir hasta que se retire.
  f.online = true;
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kReaderOnline, a.step(2300).type);
  for (uint32_t t = 2400; t < 4000; t += 100) {
    TEST_ASSERT_NOT_EQUAL(AccessEventType::kGranted, a.step(t).type);
  }
  f.fingerOn = false;
  a.step(4100);
  a.step(4200);
  TEST_ASSERT_EQUAL(AccessEventType::kFingerReleased, a.step(4300).type);
}

void test_access_isolated_comm_error_does_not_go_offline() {
  FakeFinger f;
  AccessController a(f, cfg());
  f.online = false;
  TEST_ASSERT_EQUAL(AccessEventType::kCommError, a.step(0).type);
  TEST_ASSERT_EQUAL(AccessEventType::kCommError, a.step(100).type);
  f.online = true;
  TEST_ASSERT_EQUAL(AccessEventType::kNone, a.step(200).type);  // contador se reinicia
  f.online = false;
  TEST_ASSERT_EQUAL(AccessEventType::kCommError, a.step(300).type);
  TEST_ASSERT_EQUAL(AccessController::State::kIdle, a.state());
}

void test_access_require_release_blocks_immediate_eval() {
  FakeFinger f;
  AccessController a(f, cfg());
  a.requireRelease();  // como tras un reinicio
  f.fingerOn = true;
  for (uint32_t t = 0; t < 2000; t += 100) {
    TEST_ASSERT_NOT_EQUAL(AccessEventType::kGranted, a.step(t).type);
  }
  TEST_ASSERT_EQUAL(0, f.searches);
}

void test_access_min_score_zero_accepts_reader_decision() {
  FakeFinger f;
  f.score = 0;
  AccessConfig c = cfg();
  c.minScore = 0;
  AccessController a(f, c);
  f.fingerOn = true;
  TEST_ASSERT_EQUAL(AccessEventType::kGranted, a.step(0).type);
}

void setup() {
  delay(2500);
  UNITY_BEGIN();
  RUN_TEST(test_relay_pulse_turns_off_after_duration);
  RUN_TEST(test_relay_pulse_no_retrigger_while_active_and_cooldown);
  RUN_TEST(test_relay_pulse_clamps_duration);
  RUN_TEST(test_relay_pulse_survives_millis_wraparound);
  RUN_TEST(test_relay_force_off);
  RUN_TEST(test_access_no_finger_does_nothing);
  RUN_TEST(test_access_authorized_grants_once_while_finger_held);
  RUN_TEST(test_access_regrants_after_release);
  RUN_TEST(test_access_brief_lift_does_not_count_as_release);
  RUN_TEST(test_access_unknown_finger_denied_no_grant);
  RUN_TEST(test_access_low_score_is_denied);
  RUN_TEST(test_access_image_error_retries_without_counting_fail);
  RUN_TEST(test_access_lockout_after_max_fails_then_recovers);
  RUN_TEST(test_access_grant_resets_fail_count);
  RUN_TEST(test_access_reader_disconnect_and_reconnect);
  RUN_TEST(test_access_isolated_comm_error_does_not_go_offline);
  RUN_TEST(test_access_require_release_blocks_immediate_eval);
  RUN_TEST(test_access_min_score_zero_accepts_reader_decision);
  UNITY_END();
}

void loop() {}
