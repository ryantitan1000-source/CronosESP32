// Puente transparente USB <-> lector DY50 (solo diagnóstico).
// UART1: RX = GPIO20 (TX del lector), TX = GPIO21 (RX del lector), 57600 8N1.
// GPIO4 (relé vía transistor) se deja en reposo (bajo): el molinete NO se activa.
#include <Arduino.h>

void setup() {
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  Serial.setRxBufferSize(2048);
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  Serial1.setRxBufferSize(4096);
  Serial1.begin(57600, SERIAL_8N1, 20, 21);
}

void loop() {
  uint8_t buf[256];
  int n = Serial.available();
  if (n > 0) {
    if (n > static_cast<int>(sizeof buf)) n = sizeof buf;
    n = Serial.readBytes(buf, n);
    Serial1.write(buf, n);
  }
  n = Serial1.available();
  if (n > 0) {
    if (n > static_cast<int>(sizeof buf)) n = sizeof buf;
    n = Serial1.readBytes(buf, n);
    Serial.write(buf, n);
  }
}
