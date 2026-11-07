# Chapter Player ESP32 hardware and wiring

This guide covers the ESP32-S3 N16R8 board and Chapter accessories used for this port. The exact carrier-board manufacturer and model are unknown. The supplied board photo shows an ESP32-S3 N16R8 module, two USB-C connectors, and GPIO numbers printed beside the header pads. Match the **GPIO numbers printed on your board**; do not count header positions based on a different DevKit.

The signal map is defined in [`main/hardware/chapter_pins.h`](../main/hardware/chapter_pins.h). It is for the pictured N16R8 board and has not been validated on an ESP32-C3, S3 SuperMini, or another carrier layout.

## Main parts

| Part | Port configuration |
| --- | --- |
| Controller board | ESP32-S3 N16R8; 16 MB flash and 8 MB PSRAM |
| Select/navigation control | KY-040 style rotary encoder, 3.3 V module |
| Volume/playback control | KY-040 style rotary encoder, 3.3 V module |
| Audio | MAX98357A I²S mono amplifier and one speaker |
| OLED option A | 256×64 SH1122 SPI |
| OLED option B | 128×64 SSD1306 I²C |

Bluetooth and wired headphone output are not part of this ESP32 build.

## Power and ground for all accessories

- Power the ESP32 board from its USB-C input using a suitable USB power adapter and data-capable cable when flashing.
- Connect **each KY-040 module's VCC to ESP32 3V3 and GND to ESP32 GND**. The encoder modules use three GPIO signal pins plus power and ground; they are not powered by the signal pins alone.
- Power the OLED from 3V3 and GND, if its module supports 3.3 V operation. The tested SSD1306 module works at 3.3 V.
- Connect a common ground between the ESP32 and MAX98357A, including when the amplifier uses a separate supply.
- All GPIO signals are 3.3 V logic. Do not apply 5 V to a GPIO, encoder output, OLED logic pin, or the amplifier's SD control pin.

The owner's MAX98357A has been tested from the ESP32 board's 3.3 V rail and produced sound. A regulated supply closer to 5 V can provide more speaker output; MAX98357A supply must remain between 2.5 V and 5.5 V. For a single USB-C adapter, use a suitable 5 V power distribution module to feed the board and amplifier in parallel. The carrier's 5 V/VBUS routing is not known, so do not assume a particular board pad can safely supply the amplifier. Never connect either speaker output to ground; connect the speaker only across the amplifier's `+` and `-` terminals.

The pictured carrier has an AMS1117-style regulator and no identified battery connector or charging circuit. A Li-ion charging/power board and battery wiring have not been selected. Do not connect a cell directly to 3V3 or to an unknown 5 V pad. Measure current on the complete assembled device before estimating battery life: deep-sleep current is affected by the board regulator, USB circuitry, display, amplifier, and battery module, not just the ESP32 chip.

## OLED options

Select the connected display in **Settings → Navigation and display → OLED display** on the web portal. Saving a different option restarts the ESP32. Connect only the controller that matches the installed OLED.

| OLED | Signal on display | GPIO / connection |
| --- | --- | ---: |
| SH1122 256×64 SPI | SCLK / SCK | GPIO12 |
| SH1122 256×64 SPI | SDIN / MOSI | GPIO11 |
| SH1122 256×64 SPI | CS | GPIO10 |
| SH1122 256×64 SPI | D/C | GPIO9 |
| SH1122 256×64 SPI | RES / RST | GPIO8 |
| SSD1306 128×64 I²C | SCL | GPIO12 |
| SSD1306 128×64 I²C | SDA | GPIO11 |
| Either option | VCC | 3V3 |
| Either option | GND | GND |

For the SSD1306, leave GPIO8, GPIO9, and GPIO10 disconnected. The firmware probes I²C addresses `0x3C` and `0x3D`. It uses smaller text, narrower controls, and four rows on the 128×64 display: the title wraps over the first two rows, and the information that followed it uses rows three and four. A title that still does not fit marquee-scrolls. The SH1122 display keeps the wider layout. The two bus pins are shared, so SDA/SCL on the SSD1306 connect to the same GPIOs that serve SCLK/MOSI on the SPI display.

## Rotary encoders

Both modules require power and ground, plus three signal connections:

| Encoder module | Signal | ESP32-S3 connection |
| --- | --- | ---: |
| Select/navigation | CLK / A | GPIO4 |
| Select/navigation | DT / B | GPIO5 |
| Select/navigation | SW | GPIO6 |
| Volume/playback | CLK / A | GPIO7 |
| Volume/playback | DT / B | GPIO15 |
| Volume/playback | SW | GPIO16 |
| Both modules | VCC | 3V3 |
| Both modules | GND | GND |

The two A pins and the push switches are configured as wake sources. GPIO4 and GPIO7 are RTC-capable encoder inputs on the ESP32-S3.

## MAX98357A amplifier

| MAX98357A pin | ESP32-S3 connection |
| --- | ---: |
| BCLK | GPIO17 |
| LRC / LRCLK / WS | GPIO18 |
| DIN | GPIO21 |
| SD / SD_MODE (optional shutdown control) | GPIO13 |
| VIN / VDD | 3V3 for the tested low-volume setup; a separate regulated 5 V rail can provide more output |
| GND | Common GND |
| Speaker `+` and `-` | Connect only across the amplifier's speaker output terminals |

GPIO13 lets firmware hold the amplifier in shutdown during deep sleep. GPIO14 can be substituted if easier to wire, but also change `CHAPTER_PIN_AMP_SD` in `main/hardware/chapter_pins.h`. Check the breakout's pin labels and datasheet before connecting it.

## Sleep and GPIO restrictions

When Power Save Mode is enabled and its idle timeout expires while nothing is playing, the firmware stops Wi-Fi, powers down the OLED and amplifier, and enters ESP32 deep sleep. A knob turn or button press wakes and restarts the firmware. The first action wakes it; wait for Wi-Fi and the library to return before using another control.

Deep sleep does not cut power to every component on the development board. GPIO35, GPIO36, and GPIO37 are reserved by octal PSRAM on the N16R8 module and must not be used for accessories. The listed map avoids those pins and the board's USB connections. Check the carrier-board schematic before using any additional pads.

## Mounting

The pictured carrier has no mounting holes. A small 3D-printed tray or cradle with edge clips is a practical permanent mount. Keep the antenna area clear, leave the regulator unobstructed, and add strain relief for the wires. For a temporary build, use nonconductive foam tape on an insulated surface and keep the underside away from metal and loose conductive parts.

## Flash storage and microSD

The N16R8 module has 16 MB of flash and no microSD slot on the pictured board. The partition table reserves a 6 MB firmware partition and roughly 9.6 MiB for onboard data storage. That space is sufficient for settings, library metadata, and cache files. Normal playback streams from Audiobookshelf over Wi-Fi, so a microSD card is not required. The data partition is too small for most full audiobooks to be useful for offline listening; practical offline-book storage would require a microSD module or a different storage design.
