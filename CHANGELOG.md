# Chapter ESP32 change history

This is a project-level summary of the work done to create and refine the ESP32-S3 Chapter Player. It records changes made since starting the ESP32 port; it is not an exhaustive commit-by-commit history.

## ESP32-S3 Chapter port

- Created a separate ESP-IDF firmware project for the ESP32-S3 N16R8 while keeping the Raspberry Pi implementation in its own project.
- Replaced Raspberry Pi hardware services with ESP-IDF networking, storage, display, rotary input, power, and I²S audio drivers.
- Adapted Audiobookshelf sign-in, library access, streaming, audio playback, and progress synchronization for the ESP32.
- Added SH1122 256×64 SPI and SSD1306 128×64 I²C display drivers with a portal setting to select the attached display.
- Added GPIO wiring for two KY-040 encoders and a MAX98357A I²S mono amplifier. Bluetooth and headphone output were omitted from this port.

## Chapter navigation and playback

- Added the Continue Listening, Browse by Series, Browse by Title, and Browse by Author flows.
- Continue Listening filters out completed books and sorts the remaining books by latest account activity.
- Series browsing groups books by their primary series and shows series position when browsing within a selected series. Title browsing uses the plain title.
- Added configurable knob acceleration, marquee delay/speed/loop pause, and return-to-playback timing.
- Added press/hold navigation, volume-knob play/pause, root return, and the global two-knob shutdown gesture. The same configurable hold duration applies to individual holds and shutdown.
- Added a configurable startup choice to open the user's latest in-progress book or the first root-menu choice.
- Added the adjustable sleep timer and improved the playing screen's volume display to use a filled bar.

## Display and audio refinements

- Added a compact SSD1306 layout with smaller, consistent title fonts and four display rows. The title wraps into the second row before marquee scrolling.
- Removed monochrome dithering to keep small text crisp.
- Made marquee speed consistent in pixels per second instead of scaling with title length.
- Added 1.25× playback speed. On this ESP32 audio path, speeds above 1× raise pitch.
- Refined first-play audio startup to reduce the initial-buffer stutter reported during device testing.

## Web portal and power management

- Consolidated settings into one save action and cleaned up the layout and units, including an idle timeout in minutes.
- Added the display selector, screen saver and power-save options, knob hold and acceleration controls, menu order/visibility, playback startup choice, and other navigation adjustments.
- Added device status, remote play/pause/stop, seeking, chapter changes, volume, library browsing and playback, manual library refresh, and a recent list specific to this device.
- Added deep sleep after the idle timeout with Wi-Fi, OLED, and amplifier shutdown; knob input wakes the ESP32.
- Hardened account changes: switching usernames requires the new password, the prior refresh token is cleared after a successful new sign-in, and the ESP32 only restarts after settings are confirmed written to flash.
- Account switches clear cached account progress, library listings, and metadata before restarting, so books and Continue Listening from the previous sign-in cannot be mistaken for the new account's library. Offline audio downloads are retained.
- The portal now rejects an empty API key when switching from another sign-in type instead of silently validating and keeping the prior credentials.
- When cached library data exists, it is shown immediately while the firmware reconnects to Audiobookshelf instead of leaving a Wi-Fi connection message over the display.
- Added compact metadata caching and paginated library loading for catalogs up to 7,000 items. Portal browsing is paginated too, keeping each HTTP response and web-server allocation small.
- Continue Listening is limited to the 50 most recently active unfinished books. Large-library refreshes now keep the cached menus responsive, use smaller series pages, and log the compact-cache write duration.
- Reduced Audiobookshelf item response batches to 100, refreshes one full catalog at a time to avoid duplicating large caches in PSRAM while keeping root navigation active, compacted each in-memory book record, removed repeated group sorting from root-menu redraws, and added clearer failure diagnostics.
- Added independent Series and Author group sorting choices: alphabetical or largest book count first, with alphabetical tie-breaking. The choices apply to the OLED navigation and portal browser.
- Changed deep-sleep wake to keep the cached catalog and refresh only Audiobookshelf progress for Continue Listening. Full metadata refresh still runs after a normal restart or when requested with the portal's manual refresh action.
- Reduced the setup portal's simultaneous client limit to leave LWIP sockets available for Audiobookshelf sign-in, progress sync, and playback; added connection failure diagnostics for internal RAM and socket errors.
- Fixed account switching after successful login by routing NVS selection writes and LittleFS cache deletion through an internal-stack worker, avoiding flash-cache assertions from the portal's PSRAM-backed task.

## Current known limits

- The carrier board's exact model is unknown; use GPIO numbers printed beside the pins and the [hardware guide](docs/chapter-hardware.md).
- No battery, charger, or low-current regulator design has been selected. Whole-device sleep current and battery runtime have not been measured.
- The carrier has no microSD slot. Its roughly 9.6 MiB data partition supports settings and cached metadata, not most full audiobooks.
- The web portal runs only while the player is awake and Wi-Fi is active.
- The ESP32 UI uses the same navigation concepts as the Raspberry Pi edition but is adapted to the available display and controls. Bluetooth/headphones and some Raspberry Pi-specific behavior are not included.
