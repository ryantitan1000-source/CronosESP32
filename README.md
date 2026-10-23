# CronosESP32: control de acceso con DY50 + ESP32-C3 SuperMini

Control de acceso para un molinete: lector de huellas DY50 + ESP32-C3 SuperMini + módulo relé de 5 V, y un **panel de escritorio**
(personas, vencimientos, ingreso por cédula, ventas y caja). Sin Wi-Fi ni nube. Para instalarlo en otra sucursal: [INSTALAR.md](INSTALAR.md).

Por defecto el firmware compila con el relé en **SIMULACIÓN** (`supermini`): GPIO4 queda fijo en 0 V. Con el transistor NPN, 0 V
significa relé apagado. La salida real al relé es el entorno `supermini_rele`.

**Estado (2026-10-22):** funcionando en la primera sucursal. El lector detecta el dedo (hace falta alimentar `T-3V3`), las huellas
registradas abren la entrada, las demás son rechazadas, la salida queda libre y el panel gestiona personas, vencimientos, cédulas y caja.
Pendiente recomendado: la resistencia pull-down de 4,7–10 kΩ entre la base del transistor y GND.

## Conexiones
| Desde | Hacia |
|---|---|
| DY50 3V3 | 3.3 del ESP32 |
| DY50 GND | G del ESP32 (masa común con el relé) |
| DY50 TX | GPIO20 (RX del ESP32) |
| DY50 RX | GPIO21 (TX del ESP32) |
| DY50 T-3V3 | **3V3**: mismo cable/Dupont que el 3V3 del lector (unión en Y), al 3.3 del ESP32. **Necesario**: sin esto el lector nunca detecta el dedo |
| DY50 T-OUT | sin conectar, aislado (salida del sensor táctil; el firmware no la usa) |
| Relé VCC | 5V del ESP32 |
| Relé GND | masa común |
| Relé IN | **colector** del transistor 2N3904 (nunca directo a GPIO4) |
| 2N3904 emisor | GND |
| 2N3904 base | GPIO4 a través de 1 kΩ, y 4,7 kΩ o 10 kΩ de la base a GND (recomendado) |
| Relé NO y COM | controlador del molinete, bloque **`EXIO2`**: `NO` → `Man2`, `COM` → `Gnd` (contacto seco; libera el sentido de **entrada** mientras dura el pulso). Las entradas `EXIO` son manuales: libres mientras hay contacto y se cierran al soltar. Con `Auto SW1/SW2` el molinete se bloqueaba (azul) si la salida estaba mantenida |
| Salida | Puente fijo entre `SW1` y `Gnd` del bloque **`Auto SW1`**: deja la salida libre todo el tiempo (confirmado el 2026-10-20 que la entrada por huella funciona con ese puente puesto, usando `EXIO2` para la entrada). `EXIO1` (`Man1`/`Gnd`) también es salida, manual |

2N3904 con la cara plana hacia vos y las patas hacia abajo, de izquierda a derecha: **Emisor – Base – Colector**.

