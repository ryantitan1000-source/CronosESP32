// Consola de comandos por el monitor serie (USB, 115200, fin de línea LF o CR).
#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "relay_hw.h"

using Status = FingerPort::Status;
using Link = Dy50Sensor::Link;

namespace {

char lineBuf[160];
size_t lineLen = 0;
bool lineOverflow = false;

enum class Wait { kOk, kTimeout, kCancelled, kCommError };

// Cualquier tecla (salvo Enter/espacio) cancela una operación en curso.
bool cancelRequested() {
  bool cancel = false;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c != '\r' && c != '\n' && c != ' ') cancel = true;
  }
  return cancel;
}

void printWait(Wait w) {
  switch (w) {
    case Wait::kOk: break;
    case Wait::kTimeout: Serial.println("  Tiempo agotado."); break;
    case Wait::kCancelled: Serial.println("  Cancelado por el usuario."); break;
    case Wait::kCommError:
      Serial.println("  El lector dejo de responder (revisa cables / alimentacion).");
      break;
  }
}

Wait waitFinger(uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint32_t lastNotice = start;
  uint8_t commErrors = 0;
  for (;;) {
    if (millis() - start >= timeoutMs) break;
    feedLoopWDT();
    if (cancelRequested()) return Wait::kCancelled;
    const Status s = sensor.capture();
    if (s == Status::kOk) return Wait::kOk;
    if (s == Status::kCommError) {
      if (++commErrors >= 3) return Wait::kCommError;
    } else {
      commErrors = 0;
    }
    const uint32_t elapsed = millis() - start;
    if (millis() - lastNotice >= 5000 && elapsed < timeoutMs) {
      lastNotice = millis();
      Serial.printf("  ...esperando dedo (quedan %lu s; cualquier tecla + Enter cancela)\n",
                    static_cast<unsigned long>((timeoutMs - elapsed) / 1000));
    }
    delay(60);
  }
  return Wait::kTimeout;
}

Wait waitRelease(uint32_t timeoutMs) {
  const uint32_t start = millis();
  uint8_t clear = 0;
  uint8_t commErrors = 0;
  while (millis() - start < timeoutMs) {
    feedLoopWDT();
    if (cancelRequested()) return Wait::kCancelled;
    const Status s = sensor.capture();
    if (s == Status::kNoFinger) {
      if (++clear >= ACCESS_RELEASE_POLLS) return Wait::kOk;
    } else {
      clear = 0;
    }
    if (s == Status::kCommError) {
      if (++commErrors >= 3) return Wait::kCommError;
    } else {
      commErrors = 0;
    }
    delay(60);
  }
  return Wait::kTimeout;
}

bool requireReader() {
  if (readerOnline) return true;
  Serial.println("El lector no esta conectado. Usa 'conectar' o 'baudios' primero.");
  return false;
}

uint16_t capacity() {
  return readerParamsValid ? readerParams.librarySize : 0;
}

