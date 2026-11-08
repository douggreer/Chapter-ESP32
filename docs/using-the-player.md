# Using Chapter Player ESP32

## Knob controls

| Action | Result |
| --- | --- |
| Turn select/navigation knob | Move through root choices, books, groups, or playback actions |
| Press select knob | Select the highlighted item or action |
| Hold select knob | Go back one level |
| Turn volume knob | Change volume |
| Press volume knob | Toggle play/pause and return to the playing screen when a book is loaded |
| Hold volume knob | Return to the root menu |
| Hold both knobs | Start clean shutdown into deep sleep, from any screen |

The hold duration is shared by single-knob holds and the two-knob shutdown gesture. It is adjustable in the web portal. Shutdown works even if the root-menu reminder is hidden. The “Push and hold both knobs” entry is a reminder; selecting it does not shut the device down.

While a book is playing, turn the select knob to choose Back, Previous chapter, Play/Pause, or Next chapter, then press to activate. The volume knob continues to adjust volume. The volume button returns to the playing screen and toggles playback from elsewhere in the menus.

The configurable acceleration multiplier applies when the time between encoder steps is at or below the fast-turn threshold. For example, a 5× multiplier moves five list items per detent during a sufficiently fast turn. Marquee speed is set in pixels per second and is consistent regardless of title length.

## Browse menus

- **Continue Listening** contains unfinished books from the signed-in Audiobookshelf user's progress and sorts by latest activity.
- **Browse by Series** lists primary series names and book counts. Selecting a series lists its books in series order where metadata provides a sequence. The series book number appears in this view.
- **Browse by Title** shows plain book titles in the selected library's title order.
- **Browse by Author** groups books by author and shows the count.

The top-level choices can be reordered or hidden in the portal. The two-knob shutdown gesture remains available regardless of that menu setting. Books and series metadata are cached on the ESP32 so the menu can appear while Wi-Fi is reconnecting. Libraries up to 7,000 items are fetched in 250-item pages and cached as compact Chapter metadata. Use **Refresh Audiobookshelf library** in the portal to immediately fetch updated library and series metadata; startup also loads the network library when connected.

## Open the web portal

Once the player is on your home Wi-Fi, open `http://<player-ip>/` in a browser on the same network. The address is shown in the device status page and serial log. The portal has status, remote control, browse/play, device-only recent history, and settings sections.

The remote controls can play a selected book, pause/resume, stop, seek back or forward, change volume, and skip chapters. The portal's **Recently played on this player** list is stored in RTC memory: it survives deep sleep, but clears after a full power loss or reboot that does not retain RTC memory. It is separate from Audiobookshelf account history.

The settings page uses one **Save all settings** action. It includes:

- Wi-Fi, Audiobookshelf server, and username/password or API-key sign-in.
- OLED selection (SH1122 256×64 SPI or SSD1306 128×64 I²C), brightness, screen-saver style, dim level, and a shared idle timeout in minutes.
- Power Save Mode, which sleeps the ESP32 when playback is stopped and the idle timeout expires. A knob wakes it.
- Knob-hold duration; acceleration multiplier from Off through 10×; fast-turn threshold.
- Marquee start delay, speed, and loop pause; return-to-playback delay; and startup resume choice.
- Playback speed (1×, 1.25×, 1.5×, or 2×), title sort order, sleep timer, and root-menu visibility/order.

Changing the Wi-Fi or Audiobookshelf sign-in, or changing display type, restarts the ESP32 after the settings have been saved. When switching Audiobookshelf users, enter the new username **and its password**. Leaving the password blank keeps the existing sign-in for the same user. Passwords and tokens are not displayed back in the portal.

In Power Save Mode, Wi-Fi and the web portal are unavailable while the device sleeps. Turning or pressing a knob wakes the board; wait for it to reconnect before using the portal. Without Power Save Mode, the screen saver uses the same idle timeout. Selecting **Display off** turns off the OLED while leaving the player awake.

## Initial setup

1. Connect the board to USB power and open the serial monitor at 115200 baud if you want setup details.
2. On first boot, find the `Chapter-Setup-XXXX` Wi-Fi network. The random eight-digit password appears on the OLED or in the serial log.
3. Join the setup network and open `http://192.168.4.1`.
4. Choose home Wi-Fi, enter the Audiobookshelf server address and account details, then save.
5. After the device restarts, find its home-network address in the serial log or device status page and use that address for the portal.

## Serial setup and debugging

Use an ESP-IDF 5.5 PowerShell/command prompt. With the project directory as the current directory:

```powershell
idf.py -B build-clang2 -DIDF_TOOLCHAIN=clang -p COM7 flash monitor
```

Replace `COM7` with the board's current port. If you only want to read logs without flashing:

```powershell
idf.py -B build-clang2 -DIDF_TOOLCHAIN=clang -p COM7 monitor
```

The monitor runs at 115200 baud; press `Ctrl+]` to exit. If `idf.py` is not recognized, launch the ESP-IDF PowerShell shortcut so its tools are on the path. If automatic download mode fails, hold BOOT, tap RST, release BOOT, then retry.

Useful checks:

- **Setup Wi-Fi is missing:** confirm the device has not saved a valid home Wi-Fi connection. Setup credentials are printed when the access point starts.
- **Portal does not open:** confirm the computer and player are on the same home network, confirm the player's current IP in serial output, and make sure the player is awake. The portal is off in deep sleep.
- **Display stays blank:** check 3V3 and GND, then confirm the portal's display type matches the attached controller. SSD1306 uses SDA GPIO11 and SCL GPIO12; SH1122 uses the five SPI/control GPIOs in the [wiring table](chapter-hardware.md#oled-options).
- **No audio:** confirm common ground, MAX98357A supply, BCLK GPIO17, LRC GPIO18, DIN GPIO21, speaker wiring across `+`/`-`, and the SD control wire if connected. Do not connect a speaker terminal to ground.
- **The display has a saved library but no new books:** use the portal's manual Audiobookshelf library refresh. A cached catalog may be visible before the online refresh finishes.
- **Account switch appears not to apply:** provide the new username and password in the same save. After restart, the serial startup line reports the saved account name; never share access or refresh tokens from logs.
