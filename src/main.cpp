// CronosESP32 - control de acceso con lector de huellas DY50 y ESP32-C3 SuperMini.
// Sin Wi-Fi ni servicios en la nube. Consola de comandos por USB (115200).
#include <Arduino.h>
#include <Preferences.h>
#include <esp_rom_sys.h>
#include <esp_system.h>

#include "app.h"
#include "config.h"
#include "relay_hw.h"

Dy50Sensor sensor(FP_UART, PIN_FP_RX, PIN_FP_TX);
RelayPulse relayLogic(RELAY_PULSE_DEFAULT_MS, RELAY_COOLDOWN_MS, RELAY_PULSE_MIN_MS,
                      RELAY_PULSE_MAX_MS);
AccessController accessCtl(sensor, AccessConfig{});
Settings settings{RELAY_PULSE_DEFAULT_MS, ACCESS_MIN_SCORE_DEFAULT, false, FP_DEFAULT_BAUD};
BlockList blockList;
bool readerOnline = false;
fp::SysParams readerParams{};
bool readerParamsValid = false;

namespace {

Preferences prefs;
uint32_t lastAccessStep = 0;
uint32_t lastHealthPing = 0;
uint32_t lastImageErrorLog = 0;
bool deniedBlinkActive = false;
uint32_t deniedBlinkStart = 0;
bool relayLogicWasActive = false;
uint32_t relayLogicEndMs = 0;
uint8_t healthFailures = 0;

// ------------------------------------------------------------------ ajustes

bool validBaud(uint32_t b) {
  return b >= 9600 && b <= 115200 && (b % 9600) == 0;
}

void settingsLoad() {
  prefs.begin("cronos", false);
  settings.pulseMs = prefs.getUInt("pulse", RELAY_PULSE_DEFAULT_MS);
  settings.minScore = prefs.getUShort("minscore", ACCESS_MIN_SCORE_DEFAULT);
  settings.accessOn = prefs.getBool("acceso", false);
  settings.fpBaud = prefs.getUInt("baud", FP_DEFAULT_BAUD);
  // Lista de huellas bloqueadas. Sin lista guardada (o dañada) no hay ninguna bloqueada.
  blockList.clearAll();
  if (prefs.isKey("bloq")) {
    uint8_t buf[BlockList::kBytes];
    const size_t got = prefs.getBytes("bloq", buf, sizeof buf);
    if (got == sizeof buf) blockList.load(buf, got);
  }
  prefs.end();
  if (!validBaud(settings.fpBaud)) settings.fpBaud = FP_DEFAULT_BAUD;
  settings.pulseMs = relayLogic.setPulseMs(settings.pulseMs);
}

// ------------------------------------------------------------------ LED

void ledWrite(bool on) {
  digitalWrite(PIN_STATUS_LED, (on ^ (STATUS_LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

// Patrones:
//   encendido fijo ........ pulso de apertura en curso (real o simulado)
//   parpadeo rápido ....... denegado (1 s) o lector desconectado
//   1 destello cada 2 s ... modo acceso activo
//   2 destellos cada 2 s .. modo mantenimiento (acceso desactivado)
void ledUpdate(uint32_t now) {
  bool on;
  const uint32_t phase = now % 2000;
  if (deniedBlinkActive && now - deniedBlinkStart >= 1000) deniedBlinkActive = false;
  if (relayhw::isActive()) {
    on = true;
  } else if (deniedBlinkActive && now - deniedBlinkStart < 1000) {
    on = (now / 80) % 2 == 0;
  } else if (!readerOnline) {
    on = (now % 250) < 125;
  } else if (settings.accessOn) {
    on = phase < 60;
  } else {
    on = phase < 60 || (phase >= 220 && phase < 280);
  }
  ledWrite(on);
}

// ------------------------------------------------------------------ eventos

void handleAccessEvent(const AccessEvent& e, uint32_t now) {
  switch (e.type) {
    case AccessEventType::kNone:
      break;
    case AccessEventType::kGranted:
      if (blockList.isBlocked(e.id)) {
        // La huella existe pero su acceso está suspendido (p. ej. membresía vencida).
        Serial.printf("[ACCESO] BLOQUEADO: ID %u (puntaje %u). Acceso suspendido: no se abre.\n", e.id, e.score);
        ledSignalDenied();
        break;
      }
      Serial.printf("[ACCESO] CONCEDIDO: ID %u (puntaje %u)\n", e.id, e.score);
      requestOpen("huella autorizada");
      break;
    case AccessEventType::kDenied:
      if (e.lowScore) {
        Serial.printf("[ACCESO] DENEGADO: ID %u con puntaje %u < minimo %u\n", e.id, e.score,
                      settings.minScore);
      } else {
        Serial.println("[ACCESO] DENEGADO: huella no registrada");
      }
      if (e.lockoutStarted) {
        Serial.printf("[ACCESO] BLOQUEO: %u intentos fallidos seguidos. Pausa de %lu s\n",
                      accessCtl.config().maxFails,
                      static_cast<unsigned long>(accessCtl.config().lockoutMs / 1000));
      }
      ledSignalDenied();
      break;
    case AccessEventType::kImageError:
      if (now - lastImageErrorLog > 2000) {
        lastImageErrorLog = now;
        Serial.printf("[ACCESO] imagen poco clara (%s), reintentando\n",
                      fp::codeText(sensor.lastCode()));
      }
      break;
    case AccessEventType::kFingerReleased:
      Serial.println("[ACCESO] dedo retirado: listo para otra lectura");
      break;
    case AccessEventType::kCommError:
      Serial.println("[LECTOR] error de comunicacion aislado");
      break;
    case AccessEventType::kReaderOffline:
      relayhw::off();
      relayLogic.forceOff(now);
      readerOnline = false;
      Serial.println("[LECTOR] DESCONECTADO: no responde. Rele en reposo. Reintento cada 3 s.");
      break;
    case AccessEventType::kReaderOnline:
      Serial.println("[LECTOR] volvio a responder.");
      readerConnect(true);
      break;
    case AccessEventType::kLockoutEnded:
      Serial.println("[ACCESO] fin del bloqueo. Retire el dedo para continuar.");
      break;
  }
}

void relayHousekeeping(uint32_t now) {
  const bool logicActive = relayLogic.update(now);
  if (relayLogicWasActive && !logicActive) relayLogicEndMs = now;
  relayLogicWasActive = logicActive;
  // Respaldo: el esp_timer apaga el pin a tiempo. Si 20 ms después de que la
  // lógica terminó el pin sigue activo, algo falló: apagarlo y avisar.
  if (!logicActive && relayhw::isActive() && now - relayLogicEndMs > 20) {
    relayhw::off();
    Serial.println("[RELE] aviso: apagado de respaldo desde loop()");
  }
}

void printBanner() {
  Serial.println();
  Serial.println("==================================================");
  Serial.printf(" %s v%s  (compilado %s %s)\n", FW_NAME, FW_VERSION, __DATE__, __TIME__);
  printChipLine();
  Serial.printf(" Motivo del ultimo reinicio: %s\n", resetReasonText());
#if RELAY_OUTPUT_ENABLED
  Serial.printf(" Rele: SALIDA REAL en GPIO%d, activo en %s\n", PIN_RELAY,
                RELAY_ACTIVE_LEVEL ? "ALTO" : "BAJO");
#else
  Serial.printf(" Rele: SIMULACION (GPIO%d fijo en reposo = %s)\n", PIN_RELAY,
                relayhw::restLevel() ? "ALTO" : "BAJO 0 V");
  Serial.println(" (Con transistor NPN, 0 V = rele apagado. Si IN fuera directo o solo con");
  Serial.println("  resistencia a GPIO4, 0 V lo ACTIVARIA: usar siempre el transistor.)");
#endif
  Serial.printf(" Pulso: %lu ms | Puntaje minimo: %u | Acceso automatico: %s\n",
                static_cast<unsigned long>(settings.pulseMs), settings.minScore,
                settings.accessOn ? "SI" : "NO");
  Serial.println("==================================================");
}

}  // namespace

// ------------------------------------------------------------------ API compartida

void printChipLine() {
  const uint64_t mac = ESP.getEfuseMac();
  Serial.printf("  Chip: %s rev %d, %lu MHz, flash %lu MB, MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                static_cast<unsigned long>(ESP.getCpuFreqMHz()),
                static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(mac & 0xFF), static_cast<unsigned>((mac >> 8) & 0xFF),
                static_cast<unsigned>((mac >> 16) & 0xFF), static_cast<unsigned>((mac >> 24) & 0xFF),
                static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 40) & 0xFF));
}

void settingsSave() {
  prefs.begin("cronos", false);
  prefs.putUInt("pulse", settings.pulseMs);
  prefs.putUShort("minscore", settings.minScore);
  prefs.putBool("acceso", settings.accessOn);
  prefs.putUInt("baud", settings.fpBaud);
  prefs.end();
}

void blockListSave() {
  prefs.begin("cronos", false);
  prefs.putBytes("bloq", blockList.data(), BlockList::kBytes);
  prefs.end();
}

void ledSignalDenied() {
  deniedBlinkActive = true;
  deniedBlinkStart = millis();
}

bool readerConnect(bool verbose) {
  if (sensor.baud() != settings.fpBaud) sensor.setBaud(settings.fpBaud);
  bool ok = sensor.ping() || sensor.ping();
  if (!ok) {
    readerOnline = false;
    readerParamsValid = false;
    if (verbose) {
      Serial.printf("[LECTOR] NO responde a %lu baudios.\n",
                    static_cast<unsigned long>(settings.fpBaud));
      Serial.println("         Revisa: alimentacion 3.3 V, GND, TX lector->GPIO20, RX lector->GPIO21.");
      Serial.println("         Para probar otras velocidades escribi: baudios");
    }
    return false;
  }
  const uint8_t pwdCode = sensor.lastCode();
  readerOnline = true;
  if (verbose) {
    Serial.printf("[LECTOR] responde a %lu baudios. Verificacion de contrasena: %s\n",
                  static_cast<unsigned long>(settings.fpBaud), fp::codeText(pwdCode));
  }
  readerParamsValid = (sensor.readSysParams(readerParams) == Dy50Sensor::Link::kOk &&
                       sensor.lastCode() == fp::cc::OK);
  if (verbose) {
    if (readerParamsValid) {
      Serial.printf("[LECTOR] capacidad %u huellas, nivel de seguridad %u, paquete %u B\n",
                    readerParams.librarySize, readerParams.securityLevel,
                    readerParams.packetBytes());
    } else {
      Serial.println("[LECTOR] aviso: no se pudieron leer los parametros (ReadSysPara)");
    }
  }
  return true;
}

void requestOpen(const char* reason) {
  const uint32_t now = millis();
  switch (relayLogic.request(now)) {
    case RelayPulse::Request::kStarted:
      relayhw::activate(relayLogic.pulseMs());
#if RELAY_OUTPUT_ENABLED
      Serial.printf("[RELE] PULSO de %lu ms (%s)\n", static_cast<unsigned long>(relayLogic.pulseMs()), reason);
#else
      Serial.printf("[RELE] PULSO SIMULADO de %lu ms (%s). GPIO%d NO se mueve; el LED queda fijo.\n",
                    static_cast<unsigned long>(relayLogic.pulseMs()), reason, PIN_RELAY);
#endif
      break;
    case RelayPulse::Request::kBusy:
      Serial.println("[RELE] ya hay un pulso en curso: pedido ignorado");
      break;
    case RelayPulse::Request::kCooldown:
      Serial.println("[RELE] en pausa de enfriamiento: pedido ignorado");
      break;
  }
}

const char* resetReasonText() {
  static char buf[96];
  const char* t;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: t = "encendido / boton RST"; break;
    case ESP_RST_EXT: t = "pin externo"; break;
    case ESP_RST_SW: t = "reinicio por software"; break;
    case ESP_RST_PANIC: t = "ERROR FATAL (panic)"; break;
    case ESP_RST_INT_WDT: t = "WATCHDOG de interrupciones"; break;
    case ESP_RST_TASK_WDT: t = "WATCHDOG de tarea (programa colgado)"; break;
    case ESP_RST_WDT: t = "otro watchdog"; break;
    case ESP_RST_DEEPSLEEP: t = "salida de deep sleep"; break;
    case ESP_RST_BROWNOUT: t = "BAJA TENSION (brownout)"; break;
    case ESP_RST_SDIO: t = "SDIO"; break;
    default: t = nullptr; break;
  }
  // Código de la ROM (el mismo que imprime "rst:0x.." al arrancar).
  const unsigned rom = static_cast<unsigned>(esp_rom_get_reset_reason(0));
  if (!t) {
    switch (rom) {
      case 0x15: t = "reinicio por USB (carga de firmware o herramienta)"; break;
      case 0x16: t = "reinicio por USB-JTAG"; break;
      default: t = "desconocido"; break;
    }
  }
  snprintf(buf, sizeof buf, "%s [ROM rst:0x%02X]", t, rom);
  return buf;
}

// ------------------------------------------------------------------ setup / loop

void setup() {
  // 1) Lo primero: relé en reposo.
  relayhw::init(PIN_RELAY, RELAY_OUTPUT_ENABLED != 0, RELAY_ACTIVE_LEVEL);

  pinMode(PIN_STATUS_LED, OUTPUT);
  ledWrite(true);

  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);  // espera breve al monitor

