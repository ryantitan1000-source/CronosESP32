"""Panel del molinete: personas, huellas y apertura manual (solo en esta PC).

Uso:  python tools/dashboard.py [--com auto|COM3] [--port 8765] [--simular]
Abrir http://127.0.0.1:8765  (o el acceso directo "Molinete").

- Base de datos SQLite en data/molinete.db (personas y registro de entradas).
- Mantiene abierto el puerto serie de la placa: cierre el panel antes de usar otras
  herramientas que necesiten el puerto (monitor serie, serial_cmd.py, grabar firmware).
- Solo escucha en 127.0.0.1 y exige un encabezado propio en las acciones (evita que otra
  página web del navegador pueda abrir el molinete).
- --simular: placa falsa en memoria, para probar el panel sin tocar el molinete.
"""
import argparse
import json
import os
import re
import sqlite3
import threading
import time
from collections import deque
from datetime import date, datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
DB_PATH = os.path.join(RAIZ, "data", "molinete.db")
CAPACIDAD = 150
PUERTO_HTTP = 8765


# ------------------------------------------------------------------ base de datos

def sql(q, args=(), escribir=False):
    con = sqlite3.connect(DB_PATH, timeout=10)
    try:
        con.row_factory = sqlite3.Row
        cur = con.execute(q, args)
        if escribir:
            con.commit()
            return cur.lastrowid
        return cur.fetchall()
    finally:
        con.close()


def init_db():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    con = sqlite3.connect(DB_PATH)
    con.executescript("""
        CREATE TABLE IF NOT EXISTS personas (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          nombre TEXT NOT NULL,
          documento TEXT DEFAULT '',
          telefono TEXT DEFAULT '',
          vence TEXT DEFAULT NULL,
          notas TEXT DEFAULT '',
          huella_id INTEGER UNIQUE,
          creado TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS entradas (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          ts TEXT NOT NULL,
          huella_id INTEGER,
          persona_id INTEGER,
          resultado TEXT NOT NULL,
          puntaje INTEGER
        );
    """)
    con.executescript("""
        CREATE TABLE IF NOT EXISTS productos (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          nombre TEXT NOT NULL UNIQUE COLLATE NOCASE,
          precio REAL NOT NULL DEFAULT 0,
          costo REAL NOT NULL DEFAULT 0,
          activo INTEGER NOT NULL DEFAULT 1
        );
        CREATE TABLE IF NOT EXISTS ventas (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          ts TEXT NOT NULL,
          fecha TEXT NOT NULL,
          producto_id INTEGER,
          concepto TEXT NOT NULL,
          cantidad REAL NOT NULL,
          precio REAL NOT NULL,
          costo REAL NOT NULL DEFAULT 0,
          persona_id INTEGER,
          notas TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_ventas_fecha ON ventas(fecha);
    """)
    cols = [r[1] for r in con.execute("PRAGMA table_info(personas)")]
    if "bloqueado" not in cols:  # migración: huella guardada pero sin permiso de abrir
        con.execute("ALTER TABLE personas ADD COLUMN bloqueado INTEGER NOT NULL DEFAULT 0")
    con.commit()
    con.close()


def estado_persona(p, hoy=None):
    """sin_huella | habilitado | bloqueado | vencido (vencido pero todavía sin bloquear)."""
    hoy = hoy or date.today().isoformat()
    if p["huella_id"] is None:
        return "sin_huella"
    if p["bloqueado"]:
        return "bloqueado"
    if p["vence"] and p["vence"] < hoy:
        return "vencido"
    return "habilitado"


def persona_json(p):
    d = {k: p[k] for k in ("id", "nombre", "documento", "telefono", "vence", "notas", "huella_id", "bloqueado")}
    d["estado"] = estado_persona(p)
    d["dias"] = None
    if p["vence"]:
        try:
            d["dias"] = (date.fromisoformat(p["vence"]) - date.today()).days
        except ValueError:
            pass
    return d


def registrar_entrada(resultado, huella_id=None, puntaje=None):
    persona = None
    if huella_id is not None:
        r = sql("SELECT id FROM personas WHERE huella_id=?", (huella_id,))
        persona = r[0]["id"] if r else None
    sql("INSERT INTO entradas (ts, huella_id, persona_id, resultado, puntaje) VALUES (?,?,?,?,?)",
        (datetime.now().isoformat(timespec="seconds"), huella_id, persona, resultado, puntaje), escribir=True)


# ------------------------------------------------------------------ placa (puerto serie)

def detectar_com():
    """Busca la placa ESP32-C3 (USB-Serial/JTAG de Espressif, VID 303A / PID 1001) entre los puertos serie."""
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            if p.vid == 0x303A and p.pid == 0x1001:
                return p.device
    except Exception:  # noqa: BLE001
        pass
    return None


class Ocupado(Exception):
    pass


