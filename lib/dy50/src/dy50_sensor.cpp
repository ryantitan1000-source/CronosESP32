#include "dy50_sensor.h"

Dy50Sensor::Dy50Sensor(HardwareSerial& uart, int8_t rxPin, int8_t txPin, uint32_t address,
                       uint32_t password)
    : uart_(uart), rxPin_(rxPin), txPin_(txPin), address_(address), password_(password) {}

void Dy50Sensor::begin(uint32_t baud) {
  uart_.setRxBufferSize(256);
  uart_.begin(baud, SERIAL_8N1, rxPin_, txPin_);
  baud_ = baud;
}

void Dy50Sensor::setBaud(uint32_t baud) {
  uart_.flush();
  uart_.updateBaudRate(baud);
  baud_ = baud;
  delay(5);
  while (uart_.available()) {
    uart_.read();
    ++stats_.garbageBytes;
  }
}

void Dy50Sensor::idle() {
  if (idleHook_) idleHook_();
}

void Dy50Sensor::dump(const char* tag, const uint8_t* data, size_t n) {
  if (!trace_) return;
  trace_->printf("[UART %s %u B] ", tag, static_cast<unsigned>(n));
  for (size_t i = 0; i < n; ++i) trace_->printf("%02X ", data[i]);
  trace_->println();
}

Dy50Sensor::Link Dy50Sensor::run(const fp::Command& cmd, uint32_t timeoutMs) {
  // Descarta bytes viejos (ruido, log de arranque, respuestas tardías).
  while (uart_.available()) {
    uart_.read();
    ++stats_.garbageBytes;
  }

  uint8_t frame[fp::kMaxFrame];
  const size_t n = fp::buildCommandFrame(address_, cmd, frame, sizeof frame);
  dump("TX", frame, n);
  uart_.write(frame, n);
  uart_.flush();  // espera a que salga el último bit
  ++stats_.txFrames;

  parser_.reset();
  uint8_t rxLog[fp::kMaxFrame + 16];
  size_t rxLogLen = 0;
  Link result = Link::kTimeout;
  bool done = false;

  const uint32_t start = millis();
  while (!done && static_cast<uint32_t>(millis() - start) < timeoutMs) {
    while (!done && uart_.available()) {
      const uint8_t b = static_cast<uint8_t>(uart_.read());
      if (rxLogLen < sizeof rxLog) rxLog[rxLogLen++] = b;
      switch (parser_.feed(b)) {
        case fp::FrameParser::Result::kNeedMore:
          break;
        case fp::FrameParser::Result::kComplete:
          ++stats_.rxFrames;
          lastFrame_ = parser_.frame();
          result = fp::parseAck(lastFrame_, lastAck_) ? Link::kOk : Link::kNotAck;
          done = true;
          break;
        case fp::FrameParser::Result::kBadChecksum:
          ++stats_.badChecksum;
          result = Link::kBadChecksum;
          done = true;
          break;
        case fp::FrameParser::Result::kBadLength:
        case fp::FrameParser::Result::kTooLong:
          ++stats_.badFrames;
          result = Link::kBadFrame;
          done = true;
          break;
      }
    }
    if (!done) {
      idle();
      delay(1);
    }
  }
  stats_.garbageBytes += parser_.discarded();
  if (result == Link::kTimeout) ++stats_.timeouts;
  if (trace_) {
    dump("RX", rxLog, rxLogLen);
    if (result != Link::kOk) trace_->printf("[UART] resultado: %s\n", linkText(result));
  }
  if (result != Link::kOk) lastAck_ = fp::Ack{0xFF, nullptr, 0};
  return result;
}

bool Dy50Sensor::ping() {
  return run(fp::verifyPassword(password_), kTimeoutPing) == Link::kOk;
}

