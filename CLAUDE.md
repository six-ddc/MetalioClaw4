# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for **Metalio Claw4**, a palm-size AI voice device. It is a customized fork of the
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) framework, trimmed down to a **single board
target: `metalio-claw-4`** running on an **ESP32-P4** main MCU. Wi-Fi is provided by an ESP32-C5
coprocessor over ESP-Hosted (SDIO); a NT26 module provides 4G LTE — hence the board is a
`DualNetworkBoard`. The CMake project name is still `xiaozhi` and many upstream artifact names keep
the `xiaozhi_` prefix.

Toolchain: **ESP-IDF v5.5.4**, target `esp32p4`. The repo ships a pre-tuned `sdkconfig`, so
`idf.py set-target` is **not** needed after clone.

## Build / flash / release

```bash
source ./.idf-env.sh           # activate ESP-IDF v5.5.4 for this project (see "Local env setup" below)
idf.py build                   # sdkconfig is pre-set; no set-target
idf.py -p /dev/ttyACM0 flash monitor   # P4 port = descriptor "USB JTAG/serial debug unit"; quit monitor with Ctrl+]
```

- **Local env setup (`.idf-env.sh`)** — this repo uses a **project-local ESP-IDF** so it doesn't
  depend on a wrong-version global install. Both `.esp-idf/` and `.idf-env.sh` are git-ignored (via
  `.git/info/exclude`), so activation is one command: `source ./.idf-env.sh`. If they don't exist yet
  (fresh clone), recreate them:
  - Clone ESP-IDF **v5.5.4** into `.esp-idf/` — the pin is required: a managed dependency needs
    `idf >=5.5.2`, so v5.5.0 (or older global installs) fail dependency version-solving.
    `git clone --branch v5.5.4 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .esp-idf`
  - Install tools (reuses the global `~/.espressif` toolchain cache): `cd .esp-idf && ./install.sh esp32p4`
  - `.idf-env.sh` exports `IDF_PATH=.esp-idf`, `IDF_TOOLS_PATH=~/.espressif`,
    `IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.13_env`, then sources `export.sh`.
  - **Gotcha:** the machine's default `python3` is platformio's 3.11, which makes a plain `export.sh`
    hunt for a nonexistent `idf5.5_py3.11_env` and fail. `.idf-env.sh` prepends Homebrew python 3.13
    to `PATH` and pins `IDF_PYTHON_ENV_PATH` to sidestep this — don't drop those two lines.
- The device exposes **four** serial ports. Flash/monitor the P4 firmware on the *USB JTAG/serial
  debug unit* port. The other three (`CH340K USB Serial` = Bluetooth codec chip, `log` / `at` = NT26
  4G module) are for separate subsystems — do **not** flash P4 firmware to them.
- **Release / CI build** (produces a single `build/merged-binary.bin`):
  ```bash
  python scripts/release.py metalio-claw-4 --name metalio-claw-4   # build one variant + merge-bin
  python scripts/release.py --list-boards --json                    # list variants (used by CI matrix)
  ```
  `scripts/release.py` reads each board's `config.json`, runs `idf.py -DBOARD_NAME=... -DBOARD_TYPE=... build`
  then `idf.py merge-bin`. GitHub Actions (`.github/workflows/build.yml`) builds all variants on push
  to `main`; on PRs it only rebuilds boards whose `main/boards/<board>/` changed (any change to
  `main/*` outside `boards/`, or to `boards/common/`, rebuilds everything).
- Code style: `.clang-format` (Google-based, 4-space indent, 120 col). Format C/C++ before committing.

## sdkconfig — handle with care

`sdkconfig` is committed and hand-tuned for this hardware (ESP-Hosted SDIO Wi-Fi, MIPI-DSI display,
PSRAM, custom 32 MB partition table `partitions/v1/32m.csv`). **Do not casually edit or regenerate
it** — arbitrary changes can break boot, display, Wi-Fi/4G, camera, or SD init. For customization
prefer `sdkconfig.defaults` or the board's `config.json` `sdkconfig_append` field.

## Architecture (the big picture)

Three process-wide singletons drive everything; understanding their relationship is the fastest way
in:

- **`Application` (`main/application.cc`)** — `Application::GetInstance()`. Owns the FreeRTOS main
  event loop, the device **state machine**, and the active `Protocol`. `main.cc` → `app.Start()` is
  the entry point. State machine (see `device_state.h`):
  `starting → configuring → idle ⇄ connecting ⇄ listening ⇄ speaking`, plus
  `upgrading / activating / fatal_error`. Cross-thread work is marshaled onto the loop via
  `Application::Schedule(std::function)`; loop wakeups use the `MAIN_EVENT_*` event-group bits.
- **`Board` (`main/boards/common/board.h`)** — `Board::GetInstance()` returns whatever
  `create_board()` builds. The concrete class is registered with the `DECLARE_BOARD(CLASS)` macro at
  the bottom of the board `.cc`. Here it's `class METALIO_CLAW_4 : public DualNetworkBoard`
  (`main/boards/metalio-claw-4/metalio-claw-4.cc`). `Board` is the hardware-abstraction seam:
  `GetAudioCodec()`, `GetDisplay()`, `GetNetwork()`, `GetBacklight()`, `GetCamera()`,
  `GetBatteryLevel()`, etc. Board pins/params live in `boards/metalio-claw-4/config.h`; build wiring
  in `config.json`.
- **`McpServer` (`main/mcp_server.cc`)** — `McpServer::GetInstance()`. Device-side Model Context
  Protocol: exposes device abilities to the cloud LLM as tools. Register with
  `AddTool(name, description, PropertyList, callback)`. Boards/apps add their own tools here.

Layer map (from `README.md` §9, the fullest architecture reference):