class Simulador:
    """Placa falsa: contesta como el firmware, para probar el panel sin molinete."""

    def __init__(self, emitir):
        self.emitir = emitir
        self.huellas = [1]
        self.bloqueados = set()
        self.acceso = False
        self.cancelar = False

    def recibir(self, cmd):
        threading.Thread(target=self._ejecutar, args=(cmd,), daemon=True).start()

    def _ejecutar(self, cmd):
        e = self.emitir
        if cmd in ("x", "X"):
            self.cancelar = True
        elif cmd == "rele probar":
            e("[RELE] PULSO de 1000 ms (prueba manual)")
        elif cmd == "estado":
            e("=== ESTADO ===")
            e("  Lector: CONECTADO a 57600 baudios, capacidad 150")
            e("  Modo acceso: " + ("ACTIVADO | estado: ESPERANDO DEDO" if self.acceso else "DESACTIVADO (mantenimiento)"))
        elif cmd == "lista":
            ids = sorted(self.huellas)
            e("Posiciones ocupadas (%d): %s" % (len(ids), " ".join(map(str, ids)) if ids else "ninguna"))
        elif cmd.startswith("acceso "):
            self.acceso = cmd.endswith("on")
            e("Modo acceso %s (guardado)." % ("ACTIVADO: una huella autorizada genera un pulso" if self.acceso else "DESACTIVADO (mantenimiento)"))
        elif cmd == "bloqueados":
            ids = sorted(self.bloqueados)
            e("Bloqueados (%d): %s" % (len(ids), " ".join(map(str, ids)) if ids else "ninguno"))
        elif cmd.startswith("bloquear "):
            n = int(cmd.split()[1])
            self.bloqueados.add(n)
            e("ID %d BLOQUEADO: su huella sigue guardada pero NO abre el molinete (guardado)." % n)
        elif cmd.startswith("permitir "):
            n = int(cmd.split()[1])
            self.bloqueados.discard(n)
            e("ID %d PERMITIDO: su huella vuelve a abrir el molinete (guardado)." % n)
        elif cmd.startswith("borrar "):
            n = int(cmd.split()[1])
            self.bloqueados.discard(n)
            if n in self.huellas:
                self.huellas.remove(n)
            e("  Borrar (DeleteChar): codigo 0x00 = OK")
            e("Huella ID %d eliminada." % n)
        elif cmd.startswith("enrolar "):
            n = int(cmd.split()[1])
            self.cancelar = False
            e("=== Registro de huella en ID %d === (cualquier tecla + Enter cancela)" % n)
            e("Paso 1/2: APOYA el dedo sobre el lector y mantenelo quieto.")
            if not self._esperar(3):
                return e("  Cancelado por el usuario.")
            e("  Imagen capturada y procesada.")
            e("")
            e("RETIRA el dedo.")
            if not self._esperar(2.5):
                return e("  Cancelado por el usuario.")
            e("Paso 2/2: APOYA el MISMO dedo otra vez.")
            if not self._esperar(3):
                return e("  Cancelado por el usuario.")
            e("  Imagen capturada y procesada.")
            e("  Combinar lecturas (RegModel): codigo 0x00 = OK")
            e("  Guardar (Store): codigo 0x00 = OK")
            self.bloqueados.discard(n)
            self.huellas.append(n)
            e("HUELLA GUARDADA en ID %d. Retira el dedo." % n)

    def _esperar(self, seg):
        fin = time.time() + seg
        while time.time() < fin:
            if self.cancelar:
                return False
            time.sleep(0.1)
        return True


class Placa:
    def __init__(self, com, simular=False):
        self.com = com
        self.simular = simular
        self.ser = None
        self.n = 0
        self.lineas = deque(maxlen=2000)
        self.mutex = threading.Lock()
        self.cmd_lock = threading.Lock()
        self.conectada = False
        self.lector = None
        self.acceso = None
        self.huellas = None
        self.bloqueados = None        # IDs bloqueados en el lector (None = sin consultar)
        self.soporta_bloqueo = None   # None = sin saber; False = firmware viejo
        self.ignorar_hasta = 0.0      # al conectar, la placa entrega de golpe lo que pasó con el panel cerrado
        self.sim = Simulador(self._emitir) if simular else None

    # -- arranque y lectura
    def iniciar(self):
        threading.Thread(target=self._bucle, daemon=True).start()

    def _emitir(self, texto):
        for ln in texto.replace("\r", "").split("\n"):
            self._linea(ln)

    def _linea(self, ln):
        with self.mutex:
            self.n += 1
            self.lineas.append((self.n, time.time(), ln))
        try:
            self._analizar(ln)
        except Exception:  # noqa: BLE001  (un error de análisis no debe tumbar la lectura)
            pass

    def _bucle(self):
        if self.simular:
            self.conectada = True
            self.lector = True
            self.acceso = False
            self.huellas = list(self.sim.huellas)
            self.bloqueados = []
            self.soporta_bloqueo = True
            return
        import serial
        while True:
            puerto = detectar_com() if self.com == "auto" else self.com
            if not puerto:  # la placa no está enchufada: se vuelve a buscar
                self.conectada = False
                time.sleep(2)
                continue
            try:
                ser = serial.Serial()
                ser.port = puerto
                ser.baudrate = 115200
                ser.timeout = 0.2
                ser.dtr = False  # abrir el puerto no reinicia la placa
                ser.rts = False
                ser.open()
            except Exception:  # noqa: BLE001
                self.conectada = False
                time.sleep(2)
                continue
            self.ser = ser
            self.conectada = True
            self.ignorar_hasta = time.time() + 2.0  # no anotar como nuevas las entradas viejas que entrega la placa
            threading.Timer(2.2, self._consulta_inicial).start()
            buf = b""
            try:
                while True:
                    datos = ser.read(ser.in_waiting or 1)
                    if datos:
                        buf += datos
                        while b"\n" in buf:
                            ln, buf = buf.split(b"\n", 1)
                            self._linea(ln.decode("utf-8", "replace").rstrip("\r"))
            except Exception:  # noqa: BLE001
                pass
            finally:
                self.conectada = False
                self.lector = None
                self.ser = None
                try:
                    ser.close()
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(1.5)

    def _consulta_inicial(self):
        try:
            self.ejecutar("estado", [r"UART lector"], 3)
            self.ejecutar("lista", [r"Posiciones ocupadas"], 3)
            self.ejecutar("bloqueados", [r"Bloqueados \(", r"desconocido"], 3)
            reconciliar(self)
        except Exception:  # noqa: BLE001
            pass

    # -- interpretación de lo que dice la placa
    def _analizar(self, ln):
        historial = time.time() < self.ignorar_hasta
        m = re.search(r"\[ACCESO\] CONCEDIDO: ID (\d+) \(puntaje (\d+)\)", ln)
        if m:
            if not historial:
                registrar_entrada("concedido", int(m.group(1)), int(m.group(2)))
            return
        m = re.search(r"\[ACCESO\] BLOQUEADO: ID (\d+) \(puntaje (\d+)\)", ln)
        if m:
            if not historial:
                registrar_entrada("bloqueado", int(m.group(1)), int(m.group(2)))
            return
        if "[ACCESO] DENEGADO" in ln:
            if not historial:
                m = re.search(r"ID (\d+) con puntaje (\d+)", ln)
                registrar_entrada("denegado", int(m.group(1)) if m else None, int(m.group(2)) if m else None)
            return
        m = re.search(r"Bloqueados \((\d+)\):(.*)", ln)
        if m:
            self.bloqueados = [int(x) for x in re.findall(r"\d+", m.group(2))]
            self.soporta_bloqueo = True
        if "Comando desconocido: 'bloqueados'" in ln:
            self.soporta_bloqueo = False
        if "[LECTOR] DESCONECTADO" in ln or "[LECTOR] NO responde" in ln or "Lector: NO RESPONDE" in ln:
            self.lector = False
        elif "[LECTOR] volvio a responder" in ln or "[LECTOR] responde a" in ln or "Lector: CONECTADO" in ln:
            self.lector = True
        m = re.search(r"Modo acceso:? (ACTIVADO|DESACTIVADO)", ln)
        if m:
            self.acceso = m.group(1) == "ACTIVADO"
        m = re.search(r"Acceso automatico: (SI|NO)", ln)
        if m:
            self.acceso = m.group(1) == "SI"
        m = re.search(r"Posiciones ocupadas \((\d+)\):(.*)", ln)
        if m:
            self.huellas = [int(x) for x in re.findall(r"\d+", m.group(2))]

    # -- envío de comandos
    def escribir(self, texto):
        if self.simular:
            self.sim.recibir(texto.strip())
            return
        if not self.ser:
            raise IOError("La placa no está conectada por USB")
        self.ser.write((texto + "\n").encode())

    def marca(self):
        with self.mutex:
            return self.n

    def desde(self, marca):
        with self.mutex:
            return [(n, t) for (n, _ts, t) in self.lineas if n > marca]

    def esperar(self, marca, patrones, timeout):
        fin = time.time() + timeout
        while time.time() < fin:
            txt = "\n".join(t for _, t in self.desde(marca))
            if any(re.search(p, txt) for p in patrones):
                break
            time.sleep(0.1)
        return [t for _, t in self.desde(marca)]

    def ejecutar(self, cmd, patrones=(), timeout=2.0):
        if not self.cmd_lock.acquire(timeout=0.5):
            raise Ocupado("Hay otra operación en curso (por ejemplo, registrando una huella)")
        try:
            m = self.marca()
            self.escribir(cmd)
            if patrones:
                return self.esperar(m, patrones, timeout)
            time.sleep(timeout)
            return [t for _, t in self.desde(m)]
        finally:
            self.cmd_lock.release()


