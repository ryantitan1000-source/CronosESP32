// Pruebas del protocolo EF01 (se ejecutan en la placa: pio test -e supermini).
// Las tramas esperadas se calcularon a mano con la regla del checksum
// (suma de PID + largo + contenido) y coinciden con los ejemplos de los
// manuales R30x/AS608 (GenImg, VfyPwd, Img2Tz).
#include <Arduino.h>
#include <string.h>
#include <unity.h>

#include "fp_proto.h"

using namespace fp;

void setUp() {}
void tearDown() {}

static void expectFrame(const Command& c, const uint8_t* expected, size_t n) {
  uint8_t out[kMaxFrame];
  const size_t len = buildCommandFrame(kDefaultAddress, c, out, sizeof out);
  TEST_ASSERT_EQUAL_UINT32(n, len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, n);
}

void test_genimg_frame() {
  const uint8_t e[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x01, 0x00, 0x05};
  expectFrame(genImg(), e, sizeof e);
}

void test_vfypwd_frame() {
  const uint8_t e[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x07,
                       0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B};
  expectFrame(verifyPassword(0), e, sizeof e);
}

void test_img2tz_frame() {
  const uint8_t e1[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x02, 0x01, 0x00, 0x08};
  expectFrame(img2Tz(1), e1, sizeof e1);
  const uint8_t e2[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x02, 0x02, 0x00, 0x09};
  expectFrame(img2Tz(2), e2, sizeof e2);
}

void test_search_frame() {
  // Search buffer 1, desde 0, 163 posiciones (0x00A3)
  const uint8_t e[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x08,
                       0x04, 0x01, 0x00, 0x00, 0x00, 0xA3, 0x00, 0xB1};
  expectFrame(search(1, 0, 0x00A3), e, sizeof e);
}

void test_other_command_frames() {
  const uint8_t reg[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x05, 0x00, 0x09};
  expectFrame(regModel(), reg, sizeof reg);
  const uint8_t st[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x06, 0x06, 0x01, 0x00, 0x05, 0x00, 0x13};
  expectFrame(store(1, 5), st, sizeof st);
  const uint8_t del[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x07,
                         0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x1A};
  expectFrame(deleteChar(5, 1), del, sizeof del);
  const uint8_t emp[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x0D, 0x00, 0x11};
  expectFrame(empty(), emp, sizeof emp);
  const uint8_t rsp[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x0F, 0x00, 0x13};
  expectFrame(readSysPara(), rsp, sizeof rsp);
  const uint8_t tn[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x1D, 0x00, 0x21};
  expectFrame(templateNum(), tn, sizeof tn);
  const uint8_t rit[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x1F, 0x00, 0x00, 0x24};
  expectFrame(readIndexTable(0), rit, sizeof rit);
}

void test_build_frame_rejects_small_buffer() {
  uint8_t out[8];
  TEST_ASSERT_EQUAL_UINT32(0, buildCommandFrame(kDefaultAddress, genImg(), out, sizeof out));
}

static FrameParser::Result feedAll(FrameParser& p, const uint8_t* d, size_t n) {
  FrameParser::Result r = FrameParser::Result::kNeedMore;
  for (size_t i = 0; i < n; ++i) {
    r = p.feed(d[i]);
    if (r != FrameParser::Result::kNeedMore) return r;
  }
  return r;
}

void test_parse_ok_ack() {
  const uint8_t ack[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, ack, sizeof ack));
  Ack a{};
  TEST_ASSERT_TRUE(parseAck(p.frame(), a));
  TEST_ASSERT_EQUAL_HEX8(cc::OK, a.code);
  TEST_ASSERT_EQUAL_UINT32(0, a.paramLen);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, p.frame().address);
}

void test_parse_no_finger_ack() {
  const uint8_t ack[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x02, 0x00, 0x0C};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, ack, sizeof ack));
  Ack a{};
  TEST_ASSERT_TRUE(parseAck(p.frame(), a));
  TEST_ASSERT_EQUAL_HEX8(cc::NO_FINGER, a.code);
}

void test_parse_skips_garbage_and_false_header() {
  // Basura (log de arranque, 0x55 de encendido, EF suelto, EF EF) antes de la trama.
  const uint8_t d[] = {0x55, 0x00, 0xEF, 0x33, 0xEF, 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                       0x07, 0x00, 0x03, 0x02, 0x00, 0x0C};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, d, sizeof d));
  TEST_ASSERT_EQUAL_HEX8(0x02, p.frame().content[0]);
  TEST_ASSERT_TRUE(p.discarded() >= 4);
}

void test_parse_bad_checksum() {
  const uint8_t ack[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0B};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kBadChecksum, feedAll(p, ack, sizeof ack));
}

void test_parse_rejects_too_long_and_bad_length() {
  const uint8_t big[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x01, 0x00};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kTooLong, feedAll(p, big, sizeof big));
  const uint8_t tiny[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x01};
  TEST_ASSERT_EQUAL(FrameParser::Result::kBadLength, feedAll(p, tiny, sizeof tiny));
}

void test_parse_resyncs_after_invalid_pid() {
  // PID 0x55 inválido: descartar y encontrar la trama siguiente.
  const uint8_t d[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x55,
                       0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, d, sizeof d));
  TEST_ASSERT_EQUAL_HEX8(PID_ACK, p.frame().pid);
}

void test_parse_two_frames_in_a_row_and_split_feed() {
  const uint8_t a1[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A};
  const uint8_t a2[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x02, 0x00, 0x0C};
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kNeedMore, feedAll(p, a1, 5));
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, a1 + 5, sizeof a1 - 5));
  TEST_ASSERT_EQUAL_HEX8(0x00, p.frame().content[0]);
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, a2, sizeof a2));
  TEST_ASSERT_EQUAL_HEX8(0x02, p.frame().content[0]);
}

