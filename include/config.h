// Configuración central de CronosESP32.
#pragma once

#define FW_NAME "CronosESP32"
#define FW_VERSION "0.1.0"

// ---------------------------------------------------------------- pines
// ESP32-C3 SuperMini (cara de componentes, USB abajo):
//   izquierda desde USB: 5V, G, 3.3, 4, 3, 2, 1, 0
//   derecha   desde USB: 5, 6, 7, 8, 9, 10, 20, 21
#define PIN_FP_RX 20   // RX del ESP32  <- cable TX del DY50
#define PIN_FP_TX 21   // TX del ESP32  -> cable RX del DY50
#define PIN_RELAY 4    // control futuro del relé (vía transistor, NO directo a IN)
#define PIN_STATUS_LED 8         // LED integrado de la SuperMini
#define STATUS_LED_ACTIVE_LOW 1  // en la SuperMini se enciende con nivel BAJO

// El lector va por UART1 (Serial1) remapeada a GPIO20/21. No se usa UART0 porque
// ESP-IDF manda allí sus mensajes de sistema y se mezclarían con el protocolo.
#define FP_UART Serial1

// ---------------------------------------------------------------- relé
// SEGURIDAD: la salida real al relé está DESHABILITADA. Con 0, GPIO4 queda fijo
// en reposo (0 V) y los pulsos solo se informan por el monitor y el LED.
// Para habilitarla hace falta, después de verificar la interfaz eléctrica,
// compilar con -DRELAY_OUTPUT_ENABLED=1 y -DRELAY_ACTIVE_LEVEL=<0|1>.
#ifndef RELAY_OUTPUT_ENABLED
#define RELAY_OUTPUT_ENABLED 0
#endif

#if RELAY_OUTPUT_ENABLED
#ifndef RELAY_ACTIVE_LEVEL
#error "Defini RELAY_ACTIVE_LEVEL (1 o 0) solo despues de verificar la interfaz electrica del rele"
#endif
#else
#ifndef RELAY_ACTIVE_LEVEL
// Previsto con transistor NPN: GPIO4 ALTO = relé activado. Reposo = 0 V.
#define RELAY_ACTIVE_LEVEL 1
#endif
#endif

#define RELAY_PULSE_DEFAULT_MS 1000
#define RELAY_PULSE_MIN_MS 100
#define RELAY_PULSE_MAX_MS 10000
#define RELAY_COOLDOWN_MS 1000   // pausa mínima entre el fin de un pulso y el siguiente
#define GPIO_DIAG_AUTO_REST_MS 60000  // "gpio4 alto" vuelve solo a 0 V tras 60 s

// ---------------------------------------------------------------- lector
#define FP_DEFAULT_BAUD 57600
#define FP_POWERUP_WAIT_MS 1000  // espera mínima desde el arranque antes del primer comando

// ---------------------------------------------------------------- acceso
#define ACCESS_MIN_SCORE_DEFAULT 0   // 0 = se acepta el criterio del propio lector
#define ACCESS_RELEASE_POLLS 3       // lecturas seguidas sin dedo = dedo retirado
#define ACCESS_MAX_FAILS 5           // denegaciones seguidas antes de bloquear
#define ACCESS_LOCKOUT_MS 30000
#define ACCESS_COMM_ERRORS_OFFLINE 3
#define ACCESS_RECONNECT_MS 3000
#define ACCESS_POLL_MS 120           // pausa entre lecturas en modo acceso
#define HEALTH_PING_MS 5000          // en mantenimiento: verificación periódica del lector