# ------------------------------------------------------------------ registro de huella (trabajo en segundo plano)

JOB = {"activo": False, "fase": "inactivo", "mensaje": "", "ok": None, "persona_id": None, "nombre": "", "slot": None,
       "intento": 0, "cancelar": False}


def job_set(**kw):
    JOB.update(kw)


def trabajo_enrolar(placa, persona_id, reemplazar):
    p = sql("SELECT * FROM personas WHERE id=?", (persona_id,))[0]
    nombre = p["nombre"]
    job_set(activo=True, fase="preparando", mensaje="Preparando el lector...", ok=None, persona_id=persona_id,
            nombre=nombre, slot=None, intento=0, cancelar=False)
    if not placa.cmd_lock.acquire(timeout=1):
        return job_set(activo=False, fase="error", ok=False, mensaje="Hay otra operación en curso. Intenta de nuevo.")
    try:
        if p["huella_id"] is not None:
            if not reemplazar:
                return job_set(activo=False, fase="error", ok=False, mensaje="Esa persona ya tiene una huella registrada.")
            m = placa.marca()
            placa.escribir("borrar %d" % p["huella_id"])
            placa.esperar(m, [r"eliminada", r"codigo 0x"], 3)
            sql("UPDATE personas SET huella_id=NULL, bloqueado=0 WHERE id=?", (persona_id,), escribir=True)
        m = placa.marca()
        placa.escribir("lista")
        placa.esperar(m, [r"Posiciones ocupadas"], 3)
        usados = set(placa.huellas or []) | {r["huella_id"] for r in sql("SELECT huella_id FROM personas WHERE huella_id IS NOT NULL")}
        libres = [i for i in range(1, CAPACIDAD) if i not in usados]
        if not libres:
            return job_set(activo=False, fase="error", ok=False, mensaje="El lector está lleno (150 huellas).")
        slot = libres[0]
        job_set(slot=slot)

        for intento in range(1, 4):
            job_set(intento=intento, fase="paso1", mensaje="Apoya el dedo en el lector y mantenlo quieto unos segundos")
            m = placa.marca()
            placa.escribir("enrolar %d" % slot)
            ultimo = m
            capturas = 0
            fin_intento = None
            fin_total = time.time() + 150
            while time.time() < fin_total:
                if JOB["cancelar"]:
                    placa.escribir("x")
                    JOB["cancelar"] = False
                nuevas = placa.desde(ultimo)
                for n, t in nuevas:
                    ultimo = n
                    if "Paso 1/2" in t:
                        job_set(fase="paso1", mensaje="Apoya el dedo en el lector y mantenlo quieto unos segundos")
                    elif "Imagen capturada" in t:
                        capturas += 1
                        if capturas == 1:
                            job_set(fase="retirar", mensaje="Bien. Ahora RETIRA el dedo")
                        else:
                            job_set(fase="guardando", mensaje="Guardando la huella...")
                    elif "RETIRA el dedo" in t:
                        job_set(fase="retirar", mensaje="Bien. Ahora RETIRA el dedo")
                    elif "Paso 2/2" in t:
                        job_set(fase="paso2", mensaje="Apoya el MISMO dedo otra vez, en el mismo punto")
                    elif "Imagen no util" in t:
                        job_set(mensaje="Imagen poco clara: retira el dedo y apóyalo de nuevo, plano y firme")
                    elif "HUELLA GUARDADA" in t:
                        sql("UPDATE personas SET huella_id=?, bloqueado=0 WHERE id=?", (slot, persona_id), escribir=True)
                        if placa.huellas is not None and slot not in placa.huellas:
                            placa.huellas.append(slot)
                        return job_set(activo=False, fase="ok", ok=True,
                                       mensaje="¡Listo! %s quedó registrado y habilitado para la entrada." % nombre)
                    elif "no se pudieron combinar" in t or "no coinciden lo suficiente" in t:
                        fin_intento = "combinar"
                    elif "YA esta registrada en ID" in t:
                        m2 = re.search(r"ID (\d+)", t)
                        dueno = ""
                        if m2:
                            r = sql("SELECT nombre FROM personas WHERE huella_id=?", (int(m2.group(1)),))
                            dueno = (" (%s)" % r[0]["nombre"]) if r else " (ID %s)" % m2.group(1)
                        return job_set(activo=False, fase="error", ok=False,
                                       mensaje="Esa huella ya está registrada%s. No se guardó." % dueno)
                    elif "Tiempo agotado" in t:
                        return job_set(activo=False, fase="error", ok=False,
                                       mensaje="No se detectó el dedo a tiempo. Vuelve a intentarlo.")
                    elif "Cancelado por el usuario" in t:
                        return job_set(activo=False, fase="cancelado", ok=False, mensaje="Registro cancelado.")
                    elif "no esta conectado" in t or "No se pudo guardar" in t or "posicion" in t and "ya tiene" in t:
                        return job_set(activo=False, fase="error", ok=False, mensaje="El lector no respondió. Revisa sus cables.")
                if fin_intento:
                    break
                time.sleep(0.15)
            else:
                return job_set(activo=False, fase="error", ok=False, mensaje="Se acabó el tiempo del registro.")
            if fin_intento == "combinar" and intento < 3:
                job_set(fase="reintento", mensaje="Las dos lecturas no coincidieron. Intentemos otra vez: apoya igual las dos veces")
                time.sleep(3)
                continue
            return job_set(activo=False, fase="error", ok=False,
                           mensaje="No se pudo combinar las lecturas. Limpia el vidrio, apoya el dedo plano y firme, e inténtalo de nuevo.")
    except Exception as e:  # noqa: BLE001
        job_set(activo=False, fase="error", ok=False, mensaje="Error: %s" % e)
    finally:
        placa.cmd_lock.release()
        JOB["activo"] = False


