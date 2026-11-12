# Chapter Player ESP32

Chapter Player ESP32 is a standalone ESP-IDF firmware port of the Chapter audiobook player. It runs on an ESP32-S3 N16R8 development board and plays from an Audiobookshelf server over Wi-Fi. The Raspberry Pi version remains a separate project and is preserved in the parent repository.

This port uses the existing Chapter controls and navigation model, adapted for the ESP32 board and its small OLED. Bluetooth and headphone output are not included. For the actual GPIO map and power notes, see [Hardware and wiring](docs/chapter-hardware.md). For controls, Wi-Fi setup, and portal settings, see [Using the player](docs/using-the-player.md).

## Current hardware

- ESP32-S3 N16R8 module: 16 MB flash and 8 MB PSRAM.
- Carrier board shown in the project photos has two USB-C connectors, GPIO labels, no mounting holes, and no microSD socket. Its manufacturer and exact model are not known, so use the GPIO labels rather than assuming a DevKit header layout.
- Two KY-040 style rotary encoder modules: select/navigation and volume/playback.
- MAX98357A I²S mono amplifier and a speaker.
- Either a 256×64 SH1122 SPI OLED or a 128×64 SSD1306 I²C OLED. Choose the connected display in the web portal; the firmware adapts the layout to the narrower screen.

The board, audio, Wi-Fi portal, and both display options have been brought up on the owner's hardware. The battery and charging arrangement has not been selected or tested, and sleep current for the complete assembled device has not been measured. Treat the supplied GPIO table as specific to the pictured N16R8 board; ESP32-C3 and S3 SuperMini boards are not currently supported pin-for-pin.

## Features

- Audiobookshelf sign-in using username/password or API key, token refresh, library caching, streamed playback, and progress sync. Libraries up to 7,000 items are downloaded in 100-item pages and cached as compact Chapter metadata.
- Continue Listening lists up to the 50 most recently active unfinished books. Browse by Series uses each book's primary series; Browse by Title shows the plain title; Browse by Author groups books by author. Series and Author lists can each be sorted alphabetically or by descending book count.
- A two-knob interface with configurable fast-turn acceleration, one-second configurable holds, series book numbers in the series list, and consistent-speed marquee scrolling.
- Volume knob click toggles play/pause and returns to playback. Select-knob hold goes back one level; volume-knob hold returns to the root menu. Holding both knobs anywhere starts clean shutdown into deep sleep.
- Web portal with device status, remote playback and chapter controls, library browsing and playback, a device-only recent list, manual library refresh, and one save action for settings.
- Settings for idle timeout and power-save mode, screen saver, display type, brightness, playback speed (including 1.25×), menu visibility and order, Series and Author sorting, knob holds, acceleration, marquee behavior, and startup resume.
- Deep sleep turns off the OLED, Wi-Fi, and amplifier. Turning or pressing a knob wakes the ESP32 and restarts the firmware.

On a normal restart, the firmware refreshes the full Audiobookshelf catalog when connected. A deep-sleep wake keeps the cached book and series metadata and refreshes only listening progress; use **Refresh Audiobookshelf library** in the portal to fetch metadata changes on demand.

The SSD1306 layout uses smaller type and four display rows. Long titles wrap across the first two rows, then marquee if they still do not fit. The SH1122 keeps its wider layout. The MAX98357A has been tested from the 3.3 V rail; a suitable regulated 5 V supply can provide more speaker output. See the power section in the wiring guide before changing that connection.

## First-time Wi-Fi and Audiobookshelf setup

When no saved connection is available, the player starts a temporary WPA2 setup network named `Chapter-Setup-` followed by four hexadecimal digits. Its randomly generated eight-digit password is shown on the OLED setup screen and in the USB serial log. Join it from a phone or computer, then open `http://192.168.4.1` if the page does not open automatically. Enter the home Wi-Fi details and Audiobookshelf server and sign-in, then save. The device checks the connection, stores the settings, and restarts.

After setup, the portal is available at `http://<player-ip>/` while the device is awake and connected. Find the IP in the player status page or serial log. Power Save Mode turns Wi-Fi and the portal off while the device sleeps; a knob wakes it.

## Build and flash

The firmware is written in C with ESP-IDF and targets `esp32s3`. The tested environment is ESP-IDF 5.5.6 with Espressif's experimental Clang toolchain; the standard Xtensa GCC build did not work in the original Windows setup. Components listed in `main/idf_component.yml` are resolved by the ESP-IDF component manager.

Open an ESP-IDF 5.5 PowerShell or command prompt in this directory. To build:

```powershell
idf.py -B build-clang2 -DIDF_TOOLCHAIN=clang build
```

To build, flash, and watch the serial log in one step, replace `COM7` with the board's current COM port:

```powershell
idf.py -B build-clang2 -DIDF_TOOLCHAIN=clang -p COM7 flash monitor
```

Press `Ctrl+]` to leave the serial monitor. If the board does not enter download mode automatically, hold BOOT, press and release RST, release BOOT, then retry flashing. A USB-C cable must carry data. For setup and monitor troubleshooting, see [Using the player](docs/using-the-player.md#serial-setup-and-debugging).

## How this differs from the Raspberry Pi version

This is a separate firmware and hardware port, not a continuation of the Raspberry Pi system service. It retains the Chapter browsing and portal workflows while replacing Raspberry Pi/Linux services with ESP-IDF drivers, NVS settings, onboard flash caching, and an ESP32 audio pipeline.

- No Bluetooth audio or headphone output.
- The display can be the original 256×64 SH1122 or the optional 128×64 SSD1306; each has a fitted layout.
- Battery-saving mode uses ESP32 deep sleep and knob wake. Real device battery life depends on the development board's regulator and other circuitry and has not been measured.
- The web portal and playback run directly on the ESP32; there is no Raspberry Pi setup service or SSH deployment step. Firmware updates are flashed over USB.
- The N16R8 board has no microSD slot. Playback streams from Audiobookshelf, and the current onboard data partition is too small for most full audiobooks to be useful offline.

## Project history and attribution

The Chapter-specific menu, display, control, and portal behavior is ported from the Raspberry Pi Chapter Player. Audiobookshelf networking and portions of the embedded audio/playback implementation build on [Doug Greer's ABSpresso fork](https://github.com/douggreer/ABSpresso), which is itself a fork of [Tome Kent's ABSpresso](https://github.com/tomekent/ABSpresso). This repository is maintained separately from both the Raspberry Pi project and ABSpresso.

See [CHANGELOG.md](CHANGELOG.md) for a summary of the Chapter ESP32 port and the improvements made during development. The upstream ABSpresso repositories do not currently declare a license. No project-wide license has been added here; the attribution above records the code lineage.
