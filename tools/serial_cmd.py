"""Envía comandos a la consola de CronosESP32 por USB y muestra la respuesta.

Uso (con el Python de PlatformIO, que ya trae pyserial):
  python tools/serial_cmd.py [--port COM10] [--reset] [--listen 3] [--wait 2] "estado" "info"

  --reset    reinicia la placa por USB (como el botón RST) y captura el arranque
  --listen   segundos a escuchar antes de enviar comandos (por defecto 1)
  --wait     segundos a esperar la respuesta de cada comando (por defecto 2)

No abre el puerto si otro programa (p. ej. el monitor serie) ya lo usa.
"""
import argparse
import sys
import time

import serial


def read_for(ser, seconds):
    end = time.time() + seconds
    out = bytearray()
    while time.time() < end:
        try:
            chunk = ser.read(ser.in_waiting or 1)
        except serial.SerialException:
            # La placa se reinició y el puerto USB desapareció un momento.
            out += b"\n[puerto USB reconectando...]\n"
            return out, False
        if chunk:
            out += chunk
    return out, True


def open_port(port, timeout_s=10.0):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = 115200
            ser.timeout = 0.1
            ser.dtr = False  # sin DTR/RTS: abrir el puerto NO reinicia la placa
            ser.rts = False
            ser.open()
            return ser
        except serial.SerialException as e:
            last = e
            time.sleep(0.3)
    raise SystemExit(f"No se pudo abrir {port}: {last}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--listen", type=float, default=1.0)
    ap.add_argument("--wait", type=float, default=2.0)
    ap.add_argument("commands", nargs="*")
    a = ap.parse_args()

    ser = open_port(a.port)
    if a.reset:
        # Secuencia de reinicio del USB-Serial/JTAG del ESP32-C3 (igual que esptool).
        # En Windows (usbser.sys) el cambio de RTS solo se envía junto con DTR,
        # por eso se reasigna DTR después de cada cambio (mismo truco que esptool).
        ser.dtr = False
        ser.rts = True
        ser.dtr = False
        time.sleep(0.2)
        ser.rts = False
        ser.dtr = False
        ser.close()
        time.sleep(0.5)
        ser = open_port(a.port)
    data, ok = read_for(ser, a.listen)
    if not ok:
        ser.close()
        time.sleep(1.0)
        ser = open_port(a.port)
        more, _ = read_for(ser, a.listen)
        data += more
    sys.stdout.write(data.decode("utf-8", "replace"))
    for c in a.commands:
        ser.write((c + "\n").encode())
        data, _ = read_for(ser, a.wait)
        sys.stdout.write(data.decode("utf-8", "replace"))
    sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
