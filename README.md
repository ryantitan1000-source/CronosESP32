# CronosESP32: control de acceso con DY50 + ESP32-C3 SuperMini

Lector de huellas DY50 + ESP32-C3 SuperMini + módulo relé de 5 V para un molinete.
Sin Wi-Fi ni nube. Por defecto el relé está en **SIMULACIÓN**: GPIO4 queda fijo en 0 V.
Con el transistor NPN, 0 V significa relé apagado.

## Conexiones
| Desde | Hacia |
|---|---|
| DY50 3V3 | 3.3 del ESP32 |
| DY50 GND | G del ESP32 (masa común con el relé) |
| DY50 TX | GPIO20 (RX del ESP32) |
| DY50 RX | GPIO21 (TX del ESP32) |
| DY50 T-OUT y T-3V3 | sin conectar, aislados |
| Relé VCC | 5V del ESP32 |
| Relé GND | masa común |
| Relé IN | **colector** del transistor 2N3904 (nunca directo a GPIO4) |
| 2N3904 emisor | GND |
| 2N3904 base | GPIO4 a través de 1 kΩ, y 4,7 kΩ o 10 kΩ de la base a GND (recomendado) |
| Relé COM y NO | entrada de apertura del molinete (contacto seco) |

2N3904 con la cara plana hacia vos y las patas hacia abajo, de izquierda a derecha: **Emisor – Base – Colector**.

