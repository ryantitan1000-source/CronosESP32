#include "fp_proto.h"

#include <string.h>

namespace fp {

namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t get32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

// ---------------------------------------------------------------- comandos

Command genImg() {
  Command c{};
  c.data[0] = ins::GEN_IMG;
  c.len = 1;
  return c;
}

Command img2Tz(uint8_t bufferId) {
  Command c{};
  c.data[0] = ins::IMG2TZ;
  c.data[1] = bufferId;
  c.len = 2;
  return c;
}

Command match() {
  Command c{};
  c.data[0] = ins::MATCH;
  c.len = 1;
  return c;
}

Command search(uint8_t bufferId, uint16_t startPage, uint16_t pageCount) {
  Command c{};
  c.data[0] = ins::SEARCH;
  c.data[1] = bufferId;
  put16(&c.data[2], startPage);
  put16(&c.data[4], pageCount);
  c.len = 6;
  return c;
}

Command regModel() {
  Command c{};
  c.data[0] = ins::REG_MODEL;
  c.len = 1;
  return c;
}

Command store(uint8_t bufferId, uint16_t pageId) {
  Command c{};
  c.data[0] = ins::STORE;
  c.data[1] = bufferId;
  put16(&c.data[2], pageId);
  c.len = 4;
  return c;
}

Command loadChar(uint8_t bufferId, uint16_t pageId) {
  Command c{};
  c.data[0] = ins::LOAD_CHAR;
  c.data[1] = bufferId;
  put16(&c.data[2], pageId);
  c.len = 4;
  return c;
}

Command deleteChar(uint16_t pageId, uint16_t count) {
  Command c{};
  c.data[0] = ins::DELETE_CHAR;
  put16(&c.data[1], pageId);
  put16(&c.data[3], count);
  c.len = 5;
  return c;
}

Command empty() {
  Command c{};
  c.data[0] = ins::EMPTY;
  c.len = 1;
  return c;
}

Command readSysPara() {
  Command c{};
  c.data[0] = ins::READ_SYS_PARA;
  c.len = 1;
  return c;
}

Command verifyPassword(uint32_t password) {
  Command c{};
  c.data[0] = ins::VFY_PWD;
  put32(&c.data[1], password);
  c.len = 5;
  return c;
}

Command templateNum() {
  Command c{};
  c.data[0] = ins::TEMPLATE_NUM;
  c.len = 1;
  return c;
}

Command readIndexTable(uint8_t indexPage) {
  Command c{};
  c.data[0] = ins::READ_INDEX_TABLE;
  c.data[1] = indexPage;
  c.len = 2;
  return c;
}

// ---------------------------------------------------------------- tramas

uint16_t checksum(uint8_t pid, const uint8_t* content, size_t contentLen) {
  const uint16_t lengthField = static_cast<uint16_t>(contentLen + 2);
  uint32_t sum = pid;
  sum += lengthField >> 8;
  sum += lengthField & 0xFF;
  for (size_t i = 0; i < contentLen; ++i) sum += content[i];
  return static_cast<uint16_t>(sum & 0xFFFF);
}

size_t buildFrame(uint32_t address, uint8_t pid, const uint8_t* content,
                  size_t contentLen, uint8_t* out, size_t outCap) {
  const size_t total = kFrameOverhead + contentLen;
  if (out == nullptr || total > outCap || contentLen > 0xFFFD) return 0;
  out[0] = kHeader0;
  out[1] = kHeader1;
  put32(&out[2], address);
  out[6] = pid;
  put16(&out[7], static_cast<uint16_t>(contentLen + 2));
  if (contentLen) memcpy(&out[9], content, contentLen);
  put16(&out[9 + contentLen], checksum(pid, content, contentLen));
  return total;
}

size_t buildCommandFrame(uint32_t address, const Command& cmd, uint8_t* out,
                         size_t outCap) {
  return buildFrame(address, PID_COMMAND, cmd.data, cmd.len, out, outCap);
}

// ---------------------------------------------------------------- análisis

void FrameParser::reset() {
  restart();
  discarded_ = 0;
}

void FrameParser::restart() {
  state_ = State::kHeader0;
  idx_ = 0;
  sumRx_ = 0;
  // 'frame_' conserva la última trama completa hasta que llegue otra.
}

FrameParser::Result FrameParser::feed(uint8_t b) {
  switch (state_) {
    case State::kHeader0:
      if (b == kHeader0) {
        state_ = State::kHeader1;
      } else {
        ++discarded_;
      }
      return Result::kNeedMore;

    case State::kHeader1:
      if (b == kHeader1) {
        state_ = State::kAddress;
        idx_ = 0;
        frame_.address = 0;
        frame_.contentLen = 0;
      } else if (b == kHeader0) {
        ++discarded_;  // el EF anterior era basura; este puede ser el bueno
      } else {
        discarded_ += 2;
        state_ = State::kHeader0;
      }
      return Result::kNeedMore;

    case State::kAddress:
      frame_.address = (frame_.address << 8) | b;
      if (++idx_ == 4) state_ = State::kPid;
      return Result::kNeedMore;

    case State::kPid:
      if (b != PID_COMMAND && b != PID_DATA && b != PID_ACK && b != PID_END_DATA) {
        discarded_ += 7;  // EF 01 + dirección + este byte
        restart();
        return Result::kNeedMore;
      }
      frame_.pid = b;
      frame_.lengthField = 0;
      idx_ = 0;
      state_ = State::kLength;
      return Result::kNeedMore;

    case State::kLength:
      frame_.lengthField = static_cast<uint16_t>((frame_.lengthField << 8) | b);
      if (++idx_ == 2) {
        if (frame_.lengthField < 2) {
          restart();
          return Result::kBadLength;
        }
        if (frame_.lengthField - 2u > kMaxContent) {
          restart();
          return Result::kTooLong;
        }
        frame_.contentLen = frame_.lengthField - 2u;
        idx_ = 0;
        state_ = frame_.contentLen ? State::kContent : State::kChecksum;
      }
      return Result::kNeedMore;

    case State::kContent:
      frame_.content[idx_++] = b;
      if (idx_ == frame_.contentLen) {
        idx_ = 0;
        state_ = State::kChecksum;
      }
      return Result::kNeedMore;

    case State::kChecksum:
      sumRx_ = static_cast<uint16_t>((sumRx_ << 8) | b);
      if (++idx_ == 2) {
        const uint16_t expected = checksum(frame_.pid, frame_.content, frame_.contentLen);
        const bool ok = (expected == sumRx_);
        restart();
        return ok ? Result::kComplete : Result::kBadChecksum;
      }
      return Result::kNeedMore;
  }
  restart();
  return Result::kNeedMore;
}

bool parseAck(const Frame& f, Ack& out) {
  if (f.pid != PID_ACK || f.contentLen < 1) return false;
  out.code = f.content[0];
  out.params = f.content + 1;
  out.paramLen = f.contentLen - 1;
  return true;
}

uint16_t SysParams::packetBytes() const {
  switch (packetSizeCode) {
    case 0: return 32;
    case 1: return 64;
    case 2: return 128;
    case 3: return 256;
    default: return 0;
  }
}

bool parseSysParams(const Ack& a, SysParams& out) {
  if (a.code != cc::OK || a.paramLen < 16) return false;
  const uint8_t* p = a.params;
  out.statusRegister = get16(p + 0);
  out.systemId = get16(p + 2);
  out.librarySize = get16(p + 4);
  out.securityLevel = get16(p + 6);
  out.address = get32(p + 8);
  out.packetSizeCode = get16(p + 12);
  out.baudMultiplier = get16(p + 14);
  return true;
}

bool parseSearch(const Ack& a, SearchResult& out) {
  if (a.code != cc::OK || a.paramLen < 4) return false;
  out.pageId = get16(a.params);
  out.score = get16(a.params + 2);
  return true;
}

bool parseTemplateCount(const Ack& a, uint16_t& count) {
  if (a.code != cc::OK || a.paramLen < 2) return false;
  count = get16(a.params);
  return true;
}

bool parseMatchScore(const Ack& a, uint16_t& score) {
  if (a.code != cc::OK || a.paramLen < 2) return false;
  score = get16(a.params);
  return true;
}

size_t parseIndexTable(const Ack& a, uint8_t indexPage, uint16_t* ids, size_t maxIds) {
  if (a.code != cc::OK || a.paramLen < 32) return 0;
  size_t n = 0;
  for (size_t byteIdx = 0; byteIdx < 32; ++byteIdx) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (a.params[byteIdx] & (1u << bit)) {
        if (n < maxIds && ids != nullptr) {
          ids[n] = static_cast<uint16_t>(indexPage * 256u + byteIdx * 8u + bit);
        }
        ++n;
      }
    }
  }
  return n < maxIds ? n : maxIds;
}

