# CronosESP32: control de acceso con DY50 + ESP32-C3 SuperMini

Sin Wi-Fi ni nube. El relé está **DESHABILITADO** (simulación): GPIO4 queda fijo en 0 V.

## Compilar, cargar y probar (PowerShell, dentro de esta carpeta)
```
$env:PATH = "$env:USERPROFILE\.local\bin;$env:PATH"
pio run -e supermini -t upload      # compila y graba el firmware (COM10)
pio device monitor                  # consola (Ctrl+C para salir)
pio test -e supermini               # 42 pruebas en la placa (sin lector)
pio test -e hwtest                  # prueba con el DY50 real conectado
```
Solo un programa puede usar el COM10 a la vez: cerrá el monitor antes de grabar.
Respaldo de la flash original: `backup/flash_original_44bd8d244518.bin`.

## Comandos del monitor serie
`ayuda`, `estado`, `info`, `conectar`, `baudios`, `dedo [s]`, `enrolar <id>`,
`identificar` (no activa el relé), `lista`, `borrar <id>`, `borrartodo SI`,
`acceso on|off`, `pulso <ms>` (100–10000), `puntaje <n>`,
`gpio4 alto|bajo|estado` (multímetro, con IN del relé desconectado),
`rele probar`, `traza on|off` (tramas en hexadecimal), `crudo <hex>`, `reiniciar`.

## Diseño
- Lector en UART1 (Serial1) remapeada a GPIO20 (RX) y GPIO21 (TX), a 57600 8N1, protocolo EF01.
  No se usa UART0 porque ESP-IDF manda allí sus mensajes de sistema.
- El pulso se apaga con un esp_timer, aunque `loop()` esté bloqueado. Hay una pausa de 1 s entre pulsos.
- Un solo pulso por apoyo de dedo: hacen falta 3 lecturas seguidas sin dedo para habilitar otro.
- Tras 5 denegaciones seguidas hay un bloqueo de 30 s.
- Si el lector no responde 3 veces seguidas, el relé se fuerza a reposo y se reintenta la conexión cada 3 s.
- Al arrancar, el relé queda en reposo antes que cualquier otra cosa, y se exige retirar el dedo antes de evaluar.
- Un watchdog de 5 s reinicia la placa si el programa se cuelga. `estado` muestra el motivo del último reinicio.

## Registro de pruebas (2026-10-01)
| Qué | Resultado |
|---|---|
| Chip (esptool) | ESP32-C3 QFN32 rev 0.4, 4 MB, USB-Serial/JTAG, COM10, MAC 44:bd:8d:24:45:18 |
| Flash original | Vacía (solo una marca de fábrica en 0x50000). Respaldo hecho |
| 42 pruebas unitarias en la placa | PASARON |
| Carga y reinicio por USB | OK (`rst:0x15`) |
| Comunicación con el DY50 | OK a 57600, 0 errores. VfyPwd OK. Capacidad 150, seguridad 3, paquete 128 B, 0 huellas |
| LED GPIO8 (activo en bajo) | OK (el usuario vio el patrón de dos destellos) |
| Botón RST | El usuario no encontró el botón en su placa. No se probó |
| Reinicio por encendido | OK dos veces: tras `rst:0x01` el lector se reconectó solo (28/28 y 88/88 tramas, 0 errores) |
| Cables del molinete (contactos del relé) | Cada cable conduce hasta su borne. En reposo no hay continuidad entre ellos, y el borne libre sí la tiene con COM. Conclusión: están en **COM y NO** (normalmente abierto). Falta comprobar que se cierren con el relé activado |
| Polaridad del relé | IN libre = 4,50 V (escala 20 V). La luz verde del módulo se prende al llevar IN a GND, lo que es coherente con un módulo **activo en BAJO**. El clic de la bobina con IN a GND no se confirmó |
| Multímetro | Se quedó sin batería durante las pruebas. Las últimas lecturas (p. ej. 0.009) no son confiables |
| Resistencia en IN (agregada por el usuario) | Configuración sin documentar ni verificar |

## Pendiente (en orden)
1. Medir GPIO4 con multímetro (con batería nueva): `gpio4 alto` debería dar ~3,3 V y `gpio4 bajo` ~0 V entre el pin 4 y G.
2. Probar la detección de dedo con `dedo 30`; después `enrolar 1`, `identificar`, `lista` y `borrar 1`. Todavía no se probó con un dedo real.
3. Probar el modo acceso en simulación: `acceso on`. Una huella autorizada da un pulso simulado (LED fijo); una no autorizada, parpadeo.
4. Relé: confirmar el clic con IN a GND, sin el ESP32 controlándolo. Todo indica un módulo **activo en BAJO**.
   ADVERTENCIA: una resistencia sola entre GPIO4 e IN **no sirve** para un módulo activo en BAJO de 5 V. Con GPIO4 en 0 V el relé se activa,
   y con 3,3 V puede seguir activado (ya hizo clic con IN a 3,3 V). Hace falta el transistor.
   La interfaz propuesta (aún sin componentes): transistor NPN con colector a IN, emisor a GND, 1 kΩ de GPIO4 a la base y 4,7 kΩ (o 10 kΩ) de la base a GND.
   Hay que verificar el orden de patas con la hoja de datos del fabricante exacto. Ojo: la numeración del BC547 cambia según el fabricante,
   y el BC547 (C-B-E) y el 2N3904 (E-B-C) tienen las patas en espejo.
   Hay que medir la corriente de IN: algunos módulos piden hasta 20 mA, así que se mantiene la base de 1 kΩ.
   GPIO4 queda flotante durante el reinicio (también al abrir o cerrar el monitor serie), por eso la resistencia de base a GND es obligatoria.
   Recién entonces se compila con `-DRELAY_OUTPUT_ENABLED=1 -DRELAY_ACTIVE_LEVEL=1`.