bool parseUInt(const char* s, uint32_t& out) {
  if (!s || !*s) return false;
  if (!isdigit(static_cast<unsigned char>(*s))) return false;  // rechaza '-' y '+'
  char* end = nullptr;
  const unsigned long v = strtoul(s, &end, 10);
  if (*end != '\0' || v > 0xFFFFUL) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

void printCode(const char* what) {
  Serial.printf("  %s: codigo 0x%02X = %s\n", what, sensor.lastCode(), fp::codeText(sensor.lastCode()));
}

// Ejecuta un comando y muestra el resultado; devuelve true si el código fue OK.
bool runAndReport(const char* what, const fp::Command& cmd, uint32_t timeoutMs) {
  const Link l = sensor.run(cmd, timeoutMs);
  if (l != Link::kOk) {
    Serial.printf("  %s: ERROR de comunicacion (%s)\n", what, Dy50Sensor::linkText(l));
    return false;
  }
  printCode(what);
  return sensor.lastCode() == fp::cc::OK;
}

// ------------------------------------------------------------------ comandos

// Una huella nueva o borrada nunca hereda el bloqueo de un ID anterior.
void unblockSlot(uint16_t id) {
  if (blockList.isBlocked(id)) {
    blockList.set(id, false);
    blockListSave();
  }
}

void cmdInfo() {
  if (!requireReader()) return;
  fp::SysParams p{};
  const Link l = sensor.readSysParams(p);
  if (l != Link::kOk || sensor.lastCode() != fp::cc::OK) {
    Serial.printf("ReadSysPara fallo: %s / %s\n", Dy50Sensor::linkText(l), fp::codeText(sensor.lastCode()));
    return;
  }
  readerParams = p;
  readerParamsValid = true;
  Serial.println("=== Parametros del lector (ReadSysPara 0x0F) ===");
  Serial.printf("  Registro de estado : 0x%04X (ocupado=%u, coincidencia=%u, clave_ok=%u, imagen_valida=%u)\n",
                p.statusRegister, p.statusRegister & 1, (p.statusRegister >> 1) & 1,
                (p.statusRegister >> 2) & 1, (p.statusRegister >> 3) & 1);
  Serial.printf("  Codigo de sistema  : 0x%04X\n", p.systemId);
  Serial.printf("  Capacidad          : %u huellas (IDs 0 a %u)\n", p.librarySize,
                p.librarySize ? p.librarySize - 1 : 0);
  Serial.printf("  Nivel de seguridad : %u (1 = permisivo ... 5 = estricto)\n", p.securityLevel);
  Serial.printf("  Direccion          : 0x%08lX\n", static_cast<unsigned long>(p.address));
  Serial.printf("  Tamano de paquete  : codigo %u = %u bytes\n", p.packetSizeCode, p.packetBytes());
  Serial.printf("  Velocidad          : N=%u -> %lu baudios (configurada en el ESP32: %lu)\n",
                p.baudMultiplier, static_cast<unsigned long>(p.baudRate()),
                static_cast<unsigned long>(sensor.baud()));
  uint16_t count = 0;
  if (sensor.run(fp::templateNum()) == Link::kOk && fp::parseTemplateCount(sensor.lastAck(), count)) {
    Serial.printf("  Huellas guardadas  : %u (TemplateNum 0x1D)\n", count);
  } else {
    Serial.println("  Huellas guardadas  : no se pudo leer (TemplateNum)");
  }
}

void cmdBaud() {
  Serial.println("Buscando la velocidad del lector (puede tardar ~15 s)...");
  const uint32_t found = sensor.findBaud(&Serial);
  if (!found) {
    readerOnline = false;
    Serial.println("El lector NO respondio a ninguna velocidad.");
    Serial.println("Causas probables: TX/RX invertidos o sin contacto, GND sin unir, sin 3.3 V.");
    return;
  }
  if (found != settings.fpBaud) {
    settings.fpBaud = found;
    settingsSave();
    Serial.printf("Velocidad guardada: %lu baudios.\n", static_cast<unsigned long>(found));
  }
  readerConnect(true);
}

void cmdFinger(uint32_t seconds) {
  if (!requireReader()) return;
  Serial.printf("Deteccion de dedo durante %lu s (GenImg cada ~100 ms). Apoya y retira el dedo.\n",
                static_cast<unsigned long>(seconds));
  Serial.println("Cualquier tecla + Enter termina antes.");
  const uint32_t start = millis();
  int last = -1;
  uint32_t polls = 0, present = 0, errors = 0;
  while (millis() - start < seconds * 1000UL) {
    feedLoopWDT();
    if (cancelRequested()) break;
    const Status s = sensor.capture();
    ++polls;
    int now;
    switch (s) {
      case Status::kOk: now = 1; ++present; break;
      case Status::kNoFinger: now = 0; break;
      case Status::kImageError: now = 2; ++present; break;
      case Status::kCommError: now = 3; ++errors; break;
      default: now = 4; break;
    }
    if (now != last) {
      const float t = (millis() - start) / 1000.0f;
      switch (now) {
        case 0: Serial.printf("  [%5.1f s] sin dedo\n", t); break;
        case 1: Serial.printf("  [%5.1f s] DEDO APOYADO (imagen capturada)\n", t); break;
        case 2: Serial.printf("  [%5.1f s] dedo detectado pero imagen fallida\n", t); break;
        case 3: Serial.printf("  [%5.1f s] ERROR de comunicacion\n", t); break;
        default: Serial.printf("  [%5.1f s] codigo 0x%02X: %s\n", t, sensor.lastCode(), fp::codeText(sensor.lastCode())); break;
      }
      last = now;
    }
    delay(40);
  }
  Serial.printf("Fin. Lecturas: %lu, con dedo: %lu, errores de comunicacion: %lu\n",
                static_cast<unsigned long>(polls), static_cast<unsigned long>(present),
                static_cast<unsigned long>(errors));
}

bool captureTemplate(uint8_t bufferId, const char* prompt) {
  for (int attempt = 1; attempt <= 3; ++attempt) {
    Serial.println(prompt);
    const Wait w = waitFinger(30000);
    if (w != Wait::kOk) {
      printWait(w);
      return false;
    }
    const Status s = sensor.extract(bufferId);
    if (s == Status::kOk) {
      Serial.println("  Imagen capturada y procesada.");
      return true;
    }
    if (s == Status::kCommError) {
      printWait(Wait::kCommError);
      return false;
    }
    Serial.printf("  Imagen no util (%s). Retira el dedo y proba de nuevo (intento %d/3).\n",
                  fp::codeText(sensor.lastCode()), attempt);
    const Wait r = waitRelease(15000);
    if (r != Wait::kOk) {
      printWait(r);
      return false;
    }
  }
  return false;
}

bool slotUsed(uint16_t id, bool& used) {
  static uint16_t ids[1024];
  size_t n = 0;
  if (sensor.usedIds(ids, 1024, n) != Link::kOk || sensor.lastCode() != fp::cc::OK) return false;
  used = false;
  for (size_t i = 0; i < n; ++i) {
    if (ids[i] == id) used = true;
  }
  return true;
}

void cmdEnroll(const char* arg) {
  if (!requireReader()) return;
  uint32_t id = 0;
  if (!parseUInt(arg, id)) {
    Serial.println("Uso: enrolar <id>   (ejemplo: enrolar 1)");
    return;
  }
  const uint16_t cap = capacity();
  if (cap == 0) {
    Serial.println("Capacidad del lector desconocida: ejecuta 'info' primero.");
    return;
  }
  if (id >= cap) {
    Serial.printf("ID fuera de rango: debe ser de 0 a %u.\n", cap - 1);
    return;
  }
  bool used = false;
  if (!slotUsed(static_cast<uint16_t>(id), used)) {
    Serial.println("No se pudo verificar si la posicion esta libre: registro cancelado.");
    return;
  }
  if (used) {
    Serial.printf("La posicion %lu ya tiene una huella. Borrala antes con: borrar %lu\n",
                  static_cast<unsigned long>(id), static_cast<unsigned long>(id));
    return;
  }

  Serial.printf("=== Registro de huella en ID %lu === (cualquier tecla + Enter cancela)\n",
                static_cast<unsigned long>(id));
  if (!captureTemplate(1, "Paso 1/2: APOYA el dedo sobre el lector y mantenelo quieto.")) return;

  // ¿Ya está registrada esta huella en otra posición?
  uint16_t dupId = 0, dupScore = 0;
  const Status dup = sensor.searchAll(dupId, dupScore);
  if (dup == Status::kOk) {
    Serial.printf("Esa huella YA esta registrada en ID %u (puntaje %u). Registro cancelado.\n", dupId, dupScore);
    return;
  }
  if (dup == Status::kCommError) {
    printWait(Wait::kCommError);
    return;
  }

  Serial.println("RETIRA el dedo.");
  Wait w = waitRelease(20000);
  if (w != Wait::kOk) {
    printWait(w);
    return;
  }
  if (!captureTemplate(2, "Paso 2/2: APOYA el MISMO dedo otra vez.")) return;

  if (!runAndReport("Combinar lecturas (RegModel)", fp::regModel(), Dy50Sensor::kTimeoutDefault)) {
    Serial.println("Las dos lecturas no coinciden lo suficiente. Repeti 'enrolar' apoyando igual las dos veces.");
    return;
  }
  if (!runAndReport("Guardar (Store)", fp::store(1, static_cast<uint16_t>(id)), Dy50Sensor::kTimeoutFlash)) {
    Serial.println("No se pudo guardar la huella.");
    return;
  }
  unblockSlot(static_cast<uint16_t>(id));
  Serial.printf("HUELLA GUARDADA en ID %lu. Retira el dedo.\n", static_cast<unsigned long>(id));
  waitRelease(10000);
}

void cmdIdentify() {
  if (!requireReader()) return;
  Serial.println("Identificar (NO activa el rele). APOYA el dedo...");
  Wait w = waitFinger(20000);
  if (w != Wait::kOk) {
    printWait(w);
    return;
  }
  const Status e = sensor.extract(1);
  if (e != Status::kOk) {
    Serial.printf("  Imagen no util: %s\n", e == Status::kCommError ? "error de comunicacion" : fp::codeText(sensor.lastCode()));
    return;
  }
  uint16_t id = 0, score = 0;
  const Status s = sensor.searchAll(id, score);
  if (s == Status::kOk) {
    const char* nota = "";
    if (blockList.isBlocked(id)) {
      nota = "  (BLOQUEADA: el modo acceso NO abriria)";
    } else if (score < settings.minScore) {
      nota = "  (por debajo del minimo: el modo acceso la RECHAZARIA)";
    }
    Serial.printf("  COINCIDE con ID %u, puntaje %u%s\n", id, score, nota);
  } else if (s == Status::kNotFound) {
    Serial.println("  NO REGISTRADA: no coincide con ninguna huella guardada.");
  } else if (s == Status::kCommError) {
    printWait(Wait::kCommError);
  } else {
    printCode("Busqueda");
  }
  Serial.println("  Retira el dedo.");
  waitRelease(10000);
}

void cmdList() {
  if (!requireReader()) return;
  static uint16_t ids[1024];
  size_t n = 0;
  const Link l = sensor.usedIds(ids, 1024, n);
  if (l != Link::kOk || sensor.lastCode() != fp::cc::OK) {
    Serial.printf("No se pudo leer la tabla de indices: %s / %s\n", Dy50Sensor::linkText(l),
                  fp::codeText(sensor.lastCode()));
    return;
  }
  Serial.printf("Posiciones ocupadas (%u):", static_cast<unsigned>(n));
  if (n == 0) Serial.print(" ninguna");
  for (size_t i = 0; i < n; ++i) Serial.printf(" %u", ids[i]);
  Serial.println();
  uint16_t count = 0;
  if (sensor.run(fp::templateNum()) == Link::kOk && fp::parseTemplateCount(sensor.lastAck(), count)) {
    Serial.printf("TemplateNum informa %u huella(s)%s\n", count,
                  count == n ? " (coincide)" : "  <-- NO coincide con la tabla");
  }
}

void cmdDelete(const char* arg) {
  if (!requireReader()) return;
  uint32_t id = 0;
  if (!parseUInt(arg, id)) {
    Serial.println("Uso: borrar <id>");
    return;
  }
  const uint16_t cap = capacity();
  if (cap == 0) {
    Serial.println("Capacidad del lector desconocida: ejecuta 'info' primero.");
    return;
  }
  if (id >= cap) {
    Serial.printf("ID fuera de rango: debe ser de 0 a %u.\n", cap - 1);
    return;
  }
  bool used = false;
  if (slotUsed(static_cast<uint16_t>(id), used) && !used) {
    Serial.printf("Aviso: la posicion %lu ya estaba vacia.\n", static_cast<unsigned long>(id));
  }
  if (runAndReport("Borrar (DeleteChar)", fp::deleteChar(static_cast<uint16_t>(id), 1), Dy50Sensor::kTimeoutFlash)) {
    unblockSlot(static_cast<uint16_t>(id));
    Serial.printf("Huella ID %lu eliminada.\n", static_cast<unsigned long>(id));
  }
}

void cmdDeleteAll(const char* arg) {
  if (!requireReader()) return;
  if (!arg || strcmp(arg, "si") != 0) {
    Serial.println("Esto borra TODAS las huellas. Para confirmar escribi: borrartodo SI");
    return;
  }
  if (runAndReport("Vaciar base (Empty)", fp::empty(), Dy50Sensor::kTimeoutFlash)) {
    if (blockList.count()) {
      blockList.clearAll();
      blockListSave();
    }
    Serial.println("Base de huellas vacia.");
  }
}

// Bloquear: la huella sigue guardada pero no abre. Permitir: vuelve a abrir.
void cmdBlock(const char* arg, bool block) {
  uint32_t id = 0;
  if (!parseUInt(arg, id) || id >= BlockList::kMaxIds) {
    Serial.printf("Uso: %s <id>\n", block ? "bloquear" : "permitir");
    return;
  }
  const uint16_t cap = capacity();
  if (cap != 0 && id >= cap) {
    Serial.printf("ID fuera de rango: debe ser de 0 a %u.\n", cap - 1);
    return;
  }
  blockList.set(static_cast<uint16_t>(id), block);
  blockListSave();
  if (block) {
    Serial.printf("ID %lu BLOQUEADO: su huella sigue guardada pero NO abre el molinete (guardado).\n",
                  static_cast<unsigned long>(id));
  } else {
    Serial.printf("ID %lu PERMITIDO: su huella vuelve a abrir el molinete (guardado).\n",
                  static_cast<unsigned long>(id));
  }
}

void cmdBlocked() {
  static uint16_t ids[BlockList::kMaxIds];
  const size_t n = blockList.list(ids, BlockList::kMaxIds);
  Serial.printf("Bloqueados (%u):", static_cast<unsigned>(n));
  if (n == 0) Serial.print(" ninguno");
  for (size_t i = 0; i < n; ++i) Serial.printf(" %u", ids[i]);
  Serial.println();
}

void cmdAccess(const char* arg) {
  if (!arg) {
    Serial.printf("Modo acceso: %s. Uso: acceso on | acceso off\n", settings.accessOn ? "ACTIVADO" : "DESACTIVADO");
    return;
  }
  if (strcmp(arg, "on") == 0 || strcmp(arg, "si") == 0) {
    settings.accessOn = true;
    accessCtl.clearFailures();
  } else if (strcmp(arg, "off") == 0 || strcmp(arg, "no") == 0) {
    settings.accessOn = false;
  } else {
    Serial.println("Uso: acceso on | acceso off");
    return;
  }
  settingsSave();
  accessCtl.requireRelease();
  Serial.printf("Modo acceso %s (guardado; se mantiene tras reiniciar).\n",
                settings.accessOn ? "ACTIVADO: una huella autorizada genera un pulso" : "DESACTIVADO (mantenimiento)");
}

void cmdPulse(const char* arg) {
  uint32_t ms = 0;
  if (!parseUInt(arg, ms)) {
    Serial.printf("Pulso actual: %lu ms. Uso: pulso <ms> (%d a %d)\n",
                  static_cast<unsigned long>(settings.pulseMs), RELAY_PULSE_MIN_MS, RELAY_PULSE_MAX_MS);
    return;
  }
  settings.pulseMs = relayLogic.setPulseMs(ms);
  settingsSave();
  Serial.printf("Pulso de apertura: %lu ms (guardado)%s\n", static_cast<unsigned long>(settings.pulseMs),
                settings.pulseMs != ms ? "  <-- ajustado al rango permitido" : "");
}

void cmdScore(const char* arg) {
  uint32_t v = 0;
  if (!parseUInt(arg, v) || v > 1000) {
    Serial.printf("Puntaje minimo actual: %u. Uso: puntaje <0-1000> (0 = criterio del lector)\n", settings.minScore);
    return;
  }
  settings.minScore = static_cast<uint16_t>(v);
  accessCtl.config().minScore = settings.minScore;
  settingsSave();
  Serial.printf("Puntaje minimo: %u (guardado)\n", settings.minScore);
}

void cmdGpio4(const char* arg) {
#if RELAY_OUTPUT_ENABLED
  (void)arg;
  Serial.println("Deshabilitado: la salida real del rele esta activa. Usa 'rele probar'.");
#else
  if (arg && strcmp(arg, "alto") == 0) {
    relayhw::diagSet(1, GPIO_DIAG_AUTO_REST_MS);
    Serial.printf("GPIO%d = ALTO. Entre el pin %d y G deberias medir ~3.3 V (3.1 a 3.4 V).\n", PIN_RELAY, PIN_RELAY);
    Serial.printf("Vuelve solo a 0 V en %d s. (IN del rele debe estar DESCONECTADO)\n", GPIO_DIAG_AUTO_REST_MS / 1000);
  } else if (arg && (strcmp(arg, "bajo") == 0 || strcmp(arg, "reposo") == 0)) {
    relayhw::diagSet(relayhw::restLevel(), 0);
    Serial.printf("GPIO%d = BAJO (reposo). Entre el pin %d y G deberias medir ~0 V (menos de 0.1 V).\n", PIN_RELAY, PIN_RELAY);
  } else if (arg && strcmp(arg, "estado") == 0) {
    // sin cambios, solo informar
  } else {
    Serial.println("Uso: gpio4 alto | gpio4 bajo | gpio4 estado");
  }
  Serial.printf("GPIO%d: ordenado=%d, leido en el pad=%d%s\n", PIN_RELAY, relayhw::commandedLevel(),
                relayhw::padLevel(),
                relayhw::commandedLevel() != relayhw::padLevel() ? "  <-- DIFIERE: posible corto o carga" : "");
#endif
}

void cmdRelay(const char* arg) {
  if (arg && strcmp(arg, "probar") == 0) {
    requestOpen("prueba manual");
    return;
  }
  Serial.println("Uso: rele probar   (pulso de prueba con la duracion configurada)");
}

void cmdTrace(const char* arg) {
  if (arg && strcmp(arg, "on") == 0) {
    sensor.setTrace(&Serial);
    Serial.println("Traza UART ACTIVADA: se muestran las tramas TX/RX en hexadecimal.");
  } else if (arg && strcmp(arg, "off") == 0) {
    sensor.setTrace(nullptr);
    Serial.println("Traza UART desactivada.");
  } else {
    Serial.println("Uso: traza on | traza off");
  }
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(tolower(c));
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

void cmdRaw(const char* rest) {
  uint8_t tx[64];
  size_t n = 0;
  int hi = -1;
  for (const char* p = rest; p && *p; ++p) {
    if (*p == ' ') continue;
    const int v = hexVal(*p);
    if (v < 0) {
      Serial.println("Caracter no hexadecimal. Ejemplo: crudo EF01FFFFFFFF0100071300000000001B");
      return;
    }
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= sizeof tx) {
        Serial.println("Maximo 64 bytes: no se envio nada.");
        return;
      }
      tx[n++] = static_cast<uint8_t>((hi << 4) | v);
      hi = -1;
    }
  }
  if (n == 0 || hi >= 0) {
    Serial.println("Uso: crudo <bytes en hex, cantidad par de digitos>");
    return;
  }
  uint8_t rx[128];
  const size_t got = sensor.rawExchange(tx, n, rx, sizeof rx, 1000);
  Serial.printf("TX %u B:", static_cast<unsigned>(n));
  for (size_t i = 0; i < n; ++i) Serial.printf(" %02X", tx[i]);
  Serial.printf("\nRX %u B:", static_cast<unsigned>(got));
  for (size_t i = 0; i < got; ++i) Serial.printf(" %02X", rx[i]);
  Serial.println(got ? "" : " (nada)");
}

