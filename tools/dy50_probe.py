"""Sonda de diagnóstico del DY50 a través del puente transparente (tools/diag_bridge).

Con el firmware diag_bridge en la placa, esta herramienta habla el protocolo EF01
directamente con el lector:
  - modo ciclos (por defecto): por cada ciclo pide GenImg y luego UpImage (descarga la
    imagen cruda del buffer del sensor), guarda un PNG y muestra estadísticas.
  - modo --poll N: hace GenImg continuo durante N segundos y, al primer "OK" (dedo
    detectado), descarga la imagen real y la guarda.

Uso (con el Python de PlatformIO, que ya trae pyserial):
  python tools/dy50_probe.py --port COM3 --out logs/img [--cycles 3] [--poll 40]

Una imagen de buffer sin capturar se ve como una franja negra más ruido (memoria sin
inicializar) y es idéntica entre ciclos; una captura real muestra las crestas del dedo.
No abre el puerto si otro programa (p. ej. el monitor serie) ya lo usa.
"""
import argparse
import os
import struct
import sys
import time
import zlib

import serial

HDR = b"\xEF\x01"
ADDR = 0xFFFFFFFF


def build(content, pid=0x01):
    length = len(content) + 2
    body = bytes([pid]) + struct.pack(">H", length) + bytes(content)
    return HDR + struct.pack(">I", ADDR) + body + struct.pack(">H", sum(body) & 0xFFFF)


def read_exact(ser, n, deadline):
    out = bytearray()
    while len(out) < n:
        if time.time() > deadline:
            raise TimeoutError(f"timeout leyendo {n} bytes (llegaron {len(out)})")
        chunk = ser.read(n - len(out))
        if chunk:
            out += chunk
    return bytes(out)


def read_frame(ser, timeout=2.0):
    """Devuelve (pid, contenido) validando el checksum; resincroniza buscando EF 01."""
    deadline = time.time() + timeout
    prev = b""
    while True:
        b = read_exact(ser, 1, deadline)
        if prev == b"\xEF" and b == b"\x01":
            break
        prev = b
    rest = read_exact(ser, 7, deadline)  # dirección(4) + pid(1) + largo(2)
    pid = rest[4]
    length = struct.unpack(">H", rest[5:7])[0]
    payload = read_exact(ser, length, deadline)  # contenido + checksum(2)
    content, csum = payload[:-2], struct.unpack(">H", payload[-2:])[0]
    calc = (pid + (length >> 8) + (length & 0xFF) + sum(content)) & 0xFFFF
    if calc != csum:
        raise ValueError(f"checksum malo: calculado {calc:04X}, recibido {csum:04X}")
    return pid, content


def cmd(ser, content, timeout=2.0):
    ser.reset_input_buffer()
    ser.write(build(content))
    pid, c = read_frame(ser, timeout)
    if pid != 0x07:
        raise ValueError(f"se esperaba ACK (07), llegó pid {pid:02X}")
    return c[0], c[1:]


def up_image(ser):
    """Descarga la imagen del buffer. Devuelve (código, bytes)."""
    ser.reset_input_buffer()
    ser.write(build([0x0A]))
    pid, c = read_frame(ser, 2.0)
    if pid != 0x07:
        raise ValueError(f"UpImage: se esperaba ACK, llegó pid {pid:02X}")
    code = c[0]
    if code != 0x00:
        return code, b""
    data = bytearray()
    packets = 0
    while True:
        pid, c = read_frame(ser, 3.0)
        data += c
        packets += 1
        if pid == 0x08:
            break
        if pid != 0x02:
            raise ValueError(f"UpImage: pid inesperado {pid:02X} en el paquete {packets}")
    return 0x00, bytes(data)


def write_png(path, pixels, w, h):
    raw = b"".join(b"\x00" + pixels[y * w:(y + 1) * w] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def to_gray(data):
    px = bytearray()
    for b in data:
        px.append((b >> 4) * 17)
        px.append((b & 0x0F) * 17)
    return bytes(px)


def dims_for(npx):
    for w in (256, 192, 160, 128):
        if npx % w == 0:
            return w, npx // w
    return npx, 1


def describe(px):
    n = len(px)
    mean = sum(px) / n
    std = (sum((p - mean) ** 2 for p in px) / n) ** 0.5
    levels = [0] * 16
    for p in px:
        levels[p // 17] += 1
    hist = " ".join(f"{100.0 * v / n:4.1f}" for v in levels)
    return mean, std, min(px), max(px), hist


def save_image(out_dir, name, data):
    px = to_gray(data)
    w, h = dims_for(len(px))
    mean, std, lo, hi, hist = describe(px)
    write_png(os.path.join(out_dir, name), px, w, h)
    print(f"  imagen {len(data)} B -> {w}x{h}  media={mean:.1f} desv={std:.1f} min={lo} max={hi}")
    print(f"  histograma % por nivel 0..15: {hist}")
    print(f"  guardada: {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--out", required=True, help="carpeta donde guardar los PNG")
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--lead", type=float, default=0.0, help="segundos de espera antes de empezar")
    ap.add_argument("--poll", type=float, default=0.0,
                    help="modo sondeo: segundos de GenImg continuo; al primer OK descarga la imagen")
    ap.add_argument("--max-captures", type=int, default=1)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    ser = serial.Serial()
    ser.port = a.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False  # abrir el puerto no reinicia la placa
    ser.rts = False
    ser.open()
    time.sleep(0.6)

    code, _ = cmd(ser, [0x13, 0, 0, 0, 0])
    print(f"VfyPwd: código 0x{code:02X}")
    sys.stdout.flush()
    time.sleep(a.lead)

    if a.poll > 0:
        t_end = time.time() + a.poll
        t_start = time.time()
        polls = 0
        captures = 0
        counts = {}
        last = None
        while time.time() < t_end and captures < a.max_captures:
            code, _ = cmd(ser, [0x01])  # GenImg
            polls += 1
            counts[code] = counts.get(code, 0) + 1
            if code != last:
                print(f"  [{time.time() - t_start:5.1f} s] GenImg -> 0x{code:02X}")
                sys.stdout.flush()
                last = code
            if code == 0x00:
                icode, data = up_image(ser)
                if icode == 0x00:
                    print("  IMAGEN CAPTURADA:")
                    save_image(a.out, f"img_captura{captures}.png", data)
                else:
                    print(f"  UpImage -> código 0x{icode:02X}")
                captures += 1
                sys.stdout.flush()
                time.sleep(0.5)
            else:
                time.sleep(0.03)
        print(f"Fin del sondeo: {polls} GenImg; códigos: "
              + ", ".join(f"0x{c:02X} x{n}" for c, n in sorted(counts.items())))
        ser.close()
        return

    for k in range(a.cycles):
        t0 = time.time()
        code, _ = cmd(ser, [0x01])  # GenImg
        print(f"[ciclo {k}] GenImg -> 0x{code:02X}")
        icode, data = up_image(ser)
        if icode != 0x00:
            print(f"  UpImage -> código 0x{icode:02X} (sin datos)  [{time.time() - t0:.1f} s]")
        else:
            save_image(a.out, f"img_ciclo{k}_genimg{code:02X}.png", data)
            print(f"  [{time.time() - t0:.1f} s]")
        sys.stdout.flush()
    ser.close()


if __name__ == "__main__":
    main()
