# Instalar en una sucursal nueva

Control de acceso por huella (y por cédula) para un molinete, más un panel de escritorio con personas, vencimientos y caja.
Funciona sin internet. Esta guía sirve para repetir la instalación en otra sucursal.

## Qué necesitás
- **ESP32-C3 SuperMini**, **lector de huellas DY50**, **módulo de relé de 5 V**, un transistor **2N3904**, resistencias (1 kΩ y 4,7–10 kΩ),
  cables Dupont y un cable USB de datos.
- Una **PC con Windows 10/11** que quede en recepción, con el cable USB de la placa siempre conectado.
- Acceso a los bornes del controlador del molinete (contacto seco).

## 1. Cableado
Seguí la tabla "Conexiones" del [README](README.md). Puntos que costó descubrir:
- El lector DY50 **necesita `T-3V3` unido a `3V3`**; sin eso nunca detecta el dedo.
- No conectes el `IN` del relé directo a la placa: va por el transistor. Poné la resistencia de **4,7–10 kΩ entre la base y GND** para que el
  relé no se active solo al reiniciar.
- El relé es un contacto seco hacia el controlador del molinete. **Cada modelo de molinete es distinto**: en el de la primera sucursal, las
  entradas `Auto SW1/SW2` se bloqueaban si la salida estaba mantenida, y funcionó usando la entrada manual de entrada (`EXIO2`: `NO`→`Man2`,
  `COM`→`Gnd`) con la salida libre mediante un puente en `Auto SW1`. Probá con una pinza (unir los dos bornes un segundo) qué entrada libera qué
  sentido antes de cablear el relé.

## 2. Software en la PC
1. Instalá Python 3.12 (por ejemplo `winget install Python.Python.3.12`).
2. En la carpeta del proyecto: `python -m pip install -r requirements.txt` y `python -m pip install platformio`.
3. Revisá en `platformio.ini` el puerto (`upload_port`, `monitor_port`, `test_port`): mirá el número "Dispositivo serie USB" en el Administrador de dispositivos.

## 3. Grabar el firmware en la placa
```
pio run -e supermini_rele -t upload
```
(Usá la ruta completa de `pio` si no está en el PATH.) Esto activa la salida real al relé. Para probar sin mover el molinete usá `-e supermini`.

Después, por la consola serie (`pio device monitor`) dejá lista la placa:
```
pulso 4000        duración en ms que queda libre la entrada
acceso on         una huella autorizada abre el molinete (se guarda, sobrevive a reinicios)
```

## 4. El panel
1. Doble clic en `Molinete.vbs`, o creá el acceso directo con
   `powershell -ExecutionPolicy Bypass -File tools\crear_acceso_directo.ps1` (queda en el Escritorio y en el menú Inicio).
2. El panel busca la placa solo por USB (no hace falta saber el COM). Abre en http://127.0.0.1:8765, únicamente desde esa PC.
3. En el panel: cargá a las personas (nombre, **cédula**, vencimiento), registrá cada huella con "Registrar huella", y cargá los productos con
   precio y costo en la pestaña **Ventas**.
4. Dejá el panel abierto durante el día: bloquea solo a los vencidos y guarda el registro de entradas.

## Datos y respaldo
- Todo lo del panel está en `data/molinete.db` (personas, entradas, productos y ventas). **No se sube a GitHub** (son datos personales).
  Copiá ese archivo a otro lugar cada tanto.
- Las huellas viven en la memoria del lector DY50 (hasta 150), no en la PC.

## Seguridad
- El panel solo escucha en `127.0.0.1` y rechaza pedidos que no vengan de su propia página.
- El ingreso por cédula pausa 30 s tras 5 intentos fallidos y no repite la apertura a la misma persona en 8 s.
- Si cambiás el cableado, probá primero con el molinete despejado: cada pulso libera el paso de verdad.

## Si algo falla
- *El lector no detecta el dedo:* revisá `T-3V3` y el cableado `TX/RX/3V3/GND`.
- *El panel dice "Placa desconectada":* revisá el cable USB; con otro programa usando el puerto (monitor serie, `serial_cmd.py`) el panel no puede abrirlo.
- *El relé hace clic pero el molinete no responde:* revisá los tornillos de los bornes y qué entrada del controlador usa tu modelo.
