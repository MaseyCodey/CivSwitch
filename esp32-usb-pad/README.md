# esp32-usb-pad — ESP32-S3 USB HID pad for CivSwitch

USB composite HID device (**gamepad + boot-protocol keyboard**) that types the
CivSwitch chat-XSS payload into the Switch 2, plus a **Wi-Fi control surface** so
you can drive it from a browser or a script instead of re-flashing macros.

Same descriptors and same `payload.txt` as `PicoSwitch`, just a different MCU.

```
ESP32-S3 native USB  ──►  Switch 2 dock / USB-C adapter
        (composite HID: gamepad + keyboard)

   PC / phone ── Wi-Fi ──► HTTP :80  (web UI, JSON API, OTA)
                      └──► TCP  :9060 (line commands)
```

Why it exists:

- **No Pico required.** ESP32-S3 boards are cheap and everywhere.
- **Gamepad A is real.** CH9329-style adapters only emulate a keyboard/mouse;
  the game needs a controller to open the in-game chat.
- **One USB cable.** Interface 0 is the gamepad, interface 1 is the keyboard.
- **No re-flash loop.** Buttons, typing and the macro are triggered over Wi-Fi;
  firmware updates are OTA.

## Requirements

| Item | Value |
|---|---|
| MCU | ESP32-S3 (needs native USB-OTG; ESP32 classic is **not** supported) |
| Toolchain | ESP-IDF **v6.1** |
| Flash | 16 MB board assumed (4 MB app slots); see `partitions.csv` |
| Board | any ESP32-S3 devkit with the native USB port exposed |

The firmware uses `esp_tinyusb` (managed component, fetched automatically).

## Build

```sh
export IDF_PATH=/path/to/esp-idf
export IDF_TOOLS_PATH=/path/to/.espressif
source "$IDF_PATH/export.sh"

cd esp32-usb-pad
idf.py set-target esp32s3      # only once
idf.py build
```

`payload.txt` is **not** committed; it is produced by the chat transport:

```sh
cd ../PicoSwitch
npm install          # once
node compress.js     # writes PicoSwitch/payload.txt from ../core.js
```

The build embeds it into `macro_script.h` automatically. Each payload line is
`Esc`, `A`, optional `Alt+Space` (JP IME), the line, `Enter`, `Enter`.

## Prebuilt firmware

`firmware/` holds a snapshot built from the current `PicoSwitch/payload.txt`:

| File | Use |
|---|---|
| `esp32-usb-pad-full.bin` | full image (bootloader + partition table + app) — flash at `0x0` over USB |
| `esp32-usb-pad-ota.bin` | app only — OTA (`tools/ota.py`) or write at `0x10000` |

Full USB flash, no toolchain needed:

```sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 firmware/esp32-usb-pad-full.bin
```

Then press **RESET** on the board.

Use the prebuilt image when you just want the current macro. **If you change
`payload.txt` (or `core.js`, which generates it), the payload must be rebuilt
into the firmware** — it is compiled in, not read at runtime:

```sh
cd PicoSwitch && node compress.js
cd ../esp32-usb-pad && idf.py build
```

Then flash over USB as above, or update a running board over Wi-Fi:

```sh
python3 tools/ota.py <board-ip> build/esp32-usb-pad.bin
```

## Flash (first time / recovery)

Put the board into download mode **before** it enumerates:

1. Hold **BOOT**.
2. Plug the board's **native USB port** (not the UART/CH340 port) into the PC.
3. Release **BOOT**.

```sh
idf.py -p /dev/cu.usbmodemXXXX flash
```

`idf.py flash` skips regions that already match. After changing the partition
table or flash size, force a full write once:

```sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32-usb-pad.bin
```

**Press RESET after flashing.** `esptool`'s reset can leave the chip in ROM
download mode (`boot:0x21 (DOWNLOAD)`) — a physical reset boots the app.

## OTA (normal updates)

Two 4 MB app slots (`ota_0` / `ota_1`); an upload goes to the inactive slot.

```sh
python3 tools/ota.py 10.2.0.240 build/esp32-usb-pad.bin
```

The web UI has the same thing under **Firmware update (OTA)** with a progress
bar. `/status` reports the booted slot. Rollback is enabled: the app calls
`esp_ota_mark_app_valid_cancel_rollback()` at the end of `app_main`, so an image
that panics before that is reverted on the next reset.

No USB cable or BOOT button needed for normal iterations.

## Wi-Fi

First boot (no saved credentials) starts a setup AP:

```text
SSID:     ESPNETPAD
Password: netpad12345
URL:      http://192.168.4.1
```

Join it from a phone, open the page, pick a network from the scan list, enter the
password, **Save & reboot**. The board joins the LAN and keeps the setup AP as a
recovery path, so `http://192.168.4.1` keeps working.

The Wi-Fi card on the page is collapsed when the board is already connected, and
opens + scans automatically when it is unconfigured.

ESP32-S3 is **2.4 GHz only**.

## Web UI

`http://<board-ip>/` — first row is **status + Wi-Fi**, the **virtual pad takes a
full row** (shoulders, d-pad, ABXY, minus/plus/home/capture/L3/R3, adjustable
hold time), then the Civ7 macro card, a text box, OTA, and a log pane.

The Wi-Fi card always shows the connected SSID, IP and signal strength
(`rssi`); **Change** expands the scan list and credential form. It opens and
scans by itself only when the board has no saved network.

**BOOT button:** hold it for **2 seconds** while the board is running to launch
the macro immediately in **international** mode, without opening this page. The
saved region setting is not changed.

English by default; the **中文 / EN** button switches and the choice is stored in
NVS.

## HTTP API