# ------------------------------------------------------------------ bloqueo / vencimientos

def aplicar_bloqueo(placa, huella_id, bloquear):
    """Pide a la placa bloquear/permitir una huella. True si la placa lo confirmó."""
    cmd = ("bloquear %d" if bloquear else "permitir %d") % huella_id
    lineas = placa.ejecutar(cmd, [r"BLOQUEADO", r"PERMITIDO", r"Uso:", r"fuera de rango", r"desconocido"], 3)
    ok = any(("BLOQUEADO" if bloquear else "PERMITIDO") in t for t in lineas)
    if ok and placa.bloqueados is not None:
        if bloquear and huella_id not in placa.bloqueados:
            placa.bloqueados.append(huella_id)
        if not bloquear and huella_id in placa.bloqueados:
            placa.bloqueados.remove(huella_id)
    if any("desconocido" in t for t in lineas):
        placa.soporta_bloqueo = False
    return ok


def reconciliar(placa):
    """La base de datos manda: bloquea a los vencidos y deja el lector igual a lo que dice la base.
    Se repite cada minuto y al conectar, así un vencimiento se aplica solo mientras el panel esté abierto."""
    if not placa.conectada or placa.soporta_bloqueo is False or placa.bloqueados is None:
        return
    hoy = date.today().isoformat()
    for p in sql("SELECT * FROM personas WHERE huella_id IS NOT NULL AND bloqueado=0 AND vence IS NOT NULL AND vence < ?", (hoy,)):
        sql("UPDATE personas SET bloqueado=1 WHERE id=?", (p["id"],), escribir=True)
    for p in sql("SELECT * FROM personas WHERE huella_id IS NOT NULL"):
        deseado = bool(p["bloqueado"])
        if deseado != (p["huella_id"] in placa.bloqueados):
            try:
                aplicar_bloqueo(placa, p["huella_id"], deseado)
            except Ocupado:
                return  # hay un registro en curso; se reintenta en un minuto
            except Exception:  # noqa: BLE001
                return


def vigilante(placa):
    while True:
        time.sleep(60)
        if not JOB["activo"]:
            try:
                reconciliar(placa)
            except Exception:  # noqa: BLE001
                pass


# ------------------------------------------------------------------ API

def estado_json(placa):
    ids_db = {r["huella_id"] for r in sql("SELECT huella_id FROM personas WHERE huella_id IS NOT NULL")}
    huerfanas = sorted(set(placa.huellas) - ids_db) if placa.huellas is not None else []
    return {"placa": placa.conectada, "lector": placa.lector, "acceso": placa.acceso, "huellas": placa.huellas,
            "huerfanas": huerfanas, "simulado": placa.simular, "ocupado": JOB["activo"],
            "soporta_bloqueo": placa.soporta_bloqueo, "bloqueados": placa.bloqueados}


def validar_persona(b, parcial=False):
    campos = {}
    if "nombre" in b or not parcial:
        nombre = (b.get("nombre") or "").strip()
        if not nombre:
            raise ValueError("Escribe el nombre de la persona")
        campos["nombre"] = nombre[:120]
    for k in ("documento", "telefono", "notas"):
        if k in b:
            campos[k] = (b.get(k) or "").strip()[:300]
    if "vence" in b:
        v = (b.get("vence") or "").strip()
        if v:
            try:
                date.fromisoformat(v)
            except ValueError:
                raise ValueError("La fecha de vencimiento no es válida") from None
            campos["vence"] = v
        else:
            campos["vence"] = None
    if "huella_id" in b and b["huella_id"] not in (None, ""):
        h = int(b["huella_id"])
        if not 0 <= h < CAPACIDAD:
            raise ValueError("ID de huella fuera de rango")
        campos["huella_id"] = h
    return campos


def r_estado(h, mt, q, b):
    return estado_json(h.placa), 200


def r_sincronizar(h, mt, q, b):
    try:
        h.placa.ejecutar("estado", [r"UART lector"], 3)
        h.placa.ejecutar("lista", [r"Posiciones ocupadas"], 3)
    except Ocupado as e:
        return {"ok": False, "mensaje": str(e)}, 409
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    return estado_json(h.placa), 200


def r_acceso(h, mt, q, b):
    on = bool(b.get("on"))
    try:
        h.placa.ejecutar("acceso on" if on else "acceso off", [r"Modo acceso"], 2.5)
    except Ocupado as e:
        return {"ok": False, "mensaje": str(e)}, 409
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    return estado_json(h.placa), 200


def r_abrir(h, mt, q, b):
    try:
        lineas = h.placa.ejecutar("rele probar", [r"\[RELE\]"], 2.5)
    except Ocupado:
        return {"ok": False, "mensaje": "Registrando una huella: espera a que termine."}, 409
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    txt = "\n".join(lineas)
    if "PULSO SIMULADO" in txt:
        return {"ok": False, "mensaje": "La placa está en modo simulación: el relé NO se mueve."}, 200
    if "PULSO de" in txt:
        registrar_entrada("manual")
        return {"ok": True, "mensaje": "¡Abierto! Pulso enviado. Pasa ahora."}, 200
    if "ignorado" in txt:
        return {"ok": False, "mensaje": "Espera un segundo y vuelve a pulsar."}, 200
    return {"ok": False, "mensaje": "La placa no respondió. Revisa el cable USB."}, 200


def r_personas_lista(h, mt, q, b):
    filas = sql("SELECT * FROM personas ORDER BY nombre COLLATE NOCASE")
    return [persona_json(p) for p in filas], 200