const char* codeText(uint8_t code) {
  switch (code) {
    case cc::OK: return "OK";
    case cc::PACKET_RECV_ERR: return "error al recibir el paquete";
    case cc::NO_FINGER: return "no hay dedo sobre el sensor";
    case cc::IMAGE_FAIL: return "no se pudo capturar la imagen";
    case cc::IMAGE_MESSY: return "imagen demasiado desordenada";
    case cc::FEATURE_FAIL: return "imagen con pocos puntos caracteristicos";
    case cc::NO_MATCH: return "las huellas no coinciden";
    case cc::NOT_FOUND: return "huella no encontrada en la base";
    case cc::ENROLL_MISMATCH: return "no se pudieron combinar las dos lecturas";
    case cc::BAD_LOCATION: return "posicion (ID) fuera de rango";
    case cc::DB_READ_FAIL: return "error al leer la plantilla (o posicion vacia)";
    case cc::UPLOAD_FEATURE_FAIL: return "error al subir caracteristicas";
    case cc::PACKET_RESPONSE_FAIL: return "el modulo no puede recibir paquetes";
    case cc::UPLOAD_FAIL: return "error al subir imagen";
    case cc::DELETE_FAIL: return "error al borrar la plantilla";
    case cc::DB_CLEAR_FAIL: return "error al vaciar la base";
    case cc::PASS_FAIL: return "contrasena incorrecta";
    case cc::INVALID_IMAGE: return "imagen invalida en el buffer";
    case cc::FLASH_ERR: return "error de escritura en la flash del lector";
    case cc::INVALID_REG: return "numero de registro invalido";
    default: return "codigo desconocido";
  }
}

}  // namespace fp