| method | path | notes |
|---|---|---|
| GET  | `/`           | control page (embedded `main/web/index.html`) |
| GET  | `/status`     | JSON status |
| GET  | `/wifi/scan`  | nearby 2.4 GHz APs |
| POST | `/ota`        | firmware `.bin`, then reboot |
| POST | `/cmd`        | one or more command lines |

```sh
curl -s http://10.2.0.240/status
curl -s -X POST --data 'PRESS a 80 1' http://10.2.0.240/cmd
curl -s -X POST --data 'TYPE 1 hello world' http://10.2.0.240/cmd
```

`/status`:

```json
{"mounted":true,"busy":false,"ip":"10.2.0.240","ap":"192.168.4.1",
 "ssid":"R-SH02","rssi":-49,"uptime":42,"slot":"ota_0","lang":"en","region":"jp"}
```

## TCP API

Port **9060**, one command per line, reply ends with `END`. Text needs no URL
encoding, so it is easier to drive from scripts:

```sh
printf 'PRESS home 80 1\n' | nc 10.2.0.240 9060
```

## Commands

| command | description |
|---|---|
| `STATUS` | `mounted=… busy=… ip=… rssi=… slot=… lang=… region=…` |
| `STOP` | stop the running macro after the current step |
| `BOOTSTRAP [jp\|intl]` | run the Civ7 macro; the region is saved |
| `SET REGION jp\|intl` | console region for the macro (persisted) |
| `SET LANG en\|zh` | web UI language (persisted) |
| `PRESS <btns> [hold_ms] [count]` | press buttons, default 60 ms × 1 |
| `TYPE <enter 0\|1> <text>` | type printable ASCII, `\n` = Enter |
| `WIFI <ssid> <pass>` | save credentials and reboot |
| `REBOOT` | reboot |
| `HELP` | command list |

`<btns>` is `+`-separated, e.g. `a+up`, `l+r`, `home`.

Button names: `a b x y l r zl zr minus plus l3 r3 home capture`
Directions: `up down left right` (combined into a hat/diagonal).

## Console region (JP IME)

A JP-only console keeps the chat input in Japanese IME mode, so raw ASCII opens
the candidate window and the payload arrives as kana. The macro therefore emits
`Alt+Space` (toggle to direct input) **before every line**.

That step is a runtime decision now, not a build flag:

- **JP-only console** (default) — `Alt+Space` is sent per line;
- **International console** — those steps are skipped (~20 s faster per run).

Pick it in the macro card's dropdown or `BOOTSTRAP intl`. The choice is stored in
NVS. The payload itself is identical for both; only the IME toggle differs.

## Flags

| Flag | Default | Meaning |
|---|---:|---|
| `MACRO_REPEAT` | `0` | Repeat the macro forever at boot (standalone use, no network). |
| `MACRO_REPEAT_DELAY_MS` | `5000` | Delay between rounds when repeating. |
| `PAYLOAD_FILE` | `../PicoSwitch/payload.txt` | Payload to embed. |
| `NETPAD_SKIP_USB` | `0` | Skip TinyUSB (debug console build). |

```sh
idf.py -DMACRO_REPEAT=1 build      # standalone, runs the macro at boot
```

## How it maps to PicoSwitch

| | PicoSwitch | esp32-usb-pad |
|---|---|---|
| MCU | RP2040 / RP2350 | ESP32-S3 |
| SDK | Pico SDK + TinyUSB | ESP-IDF + esp_tinyusb |
| HID layout | gamepad + keyboard | same descriptors, copied from `PicoSwitch/include/SwitchConsts.h` |
| Payload source | `PicoSwitch/payload.txt` | same file |
| Macro generator | `tools/payload_to_macro.py` | `tools/payload_to_esp32.py` |
| Trigger | repeats at boot | Wi-Fi API / web UI / OTA |
| VID/PID | `0x0f0d:0x0092` | same |

USB report layout:

- gamepad: 8 bytes (`buttons` lo/hi, hat, LX, LY, RX, RY, vendor)
- keyboard: 8-byte boot report (modifier, reserved, 6 keycodes)

## Tools

| Script | Purpose |
|---|---|
| `tools/netpad.py` | send commands (`press`, `type`, `macro`, `status`) |
| `tools/ota.py` | upload firmware over Wi-Fi |
| `tools/serial_log.py` | append the USB-Serial/JTAG console to a file |
| `tools/payload_to_esp32.py` | `payload.txt` → `macro_script.h` |

## Tested

- ESP32-S3, ESP-IDF v6.1, composite descriptor enumerates on Switch 2.
- Full 87-line payload typed into Civ7 (JP 20.4.0), Remote JS Loader on `:9026`.
- D-pad left/right (HAT `0x02` / `0x06`) verified on Switch 2 hardware.
- OTA `ota_0` ↔ `ota_1` verified over Wi-Fi.
- BOOT hold 2 s (GPIO0) launches the macro in international mode.
- Wi-Fi signal display (`rssi`) from `/status`.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Types as garbage / IME candidates | console is JP-only but region is `intl` (no `Alt+Space`) |
| Chat never opens | the 5 × A preamble missed the window; check the game is past the main menu |
| Nothing happens at all | board enumerated as UART only — use the native USB port |
| Board not detected for flashing | hold BOOT, tap RESET, release BOOT |
| Chip stays in `DOWNLOAD(USB/UART0)` after flashing | press RESET (esptool reset cannot clear the strap) |
| OTA upload times out | AP+STA share the radio; just retry (`tools/ota.py` retries once) |

## Notes

- Not a Bluetooth controller: this is USB only. Use `PicoSwitch` for wireless.
- No rumble, motion or NFC; only the buttons/sticks needed by the macro.
- CH9329 and other keyboard-only adapters **cannot** run this payload.