void execute(char* line) {
  // Copia original (para 'crudo') y versión en minúsculas para comparar.
  char original[sizeof lineBuf];
  strncpy(original, line, sizeof original);
  original[sizeof original - 1] = '\0';
  for (char* p = line; *p; ++p) *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));

  char* save = nullptr;
  char* cmd = strtok_r(line, " \t", &save);
  if (!cmd) return;
  char* a1 = strtok_r(nullptr, " \t", &save);

  Serial.printf("> %s\n", original);

  if (!strcmp(cmd, "ayuda") || !strcmp(cmd, "help") || !strcmp(cmd, "?")) printHelp();
  else if (!strcmp(cmd, "estado")) printStatus();
  else if (!strcmp(cmd, "info")) cmdInfo();
  else if (!strcmp(cmd, "conectar")) readerConnect(true);
  else if (!strcmp(cmd, "baudios")) cmdBaud();
  else if (!strcmp(cmd, "dedo")) {
    uint32_t s = 15;
    if (a1 && (!parseUInt(a1, s) || s == 0 || s > 120)) s = 15;
    cmdFinger(s);
  }
  else if (!strcmp(cmd, "enrolar")) cmdEnroll(a1);
  else if (!strcmp(cmd, "identificar")) cmdIdentify();
  else if (!strcmp(cmd, "lista")) cmdList();
  else if (!strcmp(cmd, "borrar")) cmdDelete(a1);
  else if (!strcmp(cmd, "borrartodo")) cmdDeleteAll(a1);
  else if (!strcmp(cmd, "bloquear")) cmdBlock(a1, true);
  else if (!strcmp(cmd, "permitir")) cmdBlock(a1, false);
  else if (!strcmp(cmd, "bloqueados")) cmdBlocked();
  else if (!strcmp(cmd, "acceso")) cmdAccess(a1);
  else if (!strcmp(cmd, "pulso")) cmdPulse(a1);
  else if (!strcmp(cmd, "puntaje")) cmdScore(a1);
  else if (!strcmp(cmd, "gpio4")) cmdGpio4(a1);
  else if (!strcmp(cmd, "rele")) cmdRelay(a1);
  else if (!strcmp(cmd, "traza")) cmdTrace(a1);
  else if (!strcmp(cmd, "crudo")) {
    const char* p = original;
    while (*p == ' ' || *p == '\t') ++p;
    cmdRaw(p + 5);  // salta la palabra "crudo"
  }
  else if (!strcmp(cmd, "reiniciar")) {
    Serial.println("Reiniciando...");
    relayhw::off();
    Serial.flush();
    delay(100);
    ESP.restart();
  } else {
    Serial.printf("Comando desconocido: '%s'. Escribi 'ayuda'.\n", cmd);
  }

  // Después de un comando que usó el lector, no evaluar un dedo que quedó apoyado.
  accessCtl.requireRelease();
}

}  // namespace

