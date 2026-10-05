"""Puente serie en segundo plano para pruebas interactivas.

- Mantiene abierto el puerto USB de la placa (se reconecta solo si la placa se reinicia).
- Guarda todo lo recibido en logs/serial.log con marca de tiempo.
- Envía a la placa cada línea nueva que se agregue a logs/cmd.txt.
- Termina cuando aparece el archivo logs/stop.

Uso:  python tools/serial_bridge.py [--port COM10]
"""
import argparse
import os
import time
from datetime import datetime

import serial

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
LOG = os.path.join(BASE, "serial.log")
CMD = os.path.join(BASE, "cmd.txt")
STOP = os.path.join(BASE, "stop")


def stamp():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def log_line(f, text):
    f.write(f"{stamp()} {text}\n")
    f.flush()


def open_port(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.05
    ser.dtr = False  # abrir el puerto no reinicia la placa
    ser.rts = False
    ser.open()
    return ser


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    a = ap.parse_args()
    os.makedirs(BASE, exist_ok=True)
    if os.path.exists(STOP):
        os.remove(STOP)
    open(CMD, "a").close()
    cmd_offset = os.path.getsize(CMD)  # solo comandos nuevos

    with open(LOG, "a", encoding="utf-8") as f:
        log_line(f, f"[puente] iniciado en {a.port}")
        ser = None
        pending = ""
        while not os.path.exists(STOP):
            if ser is None:
                try:
                    ser = open_port(a.port)
                    log_line(f, "[puente] puerto abierto")
                except serial.SerialException:
                    time.sleep(0.5)
                    continue
            try:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    pending += data.decode("utf-8", "replace")
                    while "\n" in pending:
                        line, pending = pending.split("\n", 1)
                        log_line(f, line.rstrip("\r"))
                size = os.path.getsize(CMD)
                if size > cmd_offset:
                    with open(CMD, "r", encoding="utf-8") as c:
                        c.seek(cmd_offset)
                        new = c.read()
                    cmd_offset = size
                    for cmd in new.splitlines():
                        cmd = cmd.strip()
                        if cmd:
                            log_line(f, f"[puente] >>> {cmd}")
                            ser.write((cmd + "\n").encode())
            except (serial.SerialException, OSError):
                log_line(f, "[puente] puerto perdido (reinicio de la placa?), reintentando")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(0.5)
        if ser:
            ser.close()
        log_line(f, "[puente] detenido")


if __name__ == "__main__":
    main()