## Instalar y compilar
1. Instalá [PlatformIO Core](https://platformio.org/install/cli). En la PC original se instaló con `uv tool install platformio`.
   En Windows también sirve Python 3.12 (winget) + `pip install platformio`. Ojo: puede que `pio.exe` y el `python.exe` real queden en
   `%LOCALAPPDATA%\Programs\Python\Python312\` (`Scripts\pio.exe`), fuera del PATH, y que el `python` del PATH sea el atajo de la
   Microsoft Store. Usá la ruta completa o agregá esas carpetas al PATH. Guía paso a paso para otra sucursal: [INSTALAR.md](INSTALAR.md).
2. Revisá el puerto: en `platformio.ini` está fijado **COM3** (en la PC original era COM10). En otra PC mirá el número en el Administrador de dispositivos
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
`bloquear <id>` / `permitir <id>` / `bloqueados` (huella guardada pero sin permiso de abrir; se guarda en la placa),
`acceso on|off`, `pulso <ms>` (100–10000), `puntaje <n>`,
`gpio4 alto|bajo|estado` (multímetro; con el transistor conectado, `alto` ACTIVA el relé hasta 60 s),
`rele probar`, `traza on|off` (tramas en hexadecimal), `crudo <hex>`, `reiniciar`.

Para registrar una huella: apoyá el dedo **de una vez, plano, centrado y firme** (el sensor captura en el primer contacto) y
repetí en el mismo punto la segunda vez. Si `enrolar` dice "no se pudieron combinar las dos lecturas", repetilo.

## Panel del molinete (app de escritorio)
Acceso directo **Molinete** en el escritorio y el menú Inicio (lanza `Molinete.vbs`, que arranca `tools/dashboard.py` oculto y abre
una ventana propia en http://127.0.0.1:8765). Base de datos SQLite en `data/molinete.db` (fuera de git; son datos personales).
- **Personas:** nombre, documento, teléfono, vencimiento y notas; estado Habilitado / Vencido / Sin huella; buscador y filtros.
- **Registrar huella:** botón por persona; guía paso a paso (apoyar, retirar, apoyar otra vez, guardar) hasta "quedó registrado y
  habilitado". Elige solo el ID libre del lector y avisa si la huella ya está registrada a otra persona.
- **Abrir manual:** botón grande; un clic = un pulso al relé. **Acceso por huella** se enciende/apaga con el interruptor de arriba.
- **Últimas entradas:** quién entró, denegados y aperturas manuales (se guardan en la base).
- Huellas del lector sin persona (por ejemplo, las registradas por consola) aparecen como aviso para asignarlas o borrarlas.
- **Ventas y caja** (pestaña *Ventas*): catálogo de productos con precio y costo ("Productos y precios"), formulario para registrar
  ventas o pagos (producto, cantidad, precio, persona opcional, fecha, nota) que se guardan con su fecha, tabla filtrable por producto, total del
  período, **calendario** (marca los días con ventas; al tocar un día filtra), botones **Ganancia del día / de la semana / del mes** (ganancia =
  vendido − costo; la semana va de lunes a domingo), vendido por producto y descarga a Excel (CSV con `;` y coma decimal). Tablas `productos`
  y `ventas` en `data/molinete.db`. Moneda: guaraníes (Gs.), sin decimales y con punto de miles. Las ventas guardan el nombre y el precio del momento, así que cambiar un precio no altera el historial.
- **Ingreso por cédula** (botón violeta de arriba): pone el panel en pantalla completa con un teclado grande; quien tecleé su cédula entra si la
  persona existe, no está bloqueada ni vencida (no hace falta que tenga huella). La cédula se compara solo por sus dígitos (con o sin puntos) y no
  puede repetirse entre personas. Protecciones: mensaje genérico en todos los rechazos, pausa de 30 s tras 5 intentos fallidos seguidos, 8 s entre
  aperturas de la misma persona, y se registra en "Últimas entradas". Para salir hay que **mantener presionado** el botón "Salir". Cada persona
  necesita su cédula cargada en el campo *Documento*.
- **Vencimientos sin volver a registrar (firmware 0.2.0):** una persona vencida se **bloquea** (la huella sigue guardada en el lector pero no
  abre) y al volver a pagar se pulsa **Renovar** (nueva fecha) y su huella vuelve a abrir al instante. El panel bloquea solo a los vencidos
  cada minuto **mientras esté abierto** (el lector no tiene reloj: si la PC está apagada, el vencimiento se aplica al volver a abrir el panel).
  **Bloquear** pausa a una persona a mano. "Borrar huella" sí elimina la huella del lector (habrá que registrarla de nuevo).
- El panel mantiene abierto el puerto serie: **cerrá el panel** antes de usar `serial_cmd.py`, el monitor serie o grabar el firmware
  (terminar el proceso `pythonw.exe` de `dashboard.py`). `python tools/dashboard.py --simular --port 8766 --db prueba.db` lo prueba con
  una placa falsa, sin mover el molinete. Solo escucha en 127.0.0.1 y exige un encabezado propio en las acciones.

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
- El sistema solo sabe "huella registrada / no registrada": no hay fechas de vencimiento ni conexión con cobros (máximo 150 huellas).
  Quién "pagó" lo decide quien registra (`enrolar`) y borra (`borrar`) huellas.

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
| Polaridad del relé | IN libre = 4,50 V; la luz verde se prende con IN a GND: módulo **activo en BAJO**. Clic confirmado el 2026-10-12 |
| Transistor 2N3904 | Instalado por el usuario: E→GND, B→1 kΩ→GPIO4, C→IN (orden correcto). Conmuta bien (clic + luz verde). No se ve la resistencia pull-down en la foto |
| Multímetro | Se quedó sin batería durante las pruebas. Mediciones pendientes |
| Esta PC (2026-10-12) | Puerto **COM3**; Python 3.12.10 + PlatformIO 6.2.0; compila (Flash 22,9 %, RAM 5,8 %) y graba OK. Pruebas en la placa: 40/40 en `supermini` |
| Salida real al relé (2026-10-12) | `supermini_rele` + `rele probar` (1000 ms): clic, luz verde y el molinete reaccionó (confirmado por el usuario) |
| Detección de dedo (2026-10-12 → 15) | Sin `T-3V3` alimentado: siempre "sin dedo". Con `T-3V3` unido a 3V3: detecta de forma sostenida (2026-10-15). Ver "Resuelto" |
| Huellas con dedo real (2026-10-15) | `enrolar 1` OK (el 1.er intento falló en RegModel 0x0A por apoyo distinto). `identificar`: coincide con ID 1, puntaje 229–244; otro dedo: no registrada |
| Modo acceso con relé real (2026-10-15) | Registrada → CONCEDIDO ID 1 y pulso de 1000 ms (el relé hace clic). 5 denegaciones seguidas → bloqueo de 30 s. Funciona como se diseñó |
| Molinete (2026-10-15 → 20) | Un cable roto hacía que el pulso no llegara; arreglado, el pulso libera el paso unos segundos. Con `Auto SW1/SW2` y la salida mantenida el molinete se bloqueaba; con el relé en `EXIO2` funciona. Ver "Resuelto: el molinete" |
| Panel, ventas y cédula (2026-10-22) | Probados en una placa simulada: registro de huella guiado, bloqueo/renovación, ventas y ganancia (día/semana/mes), calendario, ingreso por cédula y sus límites |

## Pendiente
- Resistencia pull-down de 4,7–10 kΩ entre la base y GND del 2N3904 (evita un clic del relé al encender o reiniciar) y soldar/aislar las uniones.
- Cada sucursal debe cargar las cédulas de sus personas (campo *Documento*) para el ingreso por cédula, y sus productos y precios para la caja.
- Salida controlada por el firmware (no solo libre): requiere un segundo relé y un cambio de firmware.

## Resuelto: el molinete no reaccionaba al pulso (2026-10-15 → 2026-10-20)
**Síntoma:** el relé hacía clic con cada pulso pero el molinete no se liberaba. **Causas:** un cable del relé al controlador estaba roto, y
las entradas `Auto SW1/SW2` del controlador se bloquean (el indicador pasa a azul) si la salida está mantenida mientras se pide la entrada.
**Solución:** relé en las entradas manuales: `NO` → `Man2` y `COM` → `Gnd` del bloque **`EXIO2`** (entrada), y la salida libre con un
puente fijo entre `SW1` y `Gnd` de `Auto SW1`. En este controlador: verde = libre, azul = bloqueado; `EXIO1` = salida, `EXIO2` = entrada;
las entradas `EXIO` son manuales (libres mientras hay contacto y se cierran al soltar); `Counter1_Enter` / `Counter2_Exit` son salidas de
contador (NO/COM/NC). El tiempo de apertura es el pulso del relé (`pulso <ms>`, hoy 4000). Un mismo relé no sirve como salida y entrada a la vez
(un solo contacto); usar su borne `NC` para la salida tampoco funcionó.

## Resuelto: el DY50 no detectaba el dedo (2026-10-12 → 2026-10-15)
**Causa y solución:** el pin `T-3V3` (alimentación del circuito táctil del módulo) estaba sin conectar. Al unirlo al 3,3 V (mismo punto
que `3V3`, con una Y en el Dupont del 3.3) el lector pasó a detectar el dedo de forma sostenida y se pudo registrar e identificar.
Sin ese cable, `GenImg` responde siempre "sin dedo". `T-OUT` no hace falta.

**Síntoma (antes de la solución):** `dedo` siempre daba "sin dedo", aun con el pulgar firme y quieto sobre el vidrio. La comunicación era
perfecta (0 errores, VfyPwd OK) y la luz del sensor estaba encendida.

**Mediciones** (con `tools/diag_bridge` y `tools/dy50_probe.py`):
- `GenImg` respondió 0x02 ("sin dedo") en más de 5 600 lecturas. Solo hubo 2 "OK" aislados, de una lectura cada uno,
  que volvieron a 0x02 enseguida y llegaron en ~6 ms (más rápido que una captura real).
- `UpImage` devolvía 36 864 B (256×288), pero era memoria sin inicializar (franja negra + ruido) e idéntica en 3 descargas:
  el sensor **nunca llegaba a capturar** una imagen.
- Quitarle la corriente al lector (desenchufar el USB unos segundos) no cambiaba nada.

**Hipótesis (confirmada el 2026-10-15):** en este módulo la decisión de "hay dedo" depende del circuito táctil (pines `T-3V3`/`T-OUT`)
y `T-3V3` necesita alimentación. Según las fichas, el DY50 usa el chip AS608 y en esa familia el circuito táctil tiene su propia
entrada de alimentación de 3,3 V. El pin de poder del lector está rotulado `3V3`: **no** pasarlo a 5 V.

**Cómo corregirlo en otro lector igual:** con el USB desenchufado, unir el pin **`T-3V3`** con el pin **`3V3`** del lector (no a 5V ni a
TX/RX/GND); `T-OUT` queda libre. Enchufar y correr `dedo 30` apoyando el pulgar firme.

**Herramientas de diagnóstico:**
```
pio run -d tools/diag_bridge -t upload                              # puente transparente USB <-> lector (relé en reposo)
python tools/dy50_probe.py --port COM3 --out logs/img --poll 40     # GenImg continuo; al detectar dedo guarda la imagen real
pio run -e supermini_rele -t upload                                 # volver al firmware normal (o -e supermini)
```

## Cambios
- **0.2.0** (2026-10-20): lista de huellas **bloqueadas** (`block_list`, 1024 IDs, guardada en la memoria no volátil): una huella bloqueada
  sigue guardada pero el modo acceso no la deja abrir (`[ACCESO] BLOQUEADO: ID n`). Comandos `bloquear`, `permitir`, `bloqueados`;
  `identificar` y `estado` lo informan; registrar o borrar un ID le quita el bloqueo. Sin lista guardada no hay ninguna bloqueada, así que las
  huellas ya registradas siguen funcionando. 7 pruebas nuevas (47/47 en la placa). Nuevo panel de escritorio (`tools/dashboard.py`).
- **Sin cambios en el firmware** (2026-10-15): se documenta que `T-3V3` debe alimentarse (sin eso el DY50 no detecta el dedo),
  el registro de la primera huella y el problema abierto del molinete.
- **Sin cambios en el firmware** (2026-10-12): puerto fijado en COM3 (`platformio.ini` y `tools/`), nuevas herramientas
  de diagnóstico `tools/diag_bridge` y `tools/dy50_probe.py`, y este README actualizado.
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