void consolePoll() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineOverflow) {
        Serial.printf("Linea demasiado larga (maximo %u caracteres): no se ejecuto.\n",
                      static_cast<unsigned>(sizeof lineBuf - 1));
      } else if (lineLen) {
        lineBuf[lineLen] = '\0';
        execute(lineBuf);
      }
      lineLen = 0;
      lineOverflow = false;
      continue;
    }
    if (c == 8 || c == 127) {
      if (lineLen) --lineLen;
      continue;
    }
    if (c >= 32 && c < 127) {
      if (lineLen < sizeof lineBuf - 1) {
        lineBuf[lineLen++] = static_cast<char>(c);
      } else {
        lineOverflow = true;
      }
    }
  }
}

void printHelp() {
  Serial.println(
      "=== Comandos (escribir y Enter) ===\n"
      "  ayuda                 esta lista\n"
      "  estado                estado general: lector, rele, modo, contadores\n"
      "  info                  parametros del lector y cantidad de huellas\n"
      "  conectar              reintenta hablar con el lector a la velocidad guardada\n"
      "  baudios               prueba todas las velocidades (9600..115200) y guarda la que responde\n"
      "  dedo [s]              muestra en vivo si hay dedo apoyado (por defecto 15 s)\n"
      "  enrolar <id>          registra una huella nueva en la posicion <id>\n"
      "  identificar           lee un dedo y dice si esta registrado (NO activa el rele)\n"
      "  lista                 posiciones ocupadas\n"
      "  borrar <id>           elimina la huella <id>\n"
      "  borrartodo SI         elimina TODAS las huellas\n"
      "  bloquear <id>         la huella sigue guardada pero NO abre el molinete (se guarda)\n"
      "  permitir <id>         vuelve a permitir que esa huella abra\n"
      "  bloqueados            lista de huellas bloqueadas\n"
      "  acceso on|off         modo acceso automatico (se guarda)\n"
      "  pulso <ms>            duracion del pulso de apertura, 100-10000 ms (se guarda)\n"
      "  puntaje <n>           puntaje minimo de coincidencia, 0 = criterio del lector (se guarda)\n"
      "  gpio4 alto|bajo|estado  prueba del pin con multimetro (solo en simulacion, IN desconectado)\n"
      "  rele probar           pulso de prueba (en simulacion solo informa y prende el LED)\n"
      "  traza on|off          muestra las tramas UART en hexadecimal\n"
      "  crudo <hex>           envia bytes crudos al lector y muestra la respuesta (avanzado)\n"
      "  reiniciar             reinicia el ESP32\n"
      "Durante 'enrolar', 'identificar' o 'dedo': cualquier tecla + Enter cancela.");
}