## Instalar y compilar
1. Instalá [PlatformIO Core](https://platformio.org/install/cli). En la PC original se instaló con `uv tool install platformio`.
2. Revisá el puerto: en `platformio.ini` está fijado **COM10**. En otra PC mirá el número en el Administrador de dispositivos
   ("Dispositivo serie USB") y cambiá `upload_port`, `monitor_port` y `test_port`.
3. Dentro de esta carpeta:
```
pio run -e supermini -t upload      # compila y graba el firmware (relé en simulación)
pio device monitor                  # consola (Ctrl+C para salir)
```
Solo un programa puede usar el puerto a la vez: cerrá el monitor antes de grabar.

## Pruebas automáticas
```
pio test -e supermini                    # 40 pruebas en la placa; no mueven GPIO4
pio test -e hwtest -f test_hw_dy50       # con el lector real conectado
pio test -e hwtest -f test_hw_gpio4      # PONE GPIO4 EN ALTO: el relé hace clic (molinete desconectado)
```
**Las pruebas reemplazan el firmware de la placa.** Al terminar, volvé a grabarlo con `pio run -e supermini -t upload`.

## Comandos del monitor serie
`ayuda`, `estado`, `info`, `conectar`, `baudios`, `dedo [s]`, `enrolar <id>`,
`identificar` (no activa el relé), `lista`, `borrar <id>`, `borrartodo SI`,
`acceso on|off`, `pulso <ms>` (100–10000), `puntaje <n>`,
`gpio4 alto|bajo|estado` (multímetro; con el transistor conectado, `alto` ACTIVA el relé hasta 60 s),
`rele probar`, `traza on|off` (tramas en hexadecimal), `crudo <hex>`, `reiniciar`.

## Habilitar la salida real al relé
Solo después de verificar la interfaz (pendiente 4):
```
pio run -e supermini_rele -t upload
```
Este entorno agrega `-DRELAY_OUTPUT_ENABLED=1 -DRELAY_ACTIVE_LEVEL=1` sin perder la configuración USB.
No pongas `build_flags` a mano en `[env:supermini]`: reemplazaría la configuración USB y la consola dejaría de funcionar.

## Diseño
- El lector va por UART1 (Serial1) remapeada a GPIO20/21, a 57600 8N1, con protocolo EF01 propio.
  Cada respuesta se valida: checksum, tipo de paquete y que el ID quede dentro de la capacidad.
- El pulso lo apaga un esp_timer, aunque `loop()` esté ocupado. Hay una pausa de 1 s entre pulsos.
- Un solo pulso por apoyo de dedo: hacen falta 3 lecturas seguidas sin dedo para habilitar otro.
- Tras 5 denegaciones seguidas hay un bloqueo de 30 s.
- Si el lector falla 3 veces seguidas, el relé se fuerza a reposo y se reintenta la conexión.
- Al arrancar, el relé queda en reposo antes que cualquier otra cosa, y se exige retirar el dedo antes de evaluar.
- Un watchdog de 5 s reinicia la placa si el programa se cuelga. `estado` muestra el motivo del último reinicio.
- GPIO4 sale con la fuerza mínima (~5 mA) como protección extra.

## Registro de pruebas
| Qué | Resultado |
|---|---|
| Chip (esptool) | ESP32-C3 QFN32 rev 0.4, 4 MB, USB-Serial/JTAG, MAC 44:bd:8d:24:45:18 |
| Flash original | Vacía (solo una marca de fábrica en 0x50000). Respaldo en la PC original, ver `backup/LEEME.md` |
| Pruebas unitarias en la placa | v0.1.0: 42/42 (2026-10-01). v0.1.1: 40/40 en `supermini` (2026-10-05); las 3 que mueven GPIO4 pasaron a `test_hw_gpio4` y no se corrieron con el transistor conectado |
| Carga y reinicio por USB | OK (`rst:0x15`) |
| Comunicación con el DY50 | OK a 57600, 0 errores. VfyPwd OK. Capacidad 150, seguridad 3, paquete 128 B |
| LED GPIO8 (activo en bajo) | OK (el usuario vio el patrón de dos destellos) |
| Reinicio por encendido | OK dos veces: el lector se reconectó solo (28/28 y 88/88 tramas, 0 errores) |
| Cables del molinete | Cada cable conduce hasta su borne. Están en **COM y NO**. Falta ver que se cierren con el relé activado |
| Polaridad del relé | IN libre = 4,50 V; la luz verde se prende con IN a GND: módulo **activo en BAJO**. Clic no confirmado |
| Transistor 2N3904 | Instalado por el usuario: E→GND, B→1 kΩ→GPIO4, C→IN (orden correcto). Falta confirmar colores de la resistencia y probarlo |
| Multímetro | Se quedó sin batería durante las pruebas. Mediciones pendientes |
| Huellas con dedo real | **No probado todavía** |

## Pendiente (en orden)
1. Confirmar la resistencia de base (1 kΩ = marrón–negro–rojo) y que la luz verde del relé esté apagada con la placa encendida.
2. Probar el transistor con el molinete desconectado: `gpio4 alto` → clic y luz verde; `gpio4 bajo` → se apaga.
3. Con el multímetro con batería nueva, en Ω y con el relé activado: COM–NO ≈ 0 Ω. Con el relé en reposo: abierto.
4. Huellas: `dedo 30`, `enrolar 1`, `identificar`, `lista`, `borrar 1`.
5. Modo acceso en simulación (`acceso on`): una huella autorizada da un pulso simulado (LED fijo); una no registrada, no.
6. Grabar `supermini_rele` y repetir el paso 5 con el relé real, todavía sin el molinete.
7. Recién entonces conectar el molinete (consultar su manual: entrada de apertura por contacto seco normalmente abierto).

## Cambios
- **0.1.1** (auditoría adversarial):
  - El pulso de `rele probar` ya no puede cortarse a microsegundos.
  - El apagado de respaldo tiene un margen de 20 ms.
  - El parpadeo de "denegado" es seguro ante la vuelta de `millis()`.
  - En mantenimiento, el lector se marca desconectado tras 3 fallos seguidos.
  - `enrolar` y `borrar` rechazan IDs inválidos o con capacidad desconocida, y `enrolar` nunca sobrescribe si no puede verificar la posición.
  - Se respeta la cancelación y se avisa cuando una línea es demasiado larga.
  - Las pruebas que mueven GPIO4 quedaron separadas.
  - Se agregó el entorno `supermini_rele`.
- **0.1.0**: primera versión.
