# esp32-usb-pad — ESP32-S3 replacement for PicoSwitch

USB composite HID device (gamepad + boot-protocol keyboard) that types the
CivSwitch chat-XSS payload into the Switch 2, exactly like `PicoSwitch` does on
a Raspberry Pi Pico. Same descriptors, same `payload.txt`, same workflow — just
a different MCU.

Why it exists:

- **No Pico required.** ESP32-S3 boards are cheap and everywhere.
- **Gamepad A is real.** CH9329-style adapters only emulate a keyboard/mouse;
  the game needs a controller to open the in-game chat, so a pure keyboard
  adapter cannot run the payload.
- **One USB cable.** Interface 0 is the gamepad, interface 1 is the keyboard;
  both are presented by the board's single native USB port.

```
ESP32-S3 native USB  ──►  Switch 2 dock / USB-C adapter
        (composite HID: gamepad + keyboard)
```

## Requirements

| Item | Value |
|---|---|
| MCU | ESP32-S3 (needs native USB-OTG; ESP32 classic is **not** supported) |
| Toolchain | ESP-IDF **v6.1** |
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

The build embeds it into `macro_script.h` automatically:

```
-- Generating macro_script.h from payload.txt
87 batches, ... lines -> .../esp32-usb-pad/build/esp32-usb-pad/macro_script.h
```

Output:

```text
build/esp32-usb-pad.bin
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
```

## Flash

Put the board into download mode **before** it enumerates:

1. Hold **BOOT**.
2. Plug the board's **native USB port** (not the UART/CH340 port) into the PC.
3. Release **BOOT**.

Then:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash
```

Hold-BOOT-while-plugging is the verified sequence on this board.  If you skip the BOOT hold, the firmware
boots normally and immediately starts typing the macro into the PC (the macro
runs as soon as USB enumerates, on any host), and `idf.py` will not see a port.

If no `/dev/cu.usbmodemXXXX` appears, try another USB cable or port, or check board documentation; the chip is only visible to `idf.py` while in download mode.

For a single-file image (same download mode):

```sh
esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  write-flash 0x0 build/esp32-usb-pad_flashed.bin
```

After flashing, unplug the board from the PC and connect it to the console —
it will boot straight into the macro.

## Usage

1. If your console allows it: **System Settings → System → Language** =
   English (US). A JP-only console cannot change this: build with
   `-DLANG_JP=1` (auto-toggles the JP IME), then continue with step 2.
2. **System Settings → Controllers and Accessories → USB Keyboard** =
   English (US). 
3. Plug the board into the console.
4. Start Civilization 7, load any save, open the in-game chat.
5. The macro starts ~2 s after USB enumeration:
   - 5 × **A** to focus the chat input;
   - for each of the 87 payload lines: `Esc`, `A`, optional `Alt+Space`
     (JP IME), type the line, `Enter`, `Enter`;
   - repeat every 5 s until the Remote JS Loader is up on port 9026.

Then use `send_js.py` / `installer-creator` from the repo root as usual.

Unplug the board once the loader is running — otherwise the next round types
into the game again.

## Flags

All are CMake cache values, so they can be set per build without editing code:

| Flag | Default | Meaning |
|---|---:|---|
| `LANG_JP` | `1` | **JP consoles only.** Emit `Alt+Space` before every payload line (toggle the Japanese IME to direct English input). Set `0` on an English-locale console — it saves 232 ms per line (~20 s per run) and is the only difference between the two builds. |
| `MACRO_REPEAT` | `1` | Repeat the whole macro forever. `0` = run once. |
| `MACRO_REPEAT_DELAY_MS` | `5000` | Delay between rounds when repeating. |
| `PAYLOAD_FILE` | `../PicoSwitch/payload.txt` | Payload to embed. |

Example — English console, single run:

```sh
idf.py -DLANG_JP=0 -DMACRO_REPEAT=0 build
```

The `LANG_JP` default is `1` because the hardware this was written on is a
JP-only console, which cannot be switched to English. Everyone else should
build with `-DLANG_JP=0`.

`idf.py` keeps these in `build/`, so pass them again (or use
`idf.py menuconfig` → *Component config* is not used here) after `fullclean`.

## How it maps to PicoSwitch

| | PicoSwitch | esp32-usb-pad |
|---|---|---|
| MCU | RP2040 / RP2350 | ESP32-S3 |
| SDK | Pico SDK + TinyUSB | ESP-IDF + esp_tinyusb |
| HID layout | gamepad + keyboard | same descriptors, copied from `PicoSwitch/include/SwitchConsts.h` |
| Payload source | `PicoSwitch/payload.txt` | same file |
| Macro generator | `tools/payload_to_macro.py` | `tools/payload_to_esp32.py` |
| Timing | `payload_to_macro.py` constants | `payload_to_esp32.py` constants |
| VID/PID | `0x0f0d:0x0092` | same |

The USB descriptors are byte-for-byte copies, so the console sees the same
device class from either board. USB report layout:

- gamepad: 8 bytes (`buttons` lo/hi, hat, LX, LY, RX, RY, vendor)
- keyboard: 8-byte boot report (modifier, reserved, 6 keycodes)

## Tested

- ESP32-S3 devkit, ESP-IDF v6.1, composite descriptor builds and enumerates.
- Full 87-line payload typed successfully into Civ7 on Switch 2 (JP 20.4.0),
  Remote JS Loader came up on `:9026`.
- 5 s repeat confirmed working; unplugging the board stops further rounds.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Payload types as garbage / IME candidates | console IME is not English; check `LANG_JP` and the USB keyboard language setting |
| Chat never opens | the 5 × A preamble missed the window; check the game is past the main menu and the chat button is focused |
| Nothing happens at all | board enumerated as UART only — use the native USB port |
| Board not detected | hold BOOT, tap RESET, release BOOT |

## Notes

- Not a Bluetooth controller: this is USB only. Use `PicoSwitch` if you need
  the wireless path.
- No rumble, motion or NFC; only the buttons/sticks used by the macro.
- CH9329 and other keyboard-only adapters **cannot** run this payload: the game
  needs a real gamepad `A` press to open the chat.