void printStatus() {
  const uint32_t now = millis();
  const auto& st = sensor.stats();
  Serial.println("=== ESTADO ===");
  Serial.printf("  Firmware %s v%s | encendido hace %lu s | memoria libre %lu B\n", FW_NAME, FW_VERSION,
                static_cast<unsigned long>(now / 1000), static_cast<unsigned long>(ESP.getFreeHeap()));
  printChipLine();
  Serial.printf("  Ultimo reinicio: %s\n", resetReasonText());
  Serial.printf("  Lector: %s a %lu baudios", readerOnline ? "CONECTADO" : "NO RESPONDE",
                static_cast<unsigned long>(sensor.baud()));
  if (readerParamsValid) Serial.printf(", capacidad %u", readerParams.librarySize);
  Serial.println();
  Serial.printf("  Huellas bloqueadas: %u\n", blockList.count());
  if (settings.accessOn) {
    Serial.printf("  Modo acceso: ACTIVADO | estado: %s | fallos seguidos: %u", accessStateName(accessCtl.state()),
                  accessCtl.failCount());
  } else {
    Serial.print("  Modo acceso: DESACTIVADO (mantenimiento: no se evaluan huellas)");
  }
  if (accessCtl.state() == AccessController::State::kLockout) {
    Serial.printf(" | bloqueo restante %lu s", static_cast<unsigned long>(accessCtl.lockoutRemainingMs(now) / 1000));
  }
  Serial.println();
#if RELAY_OUTPUT_ENABLED
  Serial.printf("  Rele: SALIDA REAL en GPIO%d (activo en %s)\n", PIN_RELAY, RELAY_ACTIVE_LEVEL ? "ALTO" : "BAJO");
#else
  Serial.printf("  Rele: SIMULACION (GPIO%d fijo en reposo; se habilita tras verificar la interfaz)\n", PIN_RELAY);
#endif
  Serial.printf("  Pulso: %lu ms | enfriamiento: %lu ms | activo ahora: %s | pulsos dados: %lu\n",
                static_cast<unsigned long>(relayLogic.pulseMs()), static_cast<unsigned long>(relayLogic.cooldownMs()),
                relayhw::isActive() ? "SI" : "no", static_cast<unsigned long>(relayhw::activations()));
  Serial.printf("  GPIO%d: ordenado=%d, leido=%d%s | puntaje minimo: %u\n", PIN_RELAY, relayhw::commandedLevel(),
                relayhw::padLevel(), relayhw::diagActive() ? " (prueba manual 'gpio4 alto' en curso)" : "",
                settings.minScore);
  Serial.printf("  UART lector: tramas TX %lu, RX %lu, timeouts %lu, checksum mal %lu, tramas invalidas %lu, bytes basura %lu\n",
                static_cast<unsigned long>(st.txFrames), static_cast<unsigned long>(st.rxFrames),
                static_cast<unsigned long>(st.timeouts), static_cast<unsigned long>(st.badChecksum),
                static_cast<unsigned long>(st.badFrames), static_cast<unsigned long>(st.garbageBytes));
}