def r_personas_crear(h, mt, q, b):
    try:
        c = validar_persona(b)
        if c.get("documento") and doc_duplicado(c["documento"]):
            raise ValueError("Esa cédula ya está registrada en otra persona")
        if c.get("huella_id") is not None and sql("SELECT 1 FROM personas WHERE huella_id=?", (c["huella_id"],)):
            raise ValueError("Esa huella ya está asignada a otra persona")
    except (ValueError, TypeError) as e:
        return {"ok": False, "mensaje": str(e)}, 400
    c["creado"] = datetime.now().isoformat(timespec="seconds")
    cols = ",".join(c)
    pid = sql("INSERT INTO personas (%s) VALUES (%s)" % (cols, ",".join("?" * len(c))), tuple(c.values()), escribir=True)
    return {"ok": True, "id": pid}, 201


def r_personas_editar(h, mt, q, b):
    pid = int(mt.group(1))
    if not sql("SELECT 1 FROM personas WHERE id=?", (pid,)):
        return {"ok": False, "mensaje": "No existe esa persona"}, 404
    try:
        c = validar_persona(b, parcial=True)
        if c.get("documento") and doc_duplicado(c["documento"], excluir=pid):
            raise ValueError("Esa cédula ya está registrada en otra persona")
        if c.get("huella_id") is not None and sql("SELECT 1 FROM personas WHERE huella_id=? AND id<>?", (c["huella_id"], pid)):
            raise ValueError("Esa huella ya está asignada a otra persona")
    except (ValueError, TypeError) as e:
        return {"ok": False, "mensaje": str(e)}, 400
    if c:
        sql("UPDATE personas SET %s WHERE id=?" % ",".join("%s=?" % k for k in c), tuple(c.values()) + (pid,), escribir=True)
    return {"ok": True}, 200


def borrar_huella_lector(placa, n):
    lineas = placa.ejecutar("borrar %d" % n, [r"eliminada", r"ERROR", r"fuera de rango"], 4)
    return any("eliminada" in t for t in lineas)


def r_personas_borrar(h, mt, q, b):
    pid = int(mt.group(1))
    filas = sql("SELECT * FROM personas WHERE id=?", (pid,))
    if not filas:
        return {"ok": False, "mensaje": "No existe esa persona"}, 404
    p = filas[0]
    if p["huella_id"] is not None:
        try:
            if not borrar_huella_lector(h.placa, p["huella_id"]):
                return {"ok": False, "mensaje": "No se pudo borrar la huella del lector; la persona no se eliminó."}, 502
        except Ocupado as e:
            return {"ok": False, "mensaje": str(e)}, 409
        except Exception as e:  # noqa: BLE001
            return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    sql("DELETE FROM personas WHERE id=?", (pid,), escribir=True)
    if h.placa.huellas is not None and p["huella_id"] in h.placa.huellas:
        h.placa.huellas.remove(p["huella_id"])
    return {"ok": True}, 200


def r_huella_iniciar(h, mt, q, b):
    pid = int(mt.group(1))
    if not sql("SELECT 1 FROM personas WHERE id=?", (pid,)):
        return {"ok": False, "mensaje": "No existe esa persona"}, 404
    if JOB["activo"]:
        return {"ok": False, "mensaje": "Ya hay un registro en curso"}, 409
    if not h.placa.conectada:
        return {"ok": False, "mensaje": "La placa no está conectada por USB"}, 502
    if h.placa.lector is False:
        return {"ok": False, "mensaje": "El lector de huellas no responde. Revisa sus cables."}, 502
    job_set(activo=True, fase="preparando", mensaje="Preparando el lector...", ok=None, persona_id=pid)
    threading.Thread(target=trabajo_enrolar, args=(h.placa, pid, bool(b.get("reemplazar"))), daemon=True).start()
    return {"ok": True}, 202


def r_huella_quitar(h, mt, q, b):
    pid = int(mt.group(1))
    filas = sql("SELECT * FROM personas WHERE id=?", (pid,))
    if not filas or filas[0]["huella_id"] is None:
        return {"ok": False, "mensaje": "Esa persona no tiene huella registrada"}, 404
    n = filas[0]["huella_id"]
    try:
        if not borrar_huella_lector(h.placa, n):
            return {"ok": False, "mensaje": "El lector no confirmó el borrado de la huella."}, 502
    except Ocupado as e:
        return {"ok": False, "mensaje": str(e)}, 409
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    sql("UPDATE personas SET huella_id=NULL, bloqueado=0 WHERE id=?", (pid,), escribir=True)
    if h.placa.huellas is not None and n in h.placa.huellas:
        h.placa.huellas.remove(n)
    return {"ok": True}, 200


def r_bloquear(h, mt, q, b):
    pid = int(mt.group(1))
    filas = sql("SELECT * FROM personas WHERE id=?", (pid,))
    if not filas or filas[0]["huella_id"] is None:
        return {"ok": False, "mensaje": "Esa persona no tiene huella registrada"}, 404
    if h.placa.soporta_bloqueo is False:
        return {"ok": False, "mensaje": "El firmware de la placa es viejo: hay que actualizarlo para bloquear sin borrar la huella."}, 409
    sql("UPDATE personas SET bloqueado=1 WHERE id=?", (pid,), escribir=True)
    try:
        aplicado = aplicar_bloqueo(h.placa, filas[0]["huella_id"], True)
    except Exception:  # noqa: BLE001
        aplicado = False
    aviso = "" if aplicado else "Guardado, pero la placa no confirmó: se aplicará solo en menos de 1 minuto."
    return {"ok": True, "aviso": aviso}, 200


def r_renovar(h, mt, q, b):
    """Persona que volvió a pagar: nueva fecha de vencimiento y la huella vuelve a abrir (sin registrarla de nuevo)."""
    pid = int(mt.group(1))
    filas = sql("SELECT * FROM personas WHERE id=?", (pid,))
    if not filas:
        return {"ok": False, "mensaje": "No existe esa persona"}, 404
    try:
        c = validar_persona({"vence": b.get("vence")}, parcial=True)
    except ValueError as e:
        return {"ok": False, "mensaje": str(e)}, 400
    nueva = c.get("vence")
    if nueva and nueva < date.today().isoformat():
        return {"ok": False, "mensaje": "La nueva fecha ya pasó: elige una fecha de hoy en adelante."}, 400
    sql("UPDATE personas SET vence=?, bloqueado=0 WHERE id=?", (nueva, pid), escribir=True)
    huella = filas[0]["huella_id"]
    if huella is None:
        return {"ok": True, "aviso": "Renovado. Falta registrar su huella."}, 200
    if h.placa.soporta_bloqueo is False:
        return {"ok": True, "aviso": "Renovado en la base, pero el firmware es viejo y no puede bloquear/permitir."}, 200
    try:
        aplicado = aplicar_bloqueo(h.placa, huella, False)
    except Exception:  # noqa: BLE001
        aplicado = False
    aviso = "" if aplicado else "Guardado, pero la placa no confirmó: se aplicará solo en menos de 1 minuto."
    return {"ok": True, "aviso": aviso}, 200