// Construye una respuesta ACK válida con buildFrame y la analiza.
static Ack makeAck(Frame& storage, const uint8_t* content, size_t n) {
  uint8_t buf[kMaxFrame];
  const size_t len = buildFrame(kDefaultAddress, PID_ACK, content, n, buf, sizeof buf);
  FrameParser p;
  TEST_ASSERT_EQUAL(FrameParser::Result::kComplete, feedAll(p, buf, len));
  storage = p.frame();
  Ack a{};
  TEST_ASSERT_TRUE(parseAck(storage, a));
  return a;
}

void test_parse_sys_params() {
  // estado 0x0000, sysid 0x0009, capacidad 300, seguridad 3, addr FFFFFFFF, paquete 2 (128 B), N=6
  const uint8_t c[] = {0x00, 0x00, 0x00, 0x00, 0x09, 0x01, 0x2C, 0x00, 0x03,
                       0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x06};
  Frame f{};
  const Ack a = makeAck(f, c, sizeof c);
  SysParams s{};
  TEST_ASSERT_TRUE(parseSysParams(a, s));
  TEST_ASSERT_EQUAL_UINT16(0x0009, s.systemId);
  TEST_ASSERT_EQUAL_UINT16(300, s.librarySize);
  TEST_ASSERT_EQUAL_UINT16(3, s.securityLevel);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, s.address);
  TEST_ASSERT_EQUAL_UINT16(128, s.packetBytes());
  TEST_ASSERT_EQUAL_UINT32(57600, s.baudRate());
}

void test_parse_sys_params_rejects_short_or_error() {
  const uint8_t shortC[] = {0x00, 0x00, 0x01};
  Frame f{};
  Ack a = makeAck(f, shortC, sizeof shortC);
  SysParams s{};
  TEST_ASSERT_FALSE(parseSysParams(a, s));
  const uint8_t err[] = {0x01};
  a = makeAck(f, err, sizeof err);
  TEST_ASSERT_FALSE(parseSysParams(a, s));
}

void test_parse_search_and_count() {
  const uint8_t c[] = {0x00, 0x00, 0x05, 0x00, 0x64};  // ID 5, puntaje 100
  Frame f{};
  Ack a = makeAck(f, c, sizeof c);
  SearchResult r{};
  TEST_ASSERT_TRUE(parseSearch(a, r));
  TEST_ASSERT_EQUAL_UINT16(5, r.pageId);
  TEST_ASSERT_EQUAL_UINT16(100, r.score);

  const uint8_t nf[] = {0x09};
  a = makeAck(f, nf, sizeof nf);
  TEST_ASSERT_FALSE(parseSearch(a, r));
  TEST_ASSERT_EQUAL_HEX8(cc::NOT_FOUND, a.code);

  const uint8_t tn[] = {0x00, 0x00, 0x0C};
  a = makeAck(f, tn, sizeof tn);
  uint16_t count = 0;
  TEST_ASSERT_TRUE(parseTemplateCount(a, count));
  TEST_ASSERT_EQUAL_UINT16(12, count);
}

void test_parse_index_table() {
  uint8_t c[33] = {0};
  c[0] = 0x00;       // OK
  c[1] = 0b00000011;  // IDs 0 y 1
  c[2] = 0b10000000;  // ID 15
  c[32] = 0b00000001; // ID 248
  Frame f{};
  const Ack a = makeAck(f, c, sizeof c);
  uint16_t ids[16];
  TEST_ASSERT_EQUAL_UINT32(4, parseIndexTable(a, 0, ids, 16));
  TEST_ASSERT_EQUAL_UINT16(0, ids[0]);
  TEST_ASSERT_EQUAL_UINT16(1, ids[1]);
  TEST_ASSERT_EQUAL_UINT16(15, ids[2]);
  TEST_ASSERT_EQUAL_UINT16(248, ids[3]);
  // Página de índice 1 => IDs desplazados 256
  TEST_ASSERT_EQUAL_UINT32(4, parseIndexTable(a, 1, ids, 16));
  TEST_ASSERT_EQUAL_UINT16(256, ids[0]);
}

void test_code_text_known_and_unknown() {
  TEST_ASSERT_EQUAL_STRING("OK", codeText(cc::OK));
  TEST_ASSERT_EQUAL_STRING("no hay dedo sobre el sensor", codeText(cc::NO_FINGER));
  TEST_ASSERT_EQUAL_STRING("codigo desconocido", codeText(0x77));
}

void setup() {
  delay(2500);  // dar tiempo a que la PC reabra el puerto USB tras el reinicio
  UNITY_BEGIN();
  RUN_TEST(test_genimg_frame);
  RUN_TEST(test_vfypwd_frame);
  RUN_TEST(test_img2tz_frame);
  RUN_TEST(test_search_frame);
  RUN_TEST(test_other_command_frames);
  RUN_TEST(test_build_frame_rejects_small_buffer);
  RUN_TEST(test_parse_ok_ack);
  RUN_TEST(test_parse_no_finger_ack);
  RUN_TEST(test_parse_skips_garbage_and_false_header);
  RUN_TEST(test_parse_bad_checksum);
  RUN_TEST(test_parse_rejects_too_long_and_bad_length);
  RUN_TEST(test_parse_resyncs_after_invalid_pid);
  RUN_TEST(test_parse_two_frames_in_a_row_and_split_feed);
  RUN_TEST(test_parse_sys_params);
  RUN_TEST(test_parse_sys_params_rejects_short_or_error);
  RUN_TEST(test_parse_search_and_count);
  RUN_TEST(test_parse_index_table);
  RUN_TEST(test_code_text_known_and_unknown);
  UNITY_END();
}

void loop() {}
