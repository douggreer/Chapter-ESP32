#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "abs_api.h"

void ui_init(void);
void ui_process(void);
void ui_show_message(const char *message);
void ui_show_message_locked(const char *message);
void ui_show_notice(const char *icon, const char *message);
void ui_toggle_play(void);
void ui_show_player(void);
void ui_set_books(const abs_book_t *books, int count);
void ui_set_source(bool from_cache);
void ui_get_source(uint32_t *library_revision, bool *from_cache);
void ui_set_libraries(const abs_library_t *libraries, int count, const char *selected_id);
void ui_request_library(const char *library_id);
bool ui_take_library_request(char *library_id, size_t length);
void ui_setup_show(void);
void ui_request_refresh(void);
bool ui_take_refresh_request(void);
bool ui_take_shutdown_request(void);