  enableLoopWDT();  // si loop() se cuelga más de 5 s, el ESP32 se reinicia
  settingsLoad();
  printBanner();

  accessCtl.config().minScore = settings.minScore;
  accessCtl.config().releasePolls = ACCESS_RELEASE_POLLS;
  accessCtl.config().maxFails = ACCESS_MAX_FAILS;
  accessCtl.config().lockoutMs = ACCESS_LOCKOUT_MS;
  accessCtl.config().commErrorsOffline = ACCESS_COMM_ERRORS_OFFLINE;
  accessCtl.config().reconnectMs = ACCESS_RECONNECT_MS;

  sensor.setIdleHook(feedLoopWDT);
  sensor.begin(settings.fpBaud);
  while (millis() < FP_POWERUP_WAIT_MS) {
    feedLoopWDT();
    delay(10);
  }
  readerConnect(true);

  // Tras cualquier reinicio se exige que no haya dedo apoyado antes de evaluar.
  accessCtl.requireRelease();
  if (!readerOnline) accessCtl.markOffline(millis());

  Serial.println("Listo. Escribi 'ayuda' y Enter para ver los comandos.");
  ledWrite(false);
}

void loop() {
  feedLoopWDT();
  consolePoll();
  // El tiempo se lee DESPUES de la consola: un comando puede haber iniciado un pulso.
  const uint32_t now = millis();
  relayHousekeeping(now);
  ledUpdate(now);

  if (settings.accessOn) {
    if (now - lastAccessStep >= ACCESS_POLL_MS) {
      lastAccessStep = now;
      if (readerOnline && accessCtl.state() == AccessController::State::kOffline) {
        accessCtl.markOnline();
      }
      if (!readerOnline && accessCtl.state() != AccessController::State::kOffline) {
        accessCtl.markOffline(now);
      }
      handleAccessEvent(accessCtl.step(millis()), millis());
    }
  } else if (now - lastHealthPing >= HEALTH_PING_MS) {
    lastHealthPing = now;
    const bool ok = sensor.ping();
    if (ok) {
      healthFailures = 0;
      if (!readerOnline) {
        Serial.println("[LECTOR] volvio a responder.");
        readerConnect(true);
      }
    } else if (readerOnline && ++healthFailures >= ACCESS_COMM_ERRORS_OFFLINE) {
      // Igual que en modo acceso: varios fallos seguidos, no uno aislado.
      healthFailures = 0;
      readerOnline = false;
      relayhw::off();
      relayLogic.forceOff(now);
      Serial.println("[LECTOR] DESCONECTADO: no responde a la verificacion periodica (3 intentos).");
    }
  }
  delay(2);
}