def r_huerfana_borrar(h, mt, q, b):
    n = int(mt.group(1))
    if sql("SELECT 1 FROM personas WHERE huella_id=?", (n,)):
        return {"ok": False, "mensaje": "Esa huella pertenece a una persona"}, 409
    try:
        if not borrar_huella_lector(h.placa, n):
            return {"ok": False, "mensaje": "El lector no confirmó el borrado."}, 502
    except Ocupado as e:
        return {"ok": False, "mensaje": str(e)}, 409
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo hablar con la placa: %s" % e}, 502
    if h.placa.huellas is not None and n in h.placa.huellas:
        h.placa.huellas.remove(n)
    return {"ok": True}, 200


# ------------------------------------------------------------------ ingreso por cédula

INGRESO = {"fallos": 0, "bloqueado_hasta": 0.0, "ultimo": {}}  # fallos seguidos y última apertura por persona


def solo_digitos(s):
    return re.sub(r"\D", "", s or "")


def doc_duplicado(doc, excluir=None):
    d = solo_digitos(doc)
    if not d:
        return False
    return any(p["id"] != excluir and solo_digitos(p["documento"]) == d for p in sql("SELECT id, documento FROM personas"))


def registrar_evento(resultado, persona_id=None):
    sql("INSERT INTO entradas (ts, huella_id, persona_id, resultado, puntaje) VALUES (?,?,?,?,?)",
        (datetime.now().isoformat(timespec="seconds"), None, persona_id, resultado, None), escribir=True)


def r_ingreso(h, mt, q, b):
    """Ingreso por cédula: abre el molinete si la persona existe, no está bloqueada y no está vencida."""
    ahora = time.time()
    if ahora < INGRESO["bloqueado_hasta"]:
        return {"ok": False, "mensaje": "Demasiados intentos. Espera %d s." % (int(INGRESO["bloqueado_hasta"] - ahora) + 1)}, 429
    ced = solo_digitos(str(b.get("cedula") or ""))
    if not 4 <= len(ced) <= 15:
        return {"ok": False, "mensaje": "Escribe tu número de cédula."}, 400
    persona = next((p for p in sql("SELECT * FROM personas") if solo_digitos(p["documento"]) == ced), None)
    hoy = date.today().isoformat()
    valido = persona is not None and not persona["bloqueado"] and not (persona["vence"] and persona["vence"] < hoy)
    if not valido:
        INGRESO["fallos"] += 1
        if INGRESO["fallos"] >= 5:  # frena a quien prueba números al azar
            INGRESO["fallos"] = 0
            INGRESO["bloqueado_hasta"] = ahora + 30
        registrar_evento("cedula_denegada", persona["id"] if persona else None)
        return {"ok": False, "mensaje": "No se pudo ingresar. Consulta en recepción."}, 200
    if ahora - INGRESO["ultimo"].get(persona["id"], 0) < 8:
        return {"ok": False, "mensaje": "Ya se abrió hace un momento. Espera unos segundos."}, 200
    try:
        lineas = h.placa.ejecutar("rele probar", [r"\[RELE\]"], 2.5)
    except Ocupado:
        return {"ok": False, "mensaje": "El sistema está ocupado. Intenta de nuevo."}, 200
    except Exception:  # noqa: BLE001
        return {"ok": False, "mensaje": "No se pudo abrir. Avisa en recepción."}, 200
    txt = "\n".join(lineas)
    if "PULSO de" not in txt or "SIMULADO" in txt:
        return {"ok": False, "mensaje": "No se pudo abrir. Avisa en recepción."}, 200
    INGRESO["fallos"] = 0
    INGRESO["ultimo"][persona["id"]] = ahora
    registrar_evento("cedula", persona["id"])
    return {"ok": True, "nombre": persona["nombre"].split()[0], "mensaje": "¡Bienvenido!"}, 200


# ------------------------------------------------------------------ productos y ventas (caja)

def _num(v, nombre, minimo=0.0, maximo=1e9, estricto=False):
    try:
        x = float(v)
    except (TypeError, ValueError):
        raise ValueError("%s no es un número válido" % nombre) from None
    if x != x or x > maximo or x < minimo or (estricto and x <= minimo):
        raise ValueError("%s fuera de rango" % nombre)
    return round(x, 2)


def _fecha(v, defecto=None):
    v = (v or "").strip() if isinstance(v, str) or v is None else str(v)
    if not v:
        return defecto
    try:
        return date.fromisoformat(v).isoformat()
    except ValueError:
        raise ValueError("La fecha no es válida") from None


def producto_json(p):
    return {"id": p["id"], "nombre": p["nombre"], "precio": p["precio"], "costo": p["costo"]}


def r_productos_lista(h, mt, q, b):
    return [producto_json(p) for p in sql("SELECT * FROM productos WHERE activo=1 ORDER BY nombre COLLATE NOCASE")], 200


def r_productos_crear(h, mt, q, b):
    try:
        nombre = (b.get("nombre") or "").strip()[:80]
        if not nombre:
            raise ValueError("Escribe el nombre del producto")
        precio = _num(b.get("precio", 0), "El precio")
        costo = _num(b.get("costo", 0), "El costo")
    except ValueError as e:
        return {"ok": False, "mensaje": str(e)}, 400
    previo = sql("SELECT * FROM productos WHERE nombre=?", (nombre,))
    if previo and previo[0]["activo"]:
        return {"ok": False, "mensaje": "Ya existe un producto con ese nombre"}, 409
    if previo:  # estaba borrado: se reactiva con los datos nuevos
        sql("UPDATE productos SET activo=1, precio=?, costo=? WHERE id=?", (precio, costo, previo[0]["id"]), escribir=True)
        return {"ok": True, "id": previo[0]["id"]}, 201
    pid = sql("INSERT INTO productos (nombre, precio, costo) VALUES (?,?,?)", (nombre, precio, costo), escribir=True)
    return {"ok": True, "id": pid}, 201


