# Respaldo de la flash original

`flash_original_44bd8d244518.bin` (4 MB, no se sube al repositorio) es la lectura completa
de la flash de la placa antes de grabar el primer firmware (2026-10-01).

Contenido: flash vacía (todo 0xFF), salvo 32 bytes con el patrón `5A 00 FF A5` en 0x50000
(marca de prueba de fábrica). No había bootloader ni programa.

Para restaurarlo (borra el firmware actual):
```
pio pkg exec --package tool-esptoolpy -- esptool.py --chip esp32c3 --port COM10 write_flash 0 backup/flash_original_44bd8d244518.bin
```