uint32_t Dy50Sensor::findBaud(Print* log) {
  // Primero 57600 (valor de fábrica habitual), luego el resto de N x 9600.
  static const uint32_t kOrder[] = {57600, 9600,  19200, 38400, 115200, 28800,
                                    48000, 67200, 76800, 86400, 96000,  105600};
  const uint32_t original = baud_;
  for (uint32_t b : kOrder) {
    setBaud(b);
    if (log) log->printf("  probando %6lu baudios... ", static_cast<unsigned long>(b));
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (ping()) {
        if (log) log->println("RESPONDE");
        return b;
      }
      idle();
    }
    if (log) log->println("sin respuesta");
  }
  setBaud(original);
  return 0;
}

Dy50Sensor::Link Dy50Sensor::readSysParams(fp::SysParams& out) {
  const Link l = run(fp::readSysPara());
  if (l == Link::kOk && fp::parseSysParams(lastAck_, out)) {
    librarySize_ = out.librarySize;
  }
  return l;
}

Dy50Sensor::Link Dy50Sensor::usedIds(uint16_t* ids, size_t maxIds, size_t& n) {
  n = 0;
  const uint16_t size = librarySize_ ? librarySize_ : 256;
  const uint8_t pages = static_cast<uint8_t>((size + 255) / 256);
  for (uint8_t p = 0; p < pages; ++p) {
    const Link l = run(fp::readIndexTable(p));
    if (l != Link::kOk) return l;
    if (lastAck_.code != fp::cc::OK) return l;  // quien llama mira lastCode()
    uint16_t tmp[256];
    const size_t got = fp::parseIndexTable(lastAck_, p, tmp, 256);
    for (size_t i = 0; i < got && n < maxIds; ++i) {
      if (tmp[i] < size) ids[n++] = tmp[i];
    }
  }
  return Link::kOk;
}

size_t Dy50Sensor::rawExchange(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxCap,
                               uint32_t waitMs) {
  while (uart_.available()) uart_.read();
  uart_.write(tx, txLen);
  uart_.flush();
  size_t n = 0;
  const uint32_t start = millis();
  while (static_cast<uint32_t>(millis() - start) < waitMs) {
    while (uart_.available() && n < rxCap) rx[n++] = static_cast<uint8_t>(uart_.read());
    idle();
    delay(1);
  }
  return n;
}

FingerPort::Status Dy50Sensor::capture() {
  if (run(fp::genImg()) != Link::kOk) return Status::kCommError;
  switch (lastAck_.code) {
    case fp::cc::OK: return Status::kOk;
    case fp::cc::NO_FINGER: return Status::kNoFinger;
    case fp::cc::IMAGE_FAIL: return Status::kImageError;
    default: return Status::kOtherError;
  }
}

FingerPort::Status Dy50Sensor::extract(uint8_t bufferId) {
  if (run(fp::img2Tz(bufferId)) != Link::kOk) return Status::kCommError;
  switch (lastAck_.code) {
    case fp::cc::OK: return Status::kOk;
    case fp::cc::IMAGE_MESSY:
    case fp::cc::FEATURE_FAIL:
    case fp::cc::INVALID_IMAGE: return Status::kImageError;
    default: return Status::kOtherError;
  }
}

FingerPort::Status Dy50Sensor::searchAll(uint16_t& pageId, uint16_t& score) {
  const uint16_t count = librarySize_ ? librarySize_ : 0x00A3;
  if (run(fp::search(1, 0, count), kTimeoutSearch) != Link::kOk) return Status::kCommError;
  if (lastAck_.code == fp::cc::NOT_FOUND) return Status::kNotFound;
  fp::SearchResult r{};
  if (fp::parseSearch(lastAck_, r)) {
    // Una coincidencia con un ID fuera de la base no puede ser real: respuesta inválida.
    if (r.pageId >= count) return Status::kOtherError;
    pageId = r.pageId;
    score = r.score;
    return Status::kOk;
  }
  return Status::kOtherError;
}

const char* Dy50Sensor::linkText(Link l) {
  switch (l) {
    case Link::kOk: return "OK";
    case Link::kTimeout: return "sin respuesta (timeout)";
    case Link::kBadChecksum: return "checksum incorrecto";
    case Link::kBadFrame: return "trama con largo invalido";
    case Link::kNotAck: return "trama que no es respuesta";
  }
  return "?";
}