def r_productos_editar(h, mt, q, b):
    pid = int(mt.group(1))
    if not sql("SELECT 1 FROM productos WHERE id=?", (pid,)):
        return {"ok": False, "mensaje": "No existe ese producto"}, 404
    try:
        campos = {}
        if "nombre" in b:
            nombre = (b.get("nombre") or "").strip()[:80]
            if not nombre:
                raise ValueError("Escribe el nombre del producto")
            if sql("SELECT 1 FROM productos WHERE nombre=? AND id<>? AND activo=1", (nombre, pid)):
                raise ValueError("Ya existe un producto con ese nombre")
            campos["nombre"] = nombre
        if "precio" in b:
            campos["precio"] = _num(b.get("precio"), "El precio")
        if "costo" in b:
            campos["costo"] = _num(b.get("costo"), "El costo")
    except ValueError as e:
        return {"ok": False, "mensaje": str(e)}, 400
    if campos:
        sql("UPDATE productos SET %s WHERE id=?" % ",".join("%s=?" % k for k in campos), tuple(campos.values()) + (pid,), escribir=True)
    return {"ok": True}, 200


def r_productos_borrar(h, mt, q, b):
    sql("UPDATE productos SET activo=0 WHERE id=?", (int(mt.group(1)),), escribir=True)
    return {"ok": True}, 200


def venta_json(v):
    d = {k: v[k] for k in ("id", "ts", "fecha", "producto_id", "concepto", "cantidad", "precio", "costo", "persona_id", "notas")}
    d["total"] = round(v["cantidad"] * v["precio"], 2)
    d["ganancia"] = round(v["cantidad"] * (v["precio"] - v["costo"]), 2)
    d["persona"] = v["persona"] if "persona" in v.keys() else None
    return d


def _rango(q):
    hoy = date.today().isoformat()
    desde = _fecha((q.get("desde") or [""])[0], hoy)
    hasta = _fecha((q.get("hasta") or [""])[0], desde)
    if hasta < desde:
        desde, hasta = hasta, desde
    return desde, hasta


def r_ventas_lista(h, mt, q, b):
    try:
        desde, hasta = _rango(q)
        lim = max(1, min(5000, int((q.get("limite") or ["1000"])[0])))
    except ValueError as e:
        return {"ok": False, "mensaje": str(e)}, 400
    filas = sql("""SELECT v.*, p.nombre AS persona FROM ventas v LEFT JOIN personas p ON p.id = v.persona_id
                   WHERE v.fecha BETWEEN ? AND ? ORDER BY v.ts DESC, v.id DESC LIMIT ?""", (desde, hasta, lim))
    return [venta_json(v) for v in filas], 200


def r_ventas_resumen(h, mt, q, b):
    try:
        desde, hasta = _rango(q)
    except ValueError as e:
        return {"ok": False, "mensaje": str(e)}, 400
    filas = sql("SELECT * FROM ventas WHERE fecha BETWEEN ? AND ?", (desde, hasta))
    vendido = costo = unidades = 0.0
    por_producto, por_dia = {}, {}
    for f in filas:
        tot, cos = f["cantidad"] * f["precio"], f["cantidad"] * f["costo"]
        vendido += tot
        costo += cos
        unidades += f["cantidad"]
        pp = por_producto.setdefault(f["concepto"], {"concepto": f["concepto"], "cantidad": 0.0, "vendido": 0.0, "ganancia": 0.0})
        pp["cantidad"] += f["cantidad"]
        pp["vendido"] += tot
        pp["ganancia"] += tot - cos
        pd = por_dia.setdefault(f["fecha"], {"n": 0, "total": 0.0})
        pd["n"] += 1
        pd["total"] += tot
    lista = sorted(por_producto.values(), key=lambda x: -x["vendido"])
    for x in lista:
        for k in ("cantidad", "vendido", "ganancia"):
            x[k] = round(x[k], 2)
    for x in por_dia.values():
        x["total"] = round(x["total"], 2)
    return {"desde": desde, "hasta": hasta, "ventas": len(filas), "unidades": round(unidades, 2), "vendido": round(vendido, 2),
            "costo": round(costo, 2), "ganancia": round(vendido - costo, 2), "por_producto": lista, "por_dia": por_dia}, 200


def _datos_venta(b, parcial=False, previo=None):
    campos = {}
    if not parcial or "cantidad" in b:
        campos["cantidad"] = _num(b.get("cantidad"), "La cantidad", 0, 100000, estricto=True)
    if not parcial or "precio" in b:
        campos["precio"] = _num(b.get("precio"), "El precio")
    if "costo" in b:
        campos["costo"] = _num(b.get("costo"), "El costo")
    if "notas" in b:
        campos["notas"] = (b.get("notas") or "").strip()[:300]
    if "persona_id" in b:
        pid = b.get("persona_id")
        if pid in (None, ""):
            campos["persona_id"] = None
        else:
            pid = int(pid)
            if not sql("SELECT 1 FROM personas WHERE id=?", (pid,)):
                raise ValueError("Esa persona no existe")
            campos["persona_id"] = pid
    if "fecha" in b:
        f = _fecha(b.get("fecha"), date.today().isoformat())
        if f > date.today().isoformat():
            raise ValueError("La fecha no puede ser futura")
        campos["fecha"] = f
    return campos


def r_ventas_crear(h, mt, q, b):
    try:
        c = _datos_venta(b)
        prod = None
        if b.get("producto_id") not in (None, ""):
            r = sql("SELECT * FROM productos WHERE id=?", (int(b["producto_id"]),))
            if not r:
                raise ValueError("Ese producto no existe")
            prod = r[0]
        concepto = (b.get("concepto") or (prod["nombre"] if prod else "")).strip()[:80]
        if not concepto:
            raise ValueError("Elige un producto o escribe qué se vendió")
        if "costo" not in c:
            c["costo"] = prod["costo"] if prod else 0.0
        hoy = date.today().isoformat()
        fecha = c.get("fecha", hoy)
    except (ValueError, TypeError) as e:
        return {"ok": False, "mensaje": str(e)}, 400
    ts = datetime.now().isoformat(timespec="seconds") if fecha == hoy else fecha + "T12:00:00"
    vid = sql("""INSERT INTO ventas (ts, fecha, producto_id, concepto, cantidad, precio, costo, persona_id, notas)
                 VALUES (?,?,?,?,?,?,?,?,?)""",
              (ts, fecha, prod["id"] if prod else None, concepto, c["cantidad"], c["precio"], c["costo"],
               c.get("persona_id"), c.get("notas", "")), escribir=True)
    return {"ok": True, "id": vid}, 201


