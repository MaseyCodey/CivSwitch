# Prebuilt firmware

Snapshot of `esp32-usb-pad` built from the `PicoSwitch/payload.txt` in this
commit. The payload is compiled in; if you change `payload.txt`, rebuild.

| File | Use |
|---|---|
| `esp32-usb-pad-full.bin` | bootloader + partition table + app, flash at `0x0` |
| `esp32-usb-pad-ota.bin` | app only, flash at `0x10000` or upload via OTA |

```sh
# USB, full image
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 esp32-usb-pad-full.bin
```

Press **RESET** on the board afterwards.

```sh
# Wi-Fi OTA (board already running the Wi-Fi firmware)
python3 ../tools/ota.py <board-ip> esp32-usb-pad-ota.bin
```

Build metadata:

| Item | Value |
|---|---|
| Built | 2026-11-04 |
| ESP-IDF | v6.1 |
| App size | 1,354,144 bytes (0x14a9a0) |
| MD5 (ota) | `72c336d5327e755a24b668bd6c1b1057` |
| MD5 (full) | `d927c5c6b00eb4fe43fbcd2f11533131` |
