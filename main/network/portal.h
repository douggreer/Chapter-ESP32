#pragma once

// The portal on home Wi-Fi supports browsing, playback control, and settings.
void portal_start_station(void);
// Setup portal: start the device's temporary WPA2 network and setup page. Saving tests Wi-Fi and
// Audiobookshelf credentials before storing them and restarting.

#include <stdbool.h>
#include <stddef.h>

void portal_start(void);
void portal_stop(void);
void portal_stop_setup(void);
bool portal_active(void);
bool portal_setup_active(void);
// The setup network's name and password (shown on the small OLED setup screen).
void portal_network(char *ssid, size_t slen, char *pass, size_t plen);
// Progress message for the screen, and whether a save is being processed.
void portal_status(char *msg, size_t len, bool *busy);
