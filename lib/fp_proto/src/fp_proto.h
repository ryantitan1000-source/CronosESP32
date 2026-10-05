// Protocolo UART de la familia de lectores ópticos "EF01" (R30x / AS608 / ZFM / DY50).
// Código puro C++: no depende de Arduino, se puede probar en la placa o en una PC.
//
// Formato de trama (todos los campos multi-byte en big-endian):
//   EF 01 | dirección (4) | PID (1) | largo (2) | contenido (largo-2) | checksum (2)
//   - largo   = bytes de contenido + 2 bytes de checksum
//   - checksum = suma de PID + 2 bytes de largo + bytes de contenido (16 bits bajos)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fp {

constexpr uint8_t kHeader0 = 0xEF;
constexpr uint8_t kHeader1 = 0x01;
constexpr uint32_t kDefaultAddress = 0xFFFFFFFFu;
constexpr uint32_t kDefaultPassword = 0x00000000u;

// Identificador de paquete (PID)
constexpr uint8_t PID_COMMAND = 0x01;
constexpr uint8_t PID_DATA = 0x02;
constexpr uint8_t PID_ACK = 0x07;
constexpr uint8_t PID_END_DATA = 0x08;

// Códigos de instrucción
namespace ins {
constexpr uint8_t GEN_IMG = 0x01;
constexpr uint8_t IMG2TZ = 0x02;
constexpr uint8_t MATCH = 0x03;
constexpr uint8_t SEARCH = 0x04;
constexpr uint8_t REG_MODEL = 0x05;
constexpr uint8_t STORE = 0x06;
constexpr uint8_t LOAD_CHAR = 0x07;
constexpr uint8_t DELETE_CHAR = 0x0C;
constexpr uint8_t EMPTY = 0x0D;
constexpr uint8_t READ_SYS_PARA = 0x0F;
constexpr uint8_t VFY_PWD = 0x13;
constexpr uint8_t TEMPLATE_NUM = 0x1D;
constexpr uint8_t READ_INDEX_TABLE = 0x1F;
}  // namespace ins

// Códigos de confirmación (primer byte del contenido de una respuesta)
namespace cc {
constexpr uint8_t OK = 0x00;
constexpr uint8_t PACKET_RECV_ERR = 0x01;
constexpr uint8_t NO_FINGER = 0x02;
constexpr uint8_t IMAGE_FAIL = 0x03;
constexpr uint8_t IMAGE_MESSY = 0x06;
constexpr uint8_t FEATURE_FAIL = 0x07;
constexpr uint8_t NO_MATCH = 0x08;
constexpr uint8_t NOT_FOUND = 0x09;
constexpr uint8_t ENROLL_MISMATCH = 0x0A;
constexpr uint8_t BAD_LOCATION = 0x0B;
constexpr uint8_t DB_READ_FAIL = 0x0C;
constexpr uint8_t UPLOAD_FEATURE_FAIL = 0x0D;
constexpr uint8_t PACKET_RESPONSE_FAIL = 0x0E;
constexpr uint8_t UPLOAD_FAIL = 0x0F;
constexpr uint8_t DELETE_FAIL = 0x10;
constexpr uint8_t DB_CLEAR_FAIL = 0x11;
constexpr uint8_t PASS_FAIL = 0x13;
constexpr uint8_t INVALID_IMAGE = 0x15;
constexpr uint8_t FLASH_ERR = 0x18;
constexpr uint8_t INVALID_REG = 0x1A;
}  // namespace cc

// Bytes de contenido más largos que aceptamos en una respuesta.
// ReadIndexTable es la más larga que usamos: 1 + 32 bytes.
constexpr size_t kMaxContent = 64;
constexpr size_t kFrameOverhead = 2 + 4 + 1 + 2 + 2;  // header+addr+pid+len+sum
constexpr size_t kMaxFrame = kFrameOverhead + kMaxContent;

// Contenido de un comando: instrucción + parámetros.
struct Command {
  uint8_t data[12];
  uint8_t len;
};

Command genImg();
Command img2Tz(uint8_t bufferId);
Command match();
Command search(uint8_t bufferId, uint16_t startPage, uint16_t pageCount);
Command regModel();
Command store(uint8_t bufferId, uint16_t pageId);
Command loadChar(uint8_t bufferId, uint16_t pageId);
Command deleteChar(uint16_t pageId, uint16_t count);
Command empty();
Command readSysPara();
Command verifyPassword(uint32_t password);
Command templateNum();
Command readIndexTable(uint8_t indexPage);

uint16_t checksum(uint8_t pid, const uint8_t* content, size_t contentLen);

// Arma una trama completa en 'out'. Devuelve el largo total, o 0 si no entra.
size_t buildFrame(uint32_t address, uint8_t pid, const uint8_t* content,
                  size_t contentLen, uint8_t* out, size_t outCap);
size_t buildCommandFrame(uint32_t address, const Command& cmd, uint8_t* out,
                         size_t outCap);

struct Frame {
  uint32_t address;
  uint8_t pid;
  uint16_t lengthField;  // valor del campo largo (contenido + 2)
  uint8_t content[kMaxContent];
  size_t contentLen;
};

// Analizador incremental: se le pasa byte a byte lo que llega por la UART.
// Ignora basura antes del encabezado EF 01 (p. ej. el log de arranque del ESP32
// o un byte 0x55 que algunos módulos envían al encenderse).
class FrameParser {
 public:
  enum class Result { kNeedMore, kComplete, kBadChecksum, kBadLength, kTooLong };

  FrameParser() { reset(); }
  void reset();
  Result feed(uint8_t b);
  const Frame& frame() const { return frame_; }
  // Bytes descartados por no pertenecer a una trama válida.
  uint32_t discarded() const { return discarded_; }

 private:
  enum class State { kHeader0, kHeader1, kAddress, kPid, kLength, kContent, kChecksum };
  void restart();

  State state_;
  Frame frame_;
  uint8_t idx_;
  uint16_t sumRx_;
  uint32_t discarded_;
};

// Respuesta (ACK) ya validada: código de confirmación + parámetros.
struct Ack {
  uint8_t code;
  const uint8_t* params;
  size_t paramLen;
};

bool parseAck(const Frame& f, Ack& out);

struct SysParams {
  uint16_t statusRegister;
  uint16_t systemId;
  uint16_t librarySize;
  uint16_t securityLevel;
  uint32_t address;
  uint16_t packetSizeCode;  // 0=32, 1=64, 2=128, 3=256 bytes
  uint16_t baudMultiplier;  // baudios = N x 9600
  uint32_t baudRate() const { return static_cast<uint32_t>(baudMultiplier) * 9600u; }
  uint16_t packetBytes() const;
};

bool parseSysParams(const Ack& a, SysParams& out);

struct SearchResult {
  uint16_t pageId;
  uint16_t score;
};

bool parseSearch(const Ack& a, SearchResult& out);
bool parseTemplateCount(const Ack& a, uint16_t& count);
bool parseMatchScore(const Ack& a, uint16_t& score);

// Tabla de índices: 32 bytes = 256 posiciones por página de índice.
// Devuelve cuántos IDs ocupados escribió en 'ids' (hasta maxIds).
size_t parseIndexTable(const Ack& a, uint8_t indexPage, uint16_t* ids, size_t maxIds);

// Descripción en español de un código de confirmación.
const char* codeText(uint8_t code);

}  // namespace fp