def r_ventas_editar(h, mt, q, b):
    vid = int(mt.group(1))
    if not sql("SELECT 1 FROM ventas WHERE id=?", (vid,)):
        return {"ok": False, "mensaje": "No existe esa venta"}, 404
    try:
        c = _datos_venta(b, parcial=True)
    except (ValueError, TypeError) as e:
        return {"ok": False, "mensaje": str(e)}, 400
    if "fecha" in c:
        c["ts"] = c["fecha"] + "T12:00:00"
    if c:
        sql("UPDATE ventas SET %s WHERE id=?" % ",".join("%s=?" % k for k in c), tuple(c.values()) + (vid,), escribir=True)
    return {"ok": True}, 200


def r_ventas_borrar(h, mt, q, b):
    sql("DELETE FROM ventas WHERE id=?", (int(mt.group(1)),), escribir=True)
    return {"ok": True}, 200


def r_enrolar_estado(h, mt, q, b):
    return dict(JOB), 200


def r_enrolar_cancelar(h, mt, q, b):
    if JOB["activo"]:
        JOB["cancelar"] = True
    return {"ok": True}, 200


def r_entradas(h, mt, q, b):
    try:
        lim = max(1, min(200, int(q.get("limite", ["40"])[0])))
    except ValueError:
        lim = 40
    filas = sql("""SELECT e.id, e.ts, e.huella_id, e.resultado, e.puntaje, p.nombre
                   FROM entradas e LEFT JOIN personas p ON p.id = e.persona_id
                   ORDER BY e.id DESC LIMIT ?""", (lim,))
    return [dict(f) for f in filas], 200


RUTAS = [
    ("GET", r"/api/estado", r_estado),
    ("POST", r"/api/sincronizar", r_sincronizar),
    ("POST", r"/api/acceso", r_acceso),
    ("POST", r"/api/abrir", r_abrir),
    ("GET", r"/api/personas", r_personas_lista),
    ("POST", r"/api/personas", r_personas_crear),
    ("PUT", r"/api/personas/(\d+)", r_personas_editar),
    ("DELETE", r"/api/personas/(\d+)", r_personas_borrar),
    ("POST", r"/api/personas/(\d+)/huella", r_huella_iniciar),
    ("DELETE", r"/api/personas/(\d+)/huella", r_huella_quitar),
    ("DELETE", r"/api/huellas/(\d+)", r_huerfana_borrar),
    ("POST", r"/api/personas/(\d+)/bloquear", r_bloquear),
    ("POST", r"/api/personas/(\d+)/renovar", r_renovar),
    ("GET", r"/api/enrolar", r_enrolar_estado),
    ("POST", r"/api/enrolar/cancelar", r_enrolar_cancelar),
    ("GET", r"/api/entradas", r_entradas),
    ("POST", r"/api/ingreso", r_ingreso),
    ("GET", r"/api/productos", r_productos_lista),
    ("POST", r"/api/productos", r_productos_crear),
    ("PUT", r"/api/productos/(\d+)", r_productos_editar),
    ("DELETE", r"/api/productos/(\d+)", r_productos_borrar),
    ("GET", r"/api/ventas", r_ventas_lista),
    ("POST", r"/api/ventas", r_ventas_crear),
    ("GET", r"/api/ventas/resumen", r_ventas_resumen),
    ("PUT", r"/api/ventas/(\d+)", r_ventas_editar),
    ("DELETE", r"/api/ventas/(\d+)", r_ventas_borrar),
]


class Handler(BaseHTTPRequestHandler):
    placa = None

    def log_message(self, fmt, *args):
        pass

    def _enviar(self, cuerpo, tipo, codigo=200):
        self.send_response(codigo)
        self.send_header("Content-Type", tipo)
        self.send_header("Content-Length", str(len(cuerpo)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(cuerpo)

    def _json(self, obj, codigo=200):
        self._enviar(json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8", codigo)

    def _despachar(self, metodo):
        host = (self.headers.get("Host") or "").lower()
        if host not in ("127.0.0.1:%d" % PUERTO_HTTP, "localhost:%d" % PUERTO_HTTP):
            return self._json({"ok": False, "mensaje": "Host no permitido"}, 403)
        u = urlparse(self.path)
        if metodo == "GET" and u.path == "/":
            with open(os.path.join(AQUI, "dashboard.html"), "rb") as f:
                return self._enviar(f.read(), "text/html; charset=utf-8")
        if metodo != "GET" and self.headers.get("X-Panel") != "1":
            return self._json({"ok": False, "mensaje": "Solicitud no permitida"}, 403)
        cuerpo = {}
        if metodo in ("POST", "PUT"):
            n = int(self.headers.get("Content-Length") or 0)
            if n:
                try:
                    cuerpo = json.loads(self.rfile.read(n) or b"{}")
                except ValueError:
                    return self._json({"ok": False, "mensaje": "JSON inválido"}, 400)
        for m, patron, fn in RUTAS:
            mt = re.fullmatch(patron, u.path)
            if m == metodo and mt:
                try:
                    obj, codigo = fn(self, mt, parse_qs(u.query), cuerpo)
                except Exception as e:  # noqa: BLE001
                    return self._json({"ok": False, "mensaje": "Error interno: %s" % e}, 500)
                return self._json(obj, codigo)
        self._json({"ok": False, "mensaje": "Ruta desconocida"}, 404)

    def do_GET(self):
        self._despachar("GET")

    def do_POST(self):
        self._despachar("POST")

    def do_PUT(self):
        self._despachar("PUT")

    def do_DELETE(self):
        self._despachar("DELETE")


def main():
    global PUERTO_HTTP, DB_PATH
    ap = argparse.ArgumentParser()
    ap.add_argument("--com", default="auto", help='puerto de la placa (por defecto "auto": la busca sola por su VID/PID)')
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--simular", action="store_true")
    ap.add_argument("--db", help="ruta de la base de datos (por defecto data/molinete.db)")
    a = ap.parse_args()
    PUERTO_HTTP = a.port
    if a.db:
        DB_PATH = os.path.abspath(a.db)
    init_db()
    placa = Placa(a.com, simular=a.simular)
    placa.iniciar()
    threading.Thread(target=vigilante, args=(placa,), daemon=True).start()
    if a.simular:
        threading.Timer(1.0, reconciliar, args=(placa,)).start()
    Handler.placa = placa
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("Panel en http://127.0.0.1:%d  (placa: %s). Ctrl+C para cerrar." % (a.port, "SIMULADA" if a.simular else a.com), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