| Layer         | Location                | Notes |
|---------------|-------------------------|-------|
| Entry / loop  | `main.cc`, `application.cc` | startup, state machine, protocol routing |
| Board / HAL   | `boards/metalio-claw-4/`, `boards/common/` | hw init, pin mux; common drivers = GPS, SD, fuel gauge (BQ27220), IO expander (TCA9555), dual-net |
| Audio         | `audio/`                | `codecs/` (ES8311/ES8388/… + box/dummy/no_audio), `processors/` (AFE vs no-op, chosen by `CONFIG_USE_AUDIO_PROCESSOR`), `wake_words/` (AFE/custom on S3/P4, ESP wake-word otherwise) |
| Display       | `display/`              | LVGL 9; `display/screen/<name>_screen/` = one app page each |
| Protocol      | `protocols/`            | `websocket_protocol` and `mqtt_protocol` (MQTT+UDP), both implement `Protocol` |

## Hardware notes (the non-obvious bits)

Full spec / pinout / I2C addresses are authoritative in `README.md` §5–§7, plus
`boards/metalio-claw-4/config.h` (GPIOs) and `boards/common/IOExpander.hpp` (TCA9555 mapping). Read
those before touching board init. The following are the facts that bite when writing code and are not
obvious from a single file:

- **Audio codec is NOT the ES-series.** Despite `main/CMakeLists.txt` compiling ES8311/ES8388/…
  codecs, this board's `GetAudioCodec()` returns **`BTAudioCodecDuplex`** (`bt_audio_codec.h`) — a
  professional Bluetooth audio codec chip controlled over **UART2 (GPIO 26/27, 115200, AT commands)**,
  replacing ES8311 + ES7210. I2S carries only PCM mic (GPIO 10/11) / speaker (GPIO 9/12) data at
  16 kHz. That BT chip has its own USB-UART (CH340K) and is flashed separately (README §14.5).
- **Shared I2C bus on GPIO 7/8** hosts GT911 touch, TCA9555 IO expander, BQ27220 fuel gauge, QMC6309
  magnetometer, and NU1680 wireless charger. Bus contention / address (see §7.4) matters when adding
  any I2C device.
- **Most peripheral power is gated by the TCA9555 IO expander**, not direct GPIO — GPS, camera, SD,
  Bluetooth, 4G, and the audio PA are all power-switched there (mixed active-high / active-low; camera
  and SD default **off**). "Peripheral doesn't respond" is usually a missing IO-expander enable — check
  `IOExpander.hpp` first. Power-off also runs through it (`PWR_KEY` in / `PWR_KEY_PULSE` soft-shutdown).
- **Dual network under one board.** Wi-Fi comes from the ESP32-C5 coprocessor over ESP-Hosted **SDIO**;
  4G from the NT26 over **UART** — both unified by `DualNetworkBoard`. Changing the P4↔C5 SDIO pins
  requires reflashing **both** the P4 firmware and the C5 coprocessor firmware (README §7.2).
- LCD reset and camera share reset line GPIO 3; screen driver is NV3051F by default (FL7707N optional).

## Conventions when extending

- **New source files** are not auto-globbed for core dirs — add each `.cc` to `SOURCES` in
  `main/CMakeLists.txt` and its dir to `INCLUDE_DIRS`. Exceptions that *are* globbed:
  `boards/common/*.cc` and `boards/<BOARD_TYPE>/*.{c,cc}`.
- **New app screen**: create `display/screen/<name>_screen/<name>_screen.{cc,h}`, add both the source
  to `SOURCES` and the dir to `INCLUDE_DIRS`, and register it in the home screen app list
  (`display/screen/home_screen/home_screen.cc`, `kApps[]`).
- **Conditional compilation**: `main/CMakeLists.txt` removes ESP32-incompatible codecs/camera/jpeg
  when `CONFIG_IDF_TARGET_ESP32`, and swaps audio processor / wake-word backends by Kconfig. Keep new
  hardware-specific files behind the matching guard.

## Assets & generated files (built during `idf.py build`)

- **Localized audio + `lang_config.h`**: `scripts/gen_lang.py` generates
  `main/assets/lang_config.h` from `main/assets/locales/<LANG>/language.json`; language selected by
  `CONFIG_LANGUAGE_*` (default `zh-CN`). Missing `.ogg` files fall back to `en-US`.
- **Default `assets.bin`** (fonts + emoji + wake-word model, flashed to the `assets` partition):
  built by `scripts/build_default_assets.py` when `CONFIG_FLASH_DEFAULT_ASSETS`. Use
  `CONFIG_FLASH_CUSTOM_ASSETS` + `CONFIG_CUSTOM_ASSETS_FILE` (local path or URL) to override.
- **SPIFFS `resources` partition**: built from `main/xingzhi-assets/` (sjpg/spng/eaf images).
- **Custom wake word**: dropping a `wakeword/srmodels.bin` at repo root makes the top-level
  `CMakeLists.txt` override the esp-sr packed model on flash.
- **SD-card content** (Digital Human expressions, etc.) lives in `sd_images/` — copy to a
  FAT-formatted SD root preserving structure; not part of the flash image.
- Other tooling under `scripts/`: `p3_tools/` and `ogg_converter/` (audio format conversion),
  `Image_Converter/` + `sd_images/jpg_to_sjpg.py` (LVGL image conversion), `spiffs_assets/`
  (asset packing), `acoustic_check/` (mic/AEC analysis).

## Docs

`README.md` (English) / `README_zn.md` (Chinese) are extensive and authoritative for hardware specs,
pin maps, dual-chip architecture, the full app list, protocols, and debugging (log tags like
`METALIO_CLAW_4`, `IOExpander`, `GpsService`). Read the relevant section before touching hardware or
board init code.
