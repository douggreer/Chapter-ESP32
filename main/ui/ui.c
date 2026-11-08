// Rotary-first Chapter UI for the selectable 256x64 SH1122 or 128x64 SSD1306. The screen shows one selected item at a time;
// the two encoder knobs provide the complete navigation and playback controls.

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lvgl.h"
#include "abs_api.h"
#include "chapter_display.h"
#include "chapter_rotary.h"
#include "config.h"
#include "player.h"
#include "portal.h"
#include "power.h"

#define UI_MAX_BOOKS ABS_MAX_LIBRARY_BOOKS
#define UI_MAX_GROUPS UI_MAX_BOOKS
#define ROOT_MENU_COUNT 6
#define SCREEN_SAVER_WORDS 8
#define DISPLAY_LABEL_COUNT 4

typedef enum { UI_ROOT, UI_CONTINUE, UI_GROUPS, UI_BOOKS, UI_PLAYER, UI_SLEEP_CONFIRM, UI_MESSAGE, UI_SETUP } ui_screen_t;
typedef enum { GROUP_SERIES, GROUP_AUTHOR } group_kind_t;
typedef enum { LIST_TITLE, LIST_SERIES, LIST_AUTHOR } list_kind_t;

typedef struct {
    char name[128];
    int count;
} group_entry_t;

static const char *TAG = "chapter_ui";
static const char *s_root_items[] = {
    "Continue Listening", "Browse by Series", "Browse by Title", "Browse by Author",
    "Bluetooth Settings", "Push and hold both knobs",
};
static lv_style_t s_scroll_style;
static lv_anim_t s_scroll_anim;

static lv_display_t *s_display;
static lv_obj_t *s_labels[DISPLAY_LABEL_COUNT];
static lv_obj_t *s_volume_bar;
static lv_obj_t *s_volume_bar_fill;
static lv_obj_t *s_action_labels[4];
static lv_obj_t *s_saver_labels[SCREEN_SAVER_WORDS];
static uint8_t *s_lvgl_buffer;
static bool s_ready;
static const abs_book_t *s_books;
static int s_book_count;
static int s_visible[UI_MAX_BOOKS];
static int s_visible_count;
static group_entry_t *s_groups;
static int s_group_count;
static abs_library_t s_libraries[32];
static int s_library_count;
static char s_selected_library[40];
static ui_screen_t s_screen = UI_ROOT;
static ui_screen_t s_player_return = UI_ROOT;
static int s_root_selection;
static int s_group_selection;
static int s_item_selection;
static int s_action_selection = 2;
static int s_selected_group = -1;
static group_kind_t s_group_kind;
static list_kind_t s_list_kind = LIST_TITLE;
static bool s_dirty = true;
static char s_message[192];
static bool s_message_pending;
static char s_library_request[40];
static bool s_refresh_requested;
static bool s_shutdown_requested;
static bool s_from_cache;
static bool s_setup_mode;
static bool s_startup_resume_done;
static uint32_t s_library_revision;
static portMUX_TYPE s_message_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_render_ms;
static uint32_t s_last_nav_ms;
static int s_applied_marquee_delay_ms = -1;
static int s_applied_marquee_pause_ms = -1;
static int s_applied_marquee_speed_px_s = -1;
static int64_t s_last_user_action_us;
static int64_t s_down_since[CHAPTER_KNOB_COUNT];
static bool s_long_handled[CHAPTER_KNOB_COUNT];
static bool s_both_gesture;
static bool s_input_armed;
static uint32_t s_saver_started_ms;
static double s_sleep_listened_seconds;
static int64_t s_sleep_last_check_us;
static int64_t s_sleep_prompt_deadline_us;
static int64_t s_sleep_prompt_started_us;
static bool s_sleeping;
static char s_sleep_item_id[40];
static int s_sleep_choice;
static int s_volume_return_screen;
static int64_t s_volume_until_us;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int screen_width(void)
{
    return chapter_display_width();
}

static uint32_t marquee_duration_ms(lv_obj_t *label, const char *text, int speed_px_s)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t extent;
    lv_text_get_size(&extent, text ? text : "", font,
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int gap = lv_font_get_glyph_width(font, ' ', ' ') * 3;
    const int distance = extent.x + gap;
    if (speed_px_s < 1) speed_px_s = 48;
    if (distance <= 0) return 1;
    // LVGL's circular label animation travels the text width plus its three-space loop gap.
    // Use a rounded-up duration so integer rounding never makes it scroll faster than requested.
    uint64_t duration = ((uint64_t)distance * 1000 + speed_px_s - 1) / speed_px_s;
    return duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
}

static void set_label(int index, const char *value)
{
    if (index < 0 || index >= DISPLAY_LABEL_COUNT || !s_labels[index]) return;
    const char *current = lv_label_get_text(s_labels[index]);
    if (!current || strcmp(current, value) != 0) {
        const app_config_t *cfg = config_get();
        const int speed = cfg->marquee_speed_px_s > 0 ? cfg->marquee_speed_px_s : 48;
        // Set the duration before changing the text: LVGL starts the new marquee as part of
        // lv_label_set_text(), so setting this afterward left it using the previous title's rate.
        lv_obj_set_style_anim_duration(s_labels[index], marquee_duration_ms(s_labels[index], value, speed), LV_PART_MAIN);
        lv_label_set_text(s_labels[index], value);
    }
}

static int text_width_px(const char *text, const lv_font_t *font)
{
    lv_point_t extent;
    lv_text_get_size(&extent, text ? text : "", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return extent.x;
}

static void split_title_lines(const char *title, char *first, size_t first_size,
                              char *second, size_t second_size)
{
    if (!title) title = "";
    first[0] = second[0] = 0;
    const lv_font_t *font = &lv_font_montserrat_12;
    const int available_width = screen_width() - 4;
    if (text_width_px(title, font) <= available_width) {
        strlcpy(first, title, first_size);
        return;
    }

    // Put as many whole words as possible on the first row. Any remaining text stays
    // together on row two, where the label's existing circular marquee reveals overflow.
    const char *last_fitting_space = NULL;
    for (const char *space = strchr(title, ' '); space; space = strchr(space + 1, ' ')) {
        const size_t length = (size_t)(space - title);
        if (length == 0) continue;
        char candidate[192];
        const size_t copied = length < sizeof(candidate) - 1 ? length : sizeof(candidate) - 1;
        memcpy(candidate, title, copied);
        candidate[copied] = 0;
        if (text_width_px(candidate, font) > available_width) break;
        last_fitting_space = space;
    }

    if (!last_fitting_space) {
        // A single very long word has no natural wrap point; keep it intact and marquee it.
        strlcpy(first, title, first_size);
        return;
    }
    size_t first_length = (size_t)(last_fitting_space - title);
    if (first_length >= first_size) first_length = first_size - 1;
    memcpy(first, title, first_length);
    first[first_length] = 0;
    const char *remainder = last_fitting_space + 1;
    while (*remainder == ' ') ++remainder;
    strlcpy(second, remainder, second_size);
}

static void apply_live_marquee_settings(void)
{
    const app_config_t *cfg = config_get();
    if (cfg->marquee_delay_ms != s_applied_marquee_delay_ms) {
        lv_anim_set_delay(&s_scroll_anim, cfg->marquee_delay_ms);
        s_applied_marquee_delay_ms = cfg->marquee_delay_ms;
    }
    if (cfg->marquee_loop_pause_ms != s_applied_marquee_pause_ms) {
        lv_anim_set_repeat_delay(&s_scroll_anim, cfg->marquee_loop_pause_ms);
        s_applied_marquee_pause_ms = cfg->marquee_loop_pause_ms;
    }
    if (cfg->marquee_speed_px_s != s_applied_marquee_speed_px_s) {
        const int speed = cfg->marquee_speed_px_s > 0 ? cfg->marquee_speed_px_s : 48;
        for (int i = 0; i < DISPLAY_LABEL_COUNT; ++i) {
            char value[192];
            strlcpy(value, lv_label_get_text(s_labels[i]) ? lv_label_get_text(s_labels[i]) : "", sizeof(value));
            lv_obj_set_style_anim_duration(s_labels[i], marquee_duration_ms(s_labels[i], value, speed), LV_PART_MAIN);
            // Reapply the current text so its active animation adopts the new configured rate.
            lv_label_set_text(s_labels[i], value);
        }
        s_applied_marquee_speed_px_s = cfg->marquee_speed_px_s;
    }
}

static void update_volume_bar(int volume)
{
    if (!s_volume_bar || !s_volume_bar_fill) return;
    const int inner_width = screen_width() * 80 / 100 - 4;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    lv_obj_set_width(s_volume_bar_fill, inner_width * volume / 100);
}

static void set_action_row(int selected, bool playing)
{
    static const char *actions[] = {"Back", "Prev", "Play", "Next"};
    for (int i = 0; i < 4; ++i) {
        const char *name = i == 2 && playing
                               ? (screen_width() <= 128 ? "||" : "Pause")
                               : actions[i];
        lv_label_set_text(s_action_labels[i], name);
        lv_obj_set_style_bg_opa(s_action_labels[i], i == selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_action_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_color(s_action_labels[i], i == selected ? lv_color_black() : lv_color_white(), 0);
    }
}

static void show_action_row(bool show)
{
    for (int i = 0; i < 4; ++i) {
        if (show) lv_obj_remove_flag(s_action_labels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_action_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *format_remaining(double seconds, char *out, size_t length)
{
    if (seconds < 0) seconds = 0;
    const int total = (int)seconds;
    if (total >= 3600) snprintf(out, length, "-%d:%02d", total / 3600, (total / 60) % 60);
    else snprintf(out, length, "-%d:%02d", total / 60, total % 60);
    return out;
}

static void label_setup(lv_obj_t *label, const lv_font_t *font, int y, int height)
{
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_size(label, screen_width(), height);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    // LVGL's scroll mode supplies the early marquee for long book and chapter names.
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_style(label, &s_scroll_style, LV_PART_MAIN);
}

static int root_menu_count(void)
{
    const app_config_t *cfg = config_get();
    int count = 0;
    for (int i = 0; i < ROOT_MENU_COUNT; ++i) {
        const int id = cfg->root_order[i] < ROOT_MENU_COUNT ? cfg->root_order[i] : i;
        if (id == 4) continue; // Bluetooth is intentionally omitted on the ESP32 version.
        if (cfg->root_enabled_mask & (1U << id)) ++count;
    }
    return count ? count : 1;
}

static int root_item_at(int visible_index)
{
    const app_config_t *cfg = config_get();
    int n = 0;
    for (int pos = 0; pos < ROOT_MENU_COUNT; ++pos) {
        const int id = cfg->root_order[pos] < ROOT_MENU_COUNT ? cfg->root_order[pos] : pos;
        if (id == 4) continue;
        if (!(cfg->root_enabled_mask & (1U << id))) continue;
        if (n++ == visible_index) return id;
    }
    return 0;
}

static void text_copy(char *out, size_t len, const char *value)
{
    if (!value) value = "";
    size_t n = 0;
    while (value[n] && n + 1 < len) {
        out[n] = value[n] == '\n' || value[n] == '\r' ? ' ' : value[n];
        ++n;
    }
    out[n] = 0;
}

static const char *primary_author(const char *value)
{
    static char author[128];
    text_copy(author, sizeof(author), value);
    char *separator = strchr(author, ',');
    if (separator) *separator = 0;
    char *start = author;
    while (*start == ' ') ++start;
    char *end = start + strlen(start);
    while (end > start && end[-1] == ' ') *--end = 0;
    return start;
}

static int author_sort_compare(const char *left, const char *right)
{
    char a[128], b[128];
    strlcpy(a, left ? left : "", sizeof(a));
    strlcpy(b, right ? right : "", sizeof(b));
    char *end = strchr(a, ',');
    if (end) *end = 0;
    end = strchr(b, ',');
    if (end) *end = 0;
    char *a_last = strrchr(a, ' ');
    char *b_last = strrchr(b, ' ');
    a_last = a_last ? a_last + 1 : a;
    b_last = b_last ? b_last + 1 : b;
    int result = strcasecmp(a_last, b_last);
    return result ? result : strcasecmp(left ? left : "", right ? right : "");
}

typedef struct {
    const char *name;
    bool author_label;
} group_ref_t;

static void group_ref_copy_name(const group_ref_t *ref, char out[128])
{
    strlcpy(out, ref->name ? ref->name : "", 128);
    if (ref->author_label) {
        char *separator = strchr(out, ',');
        if (separator) *separator = 0;
        char *start = out;
        while (*start == ' ' || *start == '\t') ++start;
        if (start != out) memmove(out, start, strlen(start) + 1);
        size_t length = strlen(out);
        while (length && (out[length - 1] == ' ' || out[length - 1] == '\t')) out[--length] = 0;
    }
}

static int group_ref_cmp(const void *pa, const void *pb)
{
    const group_ref_t *a = pa, *b = pb;
    char aname[128], bname[128];
    group_ref_copy_name(a, aname);
    group_ref_copy_name(b, bname);
    return strcasecmp(aname, bname);
}

static int group_series_cmp(const void *pa, const void *pb)
{
    const group_entry_t *a = pa, *b = pb;
    return strcasecmp(a->name, b->name);
}

static int group_author_cmp(const void *pa, const void *pb)
{
    const group_entry_t *a = pa, *b = pb;
    return author_sort_compare(a->name, b->name);
}

static void build_groups(group_kind_t kind)
{
    s_group_count = 0;
    if (!s_groups || !s_books || s_book_count <= 0) return;
    const size_t per_book = kind == GROUP_SERIES ? 1 : 6;
    const size_t max_refs = (size_t)s_book_count * per_book;
    group_ref_t *refs = heap_caps_calloc(max_refs ? max_refs : 1, sizeof(*refs),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!refs) return;
    size_t ref_count = 0;
    for (int i = 0; i < s_book_count; ++i) {
        const abs_book_t *book = &s_books[i];
        if (book->podcast) continue;
        if (kind == GROUP_SERIES) {
            if (book->series_name[0]) refs[ref_count++].name = book->series_name;
        } else if (book->author_count) {
            for (int a = 0; a < book->author_count; ++a) {
                if (book->authors[a] && book->authors[a][0]) refs[ref_count++].name = book->authors[a];
            }
        } else if (book->author && book->author[0]) {
            refs[ref_count].name = book->author;
            refs[ref_count++].author_label = true;
        }
    }
    qsort(refs, ref_count, sizeof(*refs), group_ref_cmp);
    char previous[128] = "";
    for (size_t i = 0; i < ref_count; ++i) {
        char name[128];
        group_ref_copy_name(&refs[i], name);
        if (!name[0]) continue;
        if (s_group_count && strcasecmp(previous, name) == 0) {
            ++s_groups[s_group_count - 1].count;
        } else if (s_group_count < UI_MAX_GROUPS) {
            strlcpy(s_groups[s_group_count].name, name, sizeof(s_groups[s_group_count].name));
            s_groups[s_group_count].count = 1;
            strlcpy(previous, name, sizeof(previous));
            ++s_group_count;
        }
    }
    free(refs);
    qsort(s_groups, s_group_count, sizeof(*s_groups), kind == GROUP_AUTHOR ? group_author_cmp : group_series_cmp);
    s_group_selection = 0;
}

static bool book_in_group(const abs_book_t *book, group_kind_t kind, int selected)
{
    if (selected < 0 || selected >= s_group_count) return false;
    if (kind == GROUP_SERIES) {
        return book->series_name[0] && strcasecmp(s_groups[selected].name, book->series_name) == 0;
    } else {
        const int names = book->author_count ? book->author_count : 1;
        for (int a = 0; a < names; ++a) {
            const char *name = book->author_count ? book->authors[a] : primary_author(book->author);
            if (strcasecmp(s_groups[selected].name, name) == 0) return true;
        }
    }
    return false;
}

static bool book_in_progress(const abs_book_t *book)
{
    return book->current_time > 0 && !book->finished && book->progress < 0.995f;
}

static int visible_series_cmp(const void *pa, const void *pb)
{
    const abs_book_t *a = &s_books[*(const int *)pa];
    const abs_book_t *b = &s_books[*(const int *)pb];
    const float as = a->series_sequence[0] ? strtof(a->series_sequence, NULL) : 1000000.0f;
    const float bs = b->series_sequence[0] ? strtof(b->series_sequence, NULL) : 1000000.0f;
    if (as != bs) return as < bs ? -1 : 1;
    return strcasecmp(a->sort_title, b->sort_title);
}

static int visible_author_cmp(const void *pa, const void *pb)
{
    const abs_book_t *a = &s_books[*(const int *)pa];
    const abs_book_t *b = &s_books[*(const int *)pb];
    const int cmp = author_sort_compare(a->author, b->author);
    return cmp ? cmp : strcasecmp(a->sort_title, b->sort_title);
}

static int visible_continue_cmp(const void *pa, const void *pb)
{
    const double a = s_books[*(const int *)pa].last_update;
    const double b = s_books[*(const int *)pb].last_update;
    return a > b ? -1 : (a < b ? 1 : 0);
}

static void build_visible_items(void)
{
    s_visible_count = 0;
    if (s_list_kind == LIST_TITLE) {
        for (int i = 0; i < s_book_count && s_visible_count < UI_MAX_BOOKS; ++i) {
            if (!s_books[i].podcast) s_visible[s_visible_count++] = i;
        }
    } else {
        for (int i = 0; i < s_book_count && s_visible_count < UI_MAX_BOOKS; ++i) {
            if (s_books[i].podcast) continue;
            if (book_in_group(&s_books[i], s_group_kind, s_selected_group)) s_visible[s_visible_count++] = i;
        }
    }
    if (s_list_kind == LIST_SERIES && s_selected_group >= 0) {
        // Books within a series should follow the server-provided series sequence when present.
        qsort(s_visible, s_visible_count, sizeof(s_visible[0]), visible_series_cmp);
    }
    if (s_list_kind == LIST_TITLE && config_get()->library_sort_mode == 1) {
        qsort(s_visible, s_visible_count, sizeof(s_visible[0]), visible_author_cmp);
    }
    if (s_item_selection >= s_visible_count) s_item_selection = s_visible_count - 1;
    if (s_item_selection < 0) s_item_selection = 0;
}

static void build_continue_items(void)
{
    s_visible_count = 0;
    for (int i = 0; i < s_book_count && s_visible_count < UI_MAX_BOOKS; ++i) {
        if (!s_books[i].podcast && book_in_progress(&s_books[i])) s_visible[s_visible_count++] = i;
    }
    qsort(s_visible, s_visible_count, sizeof(s_visible[0]), visible_continue_cmp);
    if (s_item_selection >= s_visible_count) s_item_selection = s_visible_count - 1;
    if (s_item_selection < 0) s_item_selection = 0;
}

static void change_selection(int *selection, int count, int delta)
{
    if (count <= 0 || !delta) return;
    int next = *selection + delta;
    while (next < 0) next += count;
    *selection = next % count;
}

static int accelerated_delta(int delta)
{
    if (!delta) return 0;
    const uint32_t now = tick_ms();
    const uint32_t interval = s_last_nav_ms ? now - s_last_nav_ms : UINT32_MAX;
    s_last_nav_ms = now;
    const app_config_t *cfg = config_get();
    const int multiplier = cfg->acceleration_multiplier;
    const int threshold_ms = cfg->acceleration_threshold_ms;
    return delta * (multiplier > 0 && interval <= (uint32_t)threshold_ms ? multiplier : 1);
}

static int64_t knob_hold_duration_us(void)
{
    return (int64_t)(config_get()->knob_hold_s * 1000000.0f);
}

static void return_to_root(void)
{
    if (s_screen == UI_SETUP) portal_stop_setup();
    s_screen = UI_ROOT;
    s_setup_mode = false;
    s_root_selection = 0;
    s_dirty = true;
}

static void back_one_level(void)
{
    switch (s_screen) {
    case UI_CONTINUE:
    case UI_GROUPS:
        s_screen = UI_ROOT;
        break;
    case UI_BOOKS:
        s_screen = (s_list_kind == LIST_SERIES || s_list_kind == LIST_AUTHOR) ? UI_GROUPS : UI_ROOT;
        break;
    case UI_PLAYER:
        s_screen = s_player_return == UI_PLAYER ? UI_ROOT : s_player_return;
        break;
    case UI_SLEEP_CONFIRM:
        player_toggle();
        s_sleep_listened_seconds = 0;
        s_sleep_prompt_deadline_us = 0;
        s_screen = UI_PLAYER;
        break;
    case UI_SETUP:
    case UI_MESSAGE:
        return_to_root();
        return;
    case UI_ROOT:
    default:
        return;
    }
    s_dirty = true;
}

static void open_book_at_selection(void)
{
    if (s_item_selection < 0 || s_item_selection >= s_visible_count) return;
    const int index = s_visible[s_item_selection];
    if (index < 0 || index >= s_book_count) return;
    s_player_return = s_screen;
    s_sleeping = false;
    player_open(&s_books[index]);
    s_action_selection = 2;
    s_screen = UI_PLAYER;
    s_dirty = true;
}

static void enter_root_item(void)
{
    switch (root_item_at(s_root_selection)) {
    case 0:
        s_screen = UI_CONTINUE;
        s_item_selection = 0;
        build_continue_items();
        break;
    case 1:
        s_group_kind = GROUP_SERIES;
        build_groups(s_group_kind);
        s_screen = UI_GROUPS;
        break;
    case 2:
        s_list_kind = LIST_TITLE;
        s_item_selection = 0;
        build_visible_items();
        s_screen = UI_BOOKS;
        break;
    case 3:
        s_group_kind = GROUP_AUTHOR;
        build_groups(s_group_kind);
        s_screen = UI_GROUPS;
        break;
    case 4: // Bluetooth audio is not part of the ESP32 build.
        break;
    case 5: // Informational item: clean shutdown is always the two-knob hold gesture.
    default:
        break;
    }
    s_dirty = true;
}

static void select_action(void)
{
    power_note_activity();
    s_last_user_action_us = esp_timer_get_time();
    switch (s_screen) {
    case UI_ROOT: enter_root_item(); break;
    case UI_CONTINUE:
    case UI_BOOKS: open_book_at_selection(); break;
    case UI_GROUPS:
        if (s_group_count > 0) {
            s_selected_group = s_group_selection;
            s_list_kind = s_group_kind == GROUP_SERIES ? LIST_SERIES : LIST_AUTHOR;
            s_item_selection = 0;
            build_visible_items();
            s_screen = UI_BOOKS;
            s_dirty = true;
        }
        break;
    case UI_PLAYER:
        if (s_action_selection == 0) {
            back_one_level();
        } else if (s_action_selection == 1) {
            player_chapter_step(-1);
        } else if (s_action_selection == 2) {
            player_toggle();
        } else {
            player_chapter_step(1);
        }
        break;
    case UI_SLEEP_CONFIRM:
        if (s_sleep_choice == 0) player_toggle();
        s_sleep_listened_seconds = 0;
        s_sleep_prompt_deadline_us = 0;
        s_screen = UI_PLAYER;
        s_action_selection = 2;
        break;
    case UI_MESSAGE:
    case UI_SETUP:
        return_to_root();
        break;
    }
}

static void volume_turn(int delta)
{
    if (!delta) return;
    power_note_activity();
    s_last_user_action_us = esp_timer_get_time();
    player_status_t state;
    player_get_status(&state);
    player_set_volume(state.volume + delta * 2);
    s_volume_return_screen = s_screen;
    s_volume_until_us = esp_timer_get_time() + 1500000LL;
    s_dirty = true;
}

static void select_turn(int delta)
{
    if (!delta) return;
    power_note_activity();
    s_last_user_action_us = esp_timer_get_time();
    delta = accelerated_delta(delta);
    switch (s_screen) {
    case UI_ROOT: change_selection(&s_root_selection, root_menu_count(), delta); break;
    case UI_GROUPS: change_selection(&s_group_selection, s_group_count, delta); break;
    case UI_CONTINUE:
    case UI_BOOKS: change_selection(&s_item_selection, s_visible_count, delta); break;
    case UI_PLAYER: change_selection(&s_action_selection, 4, delta); break;
    case UI_SLEEP_CONFIRM: s_sleep_choice = (s_sleep_choice + (delta > 0 ? 1 : -1) + 2) % 2; break;
    default: break;
    }
    s_dirty = true;
}

static void handle_knob_click(chapter_knob_t knob)
{
    s_last_user_action_us = esp_timer_get_time();
    if (knob == CHAPTER_KNOB_SELECT) {
        select_action();
        return;
    }
    power_note_activity();
    player_status_t state;
    player_get_status(&state);
    if (state.state != PLAYER_IDLE && state.state != PLAYER_FINISHED) {
        player_toggle();
        if (s_screen != UI_PLAYER) s_player_return = s_screen;
        s_screen = UI_PLAYER;
        s_action_selection = 2;
    }
    s_dirty = true;
}

static void handle_knobs(void)
{
    chapter_knob_sample_t samples[CHAPTER_KNOB_COUNT];
    chapter_rotary_poll(CHAPTER_KNOB_SELECT, &samples[CHAPTER_KNOB_SELECT]);
    chapter_rotary_poll(CHAPTER_KNOB_VOLUME, &samples[CHAPTER_KNOB_VOLUME]);
    const int64_t now = esp_timer_get_time();
    const bool any_activity = samples[0].detents || samples[1].detents ||
                              samples[0].pressed_edge || samples[1].pressed_edge ||
                              samples[0].released_edge || samples[1].released_edge;

    if (!s_sleeping && power_screen_saver_active() && any_activity &&
        !(samples[0].pressed && samples[1].pressed)) {
        power_note_activity();
        s_last_user_action_us = now;
        s_input_armed = false;
        s_down_since[0] = s_down_since[1] = 0;
        s_long_handled[0] = s_long_handled[1] = false;
        s_both_gesture = false;
        s_dirty = true;
        return;
    }

    if (s_sleeping) {
        if (!any_activity) return;
        s_sleeping = false;
        s_sleep_listened_seconds = 0;
        s_sleep_prompt_deadline_us = 0;
        s_screen = UI_PLAYER;
        s_last_user_action_us = 0;
        power_note_activity();
        s_dirty = true;
        if (!(samples[0].pressed && samples[1].pressed)) {
            s_input_armed = false;
            s_down_since[0] = s_down_since[1] = 0;
            s_long_handled[0] = s_long_handled[1] = false;
            s_both_gesture = false;
            return;
        }
        s_input_armed = true; // Preserve the global two-knob shutdown gesture while waking.
        s_down_since[0] = s_down_since[1] = now;
        s_long_handled[0] = s_long_handled[1] = false;
        s_both_gesture = true;
    }

    if (!s_input_armed) {
        if (samples[0].pressed && samples[1].pressed) {
            s_input_armed = true;
            s_down_since[0] = s_down_since[1] = now;
            s_long_handled[0] = s_long_handled[1] = false;
            s_both_gesture = true;
        } else {
            if (!samples[0].pressed && !samples[1].pressed) s_input_armed = true;
            return;
        }
    }

    const bool both_pressed_now = samples[0].pressed && samples[1].pressed;
    if (both_pressed_now) s_both_gesture = true;
    for (int i = 0; i < CHAPTER_KNOB_COUNT; ++i) {
        if (samples[i].pressed_edge) {
            s_down_since[i] = now;
            s_long_handled[i] = false;
            power_note_activity();
            s_last_user_action_us = now;
        }
        if (samples[i].released_edge) {
            if (s_down_since[i] && !s_long_handled[i] && !s_both_gesture) handle_knob_click((chapter_knob_t)i);
            s_down_since[i] = 0;
            s_long_handled[i] = false;
        }
    }

    const bool both_pressed = both_pressed_now && s_down_since[0] && s_down_since[1];
    const int64_t both_down_since = s_down_since[0] > s_down_since[1] ? s_down_since[0] : s_down_since[1];
    if (both_pressed && now - both_down_since >= knob_hold_duration_us()) {
        s_long_handled[0] = s_long_handled[1] = true;
        s_shutdown_requested = true;
        return;
    }
    // Give the two-knob shutdown gesture priority even when the knobs were pressed a little apart.
    if (!samples[0].pressed && !samples[1].pressed) s_both_gesture = false;
    if (s_both_gesture) return;
    for (int i = 0; i < CHAPTER_KNOB_COUNT; ++i) {
        if (samples[i].pressed && s_down_since[i] && !s_long_handled[i] &&
            now - s_down_since[i] >= knob_hold_duration_us()) {
            s_long_handled[i] = true;
            s_last_user_action_us = now;
            if (i == CHAPTER_KNOB_SELECT) back_one_level();
            else return_to_root();
        }
    }

    if (samples[CHAPTER_KNOB_SELECT].detents) select_turn(samples[CHAPTER_KNOB_SELECT].detents);
    if (samples[CHAPTER_KNOB_VOLUME].detents) volume_turn(samples[CHAPTER_KNOB_VOLUME].detents);
}

static void render_message(void)
{
    char message[sizeof(s_message)];
    portENTER_CRITICAL(&s_message_lock);
    strlcpy(message, s_message, sizeof(message));
    portEXIT_CRITICAL(&s_message_lock);
    char *lines[DISPLAY_LABEL_COUNT] = {message, "", "", ""};
    for (int i = 1; i < DISPLAY_LABEL_COUNT; ++i) {
        char *break_at = strchr(lines[i - 1], '\n');
        if (!break_at) break;
        *break_at = 0;
        lines[i] = break_at + 1;
    }
    for (int i = 0; i < DISPLAY_LABEL_COUNT; ++i) set_label(i, lines[i]);
    if (screen_width() > 128) lv_obj_add_flag(s_labels[3], LV_OBJ_FLAG_HIDDEN);
}

static void render_idle_saver(void)
{
    power_config_t pc;
    power_get_config(&pc);
    power_apply_screen_saver(true);
    show_action_row(false);
    for (int i = 0; i < DISPLAY_LABEL_COUNT; ++i) lv_obj_add_flag(s_labels[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < SCREEN_SAVER_WORDS; ++i) lv_obj_add_flag(s_saver_labels[i], LV_OBJ_FLAG_HIDDEN);
    if (pc.screen_saver_mode == 3) return;

    if (pc.screen_saver_mode == 2) {
        time_t now = time(NULL);
        char clock_text[32] = "Waiting for network time";
        if (now > 100000) {
            struct tm local;
            localtime_r(&now, &local);
            strftime(clock_text, sizeof(clock_text), "%I:%M %p", &local);
            if (clock_text[0] == '0') memmove(clock_text, clock_text + 1, strlen(clock_text));
        }
        lv_obj_t *clock_label = s_saver_labels[0];
        lv_label_set_text(clock_label, clock_text);
        lv_obj_set_pos(clock_label, 0, 17);
        lv_obj_set_size(clock_label, screen_width(), 30);
        lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
        lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const uint32_t now_ms = tick_ms();
    if (!s_saver_started_ms) s_saver_started_ms = now_ms ? now_ms : 1;
    const uint32_t frame = (now_ms - s_saver_started_ms) / 125;
    const int width = screen_width();
    const int travel = width + 150;
    const int lanes = chapter_display_height() - 18;
    for (int i = 0; i < SCREEN_SAVER_WORDS; ++i) {
        lv_obj_t *label = s_saver_labels[i];
        const uint32_t speed = 2 + i % 4;
        const int x = (int)((frame * speed + i * 71) % travel) - 120;
        int lane = i * 11 + (int)lroundf(sinf((frame + i * 9) / 9.0f) * 5.0f);
        lane %= lanes;
        if (lane < 0) lane += lanes;
        const int y = 2 + lane;
        const uint8_t gray = (uint8_t)(95 + ((frame * 7 + i * 43) % 160));
        const lv_font_t *font = ((i + frame / 96) & 1) ? &lv_font_montserrat_14 : &lv_font_montserrat_16;
        const char *text = "chapter";
        if (pc.screen_saver_mode == 1 && s_books && s_book_count > 0) {
            const int book_index = (i + frame / 96) % s_book_count;
            text = s_books[book_index].title ? s_books[book_index].title : "Untitled";
        }
        if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_make(gray, gray, gray), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_point_t extent;
        lv_text_get_size(&extent, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_size(label, extent.x + 2, font->line_height + 2);
        lv_obj_set_pos(label, x, y);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_sleep_timer(int64_t now)
{
    player_status_t state;
    player_get_status(&state);
    const app_config_t *cfg = config_get();
    const int64_t elapsed_us = s_sleep_last_check_us ? now - s_sleep_last_check_us : 0;
    s_sleep_last_check_us = now;
    const bool has_session = state.item_id[0] && state.state != PLAYER_IDLE &&
                             state.state != PLAYER_FINISHED && state.state != PLAYER_ERROR;
    if (!cfg->sleep_timer_enabled || !has_session) {
        s_sleep_listened_seconds = 0;
        s_sleep_item_id[0] = 0;
        if (s_sleeping) {
            s_sleeping = false;
            s_screen = has_session ? UI_PLAYER : UI_ROOT;
            s_dirty = true;
        }
        if (s_sleep_prompt_deadline_us) {
            s_sleep_prompt_deadline_us = 0;
            s_screen = has_session ? UI_PLAYER : UI_ROOT;
            s_dirty = true;
        }
        return;
    }
    if (strcmp(s_sleep_item_id, state.item_id) != 0) {
        strlcpy(s_sleep_item_id, state.item_id, sizeof(s_sleep_item_id));
        s_sleep_listened_seconds = 0;
        s_sleep_prompt_deadline_us = 0;
        s_sleeping = false;
    }
    if (state.state == PLAYER_PLAYING) s_sleep_listened_seconds += elapsed_us / 1000000.0;
    if (s_sleep_prompt_deadline_us) {
        if (state.state == PLAYER_PLAYING && now - s_sleep_prompt_started_us > 750000) {
            // A web control or the volume-knob click resumed playback during the warning.
            s_sleep_prompt_deadline_us = 0;
            s_sleep_listened_seconds = 0;
            s_screen = UI_PLAYER;
            s_dirty = true;
        } else if (now >= s_sleep_prompt_deadline_us) {
            // The Pi keeps the player paused and shows the configured screen saver until a knob wakes it.
            s_sleep_prompt_deadline_us = 0;
            s_sleep_listened_seconds = 0;
            s_screen = UI_PLAYER;
            s_sleeping = true;
            s_dirty = true;
        }
        return;
    }
    if (s_sleep_listened_seconds >= cfg->sleep_timer_minutes * 60.0) {
        if (state.state == PLAYER_PLAYING || state.state == PLAYER_BUFFERING) player_toggle();
        s_sleep_prompt_started_us = now;
        s_sleep_prompt_deadline_us = now + 30 * 1000000LL;
        s_sleep_choice = 0;
        s_screen = UI_SLEEP_CONFIRM;
        s_last_user_action_us = now;
        power_note_activity();
        s_dirty = true;
    }
}

static void render_screen(void)
{
    if (!s_ready) return;
    const uint32_t now_ms = tick_ms();
    if (s_sleeping) {
        render_idle_saver();
        s_last_render_ms = now_ms;
        return;
    }
    if (power_screen_saver_active()) {
        render_idle_saver();
        s_last_render_ms = now_ms;
        return;
    }
    power_apply_screen_saver(false);
    s_saver_started_ms = 0;
    for (int i = 0; i < SCREEN_SAVER_WORDS; ++i) lv_obj_add_flag(s_saver_labels[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < DISPLAY_LABEL_COUNT; ++i) lv_obj_remove_flag(s_labels[i], LV_OBJ_FLAG_HIDDEN);
    if (esp_timer_get_time() < s_volume_until_us) {
        show_action_row(false);
        lv_obj_remove_flag(s_labels[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_labels[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_labels[2], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_labels[3], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_volume_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_volume_bar_fill, LV_OBJ_FLAG_HIDDEN);
        player_status_t state;
        player_get_status(&state);
        char top[80];
        snprintf(top, sizeof(top), "Volume  %d%%", state.volume);
        set_label(0, top);
        update_volume_bar(state.volume);
        s_last_render_ms = now_ms;
        return;
    } else if (s_volume_until_us) {
        s_volume_until_us = 0;
        lv_obj_add_flag(s_volume_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_volume_bar_fill, LV_OBJ_FLAG_HIDDEN);
        s_screen = (ui_screen_t)s_volume_return_screen;
        s_dirty = true;
    }

    if (s_screen == UI_MESSAGE || s_screen == UI_SETUP) {
        show_action_row(false);
        lv_obj_remove_flag(s_labels[2], LV_OBJ_FLAG_HIDDEN);
        render_message();
        s_last_render_ms = now_ms;
        return;
    }

    char line1[192] = "", line2[192] = "", line3[192] = "";
    bool playing_screen = false;
    bool playing_now = false;
    switch (s_screen) {
    case UI_ROOT: {
        const int id = root_item_at(s_root_selection);
        snprintf(line1, sizeof(line1), "%s", s_root_items[id]);
        if (id == 0) {
            int in_progress = 0;
            for (int i = 0; i < s_book_count; ++i) if (!s_books[i].podcast && book_in_progress(&s_books[i])) ++in_progress;
            snprintf(line2, sizeof(line2), "%d book%s in progress", in_progress, in_progress == 1 ? "" : "s");
        } else if (id == 1) {
            build_groups(GROUP_SERIES);
            snprintf(line2, sizeof(line2), "%d series", s_group_count);
        } else if (id == 2) {
            int books = 0;
            for (int i = 0; i < s_book_count; ++i) if (!s_books[i].podcast) ++books;
            snprintf(line2, sizeof(line2), "%d books", books);
        } else if (id == 3) {
            build_groups(GROUP_AUTHOR);
            snprintf(line2, sizeof(line2), "%d authors", s_group_count);
        } else if (id == 5) {
            strlcpy(line2, "to turn off device", sizeof(line2));
        }
        break;
    }
    case UI_CONTINUE:
    case UI_BOOKS: {
        if (s_visible_count == 0) {
            snprintf(line1, sizeof(line1), "%s", s_screen == UI_CONTINUE ? "Nothing in progress" : "No books found");
            snprintf(line2, sizeof(line2), "Hold either knob for menu");
            break;
        }
        const abs_book_t *book = &s_books[s_visible[s_item_selection]];
        snprintf(line1, sizeof(line1), "%s", book->title ? book->title : "Untitled");
        if (s_screen == UI_CONTINUE) {
            float progress = book->progress;
            if (progress <= 0 && book->duration > 0) progress = book->current_time / book->duration;
            const int percent = (int)(progress * 100 + 0.5f);
            if (book->duration <= 0) {
                snprintf(line2, sizeof(line2), "%d%% listened", percent);
            } else {
                int seconds = (int)(book->duration - book->current_time);
                if (seconds < 0) seconds = 0;
                const int hours = seconds / 3600, minutes = (seconds % 3600) / 60;
                if (hours && minutes) snprintf(line2, sizeof(line2), "%d%%  %dh %dm left", percent, hours, minutes);
                else if (hours) snprintf(line2, sizeof(line2), "%d%%  %dh left", percent, hours);
                else if (minutes) snprintf(line2, sizeof(line2), "%d%%  %dm left", percent, minutes);
                else snprintf(line2, sizeof(line2), "%d%%  <1m left", percent);
            }
        } else if (s_list_kind == LIST_SERIES) {
            snprintf(line2, sizeof(line2), "Series: %.160s", s_groups[s_selected_group].name);
            if (book->series_sequence[0] && book->series_total >= 2)
                snprintf(line3, sizeof(line3), "%s/%d", book->series_sequence, book->series_total);
        } else if (s_list_kind == LIST_AUTHOR) {
            snprintf(line2, sizeof(line2), "Author: %.160s", s_groups[s_selected_group].name);
        } else {
            snprintf(line2, sizeof(line2), "By %.170s", primary_author(book->author));
        }
        break;
    }
    case UI_GROUPS: {
        if (s_group_count == 0) {
            snprintf(line1, sizeof(line1), "No %s found", s_group_kind == GROUP_SERIES ? "series" : "authors");
            break;
        }
        const group_entry_t *group = &s_groups[s_group_selection];
        snprintf(line1, sizeof(line1), "%s", group->name);
        snprintf(line2, sizeof(line2), "%d book%s", group->count, group->count == 1 ? "" : "s");
        break;
    }
    case UI_PLAYER: {
        player_status_t state;
        player_get_status(&state);
        snprintf(line1, sizeof(line1), "%s", state.chapter[0] ? state.chapter : state.title);
        const int percent = state.duration > 0 ? (int)(100.0 * state.position / state.duration) : 0;
        char remaining[24];
        snprintf(line2, sizeof(line2), "%s left  ·  %d%%", format_remaining(state.duration - state.position, remaining, sizeof(remaining)), percent);
        playing_screen = true;
        playing_now = state.state == PLAYER_PLAYING || state.state == PLAYER_BUFFERING;
        break;
    }
    case UI_SLEEP_CONFIRM: {
        const int seconds = s_sleep_prompt_deadline_us > esp_timer_get_time()
                                ? (int)((s_sleep_prompt_deadline_us - esp_timer_get_time() + 999999) / 1000000)
                                : 0;
        strlcpy(line1, "Still listening?", sizeof(line1));
        snprintf(line2, sizeof(line2), "Continue listening?  %ds", seconds);
        snprintf(line3, sizeof(line3), "%s  %s", s_sleep_choice == 0 ? "[Continue]" : "Continue",
                 s_sleep_choice == 1 ? "[Stay paused]" : "Stay paused");
        break;
    }
    case UI_MESSAGE:
    case UI_SETUP: break;
    }
    const bool compact_display = screen_width() <= 128;
    const int footer_label = compact_display ? 3 : 2;
    if (compact_display) {
        char title_first[192], title_second[192];
        split_title_lines(line1, title_first, sizeof(title_first), title_second, sizeof(title_second));
        set_label(0, title_first);
        set_label(1, title_second);
        set_label(2, line2);
        set_label(3, line3);
    } else {
        set_label(0, line1);
        set_label(1, line2);
        set_label(2, line3);
        set_label(3, "");
        lv_obj_add_flag(s_labels[3], LV_OBJ_FLAG_HIDDEN);
    }
    const bool show_series_position = s_screen == UI_BOOKS && s_list_kind == LIST_SERIES && line3[0];
    lv_obj_set_style_text_align(s_labels[footer_label], show_series_position ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(s_labels[footer_label], show_series_position ? 3 : 0, 0);
    if (playing_screen) {
        lv_obj_add_flag(s_labels[footer_label], LV_OBJ_FLAG_HIDDEN);
        if (compact_display) lv_obj_add_flag(s_labels[3], LV_OBJ_FLAG_HIDDEN);
        show_action_row(true);
        set_action_row(s_action_selection, playing_now);
    } else {
        lv_obj_remove_flag(s_labels[footer_label], LV_OBJ_FLAG_HIDDEN);
        show_action_row(false);
    }
    s_last_render_ms = now_ms;
}

void ui_init(void)
{
    s_groups = heap_caps_calloc(UI_MAX_GROUPS, sizeof(*s_groups), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_groups) {
        ESP_LOGE(TAG, "unable to allocate the browse index in PSRAM");
        return;
    }
    if (chapter_display_init() != ESP_OK) return;
    if (chapter_rotary_init() != ESP_OK) return;
    lv_init();
    lv_style_init(&s_scroll_style);
    lv_anim_init(&s_scroll_anim);
    lv_anim_set_delay(&s_scroll_anim, config_get()->marquee_delay_ms);
    lv_anim_set_repeat_delay(&s_scroll_anim, config_get()->marquee_loop_pause_ms);
    lv_style_set_anim(&s_scroll_style, &s_scroll_anim);
    lv_tick_set_cb(tick_ms);
    const int width = chapter_display_width();
    const int height = chapter_display_height();
    s_display = lv_display_create(width, height);
    if (!s_display) return;
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    const size_t frame_bytes = (size_t)width * height * sizeof(uint16_t);
    s_lvgl_buffer = heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lvgl_buffer) s_lvgl_buffer = malloc(frame_bytes);
    if (!s_lvgl_buffer) return;
    lv_display_set_buffers(s_display, s_lvgl_buffer, NULL, frame_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_display, chapter_display_flush);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_labels[0] = lv_label_create(screen);
    s_labels[1] = lv_label_create(screen);
    s_labels[2] = lv_label_create(screen);
    s_labels[3] = lv_label_create(screen);
    if (width <= 128) {
        label_setup(s_labels[0], &lv_font_montserrat_12, 0, 16);
        label_setup(s_labels[1], &lv_font_montserrat_12, 16, 16);
        label_setup(s_labels[2], &lv_font_montserrat_12, 32, 16);
        label_setup(s_labels[3], &lv_font_montserrat_12, 48, 16);
    } else {
        label_setup(s_labels[0], &lv_font_montserrat_16, 4, 22);
        label_setup(s_labels[1], &lv_font_montserrat_14, 27, 18);
        label_setup(s_labels[2], &lv_font_montserrat_14, 48, 16);
        label_setup(s_labels[3], &lv_font_montserrat_14, 48, 16);
        lv_obj_add_flag(s_labels[3], LV_OBJ_FLAG_HIDDEN);
    }
    s_volume_bar = lv_obj_create(screen);
    const int bar_width = width * 80 / 100;
    const int bar_height = 14;
    lv_obj_set_pos(s_volume_bar, (width - bar_width) / 2, 42);
    lv_obj_set_size(s_volume_bar, bar_width, bar_height);
    lv_obj_remove_flag(s_volume_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_volume_bar, 0, 0);
    lv_obj_set_style_radius(s_volume_bar, 4, 0);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_volume_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_volume_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_volume_bar, 1, 0);
    lv_obj_set_style_shadow_width(s_volume_bar, 0, 0);
    s_volume_bar_fill = lv_obj_create(s_volume_bar);
    lv_obj_set_pos(s_volume_bar_fill, 2, 2);
    lv_obj_set_size(s_volume_bar_fill, 0, bar_height - 4);
    lv_obj_remove_flag(s_volume_bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_volume_bar_fill, 0, 0);
    lv_obj_set_style_radius(s_volume_bar_fill, 2, 0);
    lv_obj_set_style_bg_color(s_volume_bar_fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_volume_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_bar_fill, 0, 0);
    lv_obj_set_style_shadow_width(s_volume_bar_fill, 0, 0);
    lv_obj_add_flag(s_volume_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_volume_bar_fill, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; ++i) {
        s_action_labels[i] = lv_label_create(screen);
        const int cell_width = width / 4;
        lv_obj_set_pos(s_action_labels[i], i * cell_width + 1, 44);
        lv_obj_set_size(s_action_labels[i], cell_width - 2, 17);
        lv_label_set_long_mode(s_action_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(s_action_labels[i], width <= 128 ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_action_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(s_action_labels[i], 4, 0);
        lv_obj_set_style_pad_hor(s_action_labels[i], 1, 0);
        lv_obj_set_style_pad_ver(s_action_labels[i], 1, 0);
        lv_obj_add_flag(s_action_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < SCREEN_SAVER_WORDS; ++i) {
        s_saver_labels[i] = lv_label_create(screen);
        lv_label_set_long_mode(s_saver_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(s_saver_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_ready = true;
    s_dirty = true;
    s_applied_marquee_delay_ms = config_get()->marquee_delay_ms;
    s_applied_marquee_pause_ms = config_get()->marquee_loop_pause_ms;
    s_applied_marquee_speed_px_s = config_get()->marquee_speed_px_s;
    ESP_LOGI(TAG, "Chapter two-knob interface initialized");
}

void ui_process(void)
{
    if (!s_ready) return;
    bool message_pending;
    char current_message[sizeof(s_message)];
    portENTER_CRITICAL(&s_message_lock);
    message_pending = s_message_pending;
    s_message_pending = false;
    if (message_pending) strlcpy(current_message, s_message, sizeof(current_message));
    portEXIT_CRITICAL(&s_message_lock);
    if (message_pending) {
        if (current_message[0]) s_screen = UI_MESSAGE;
        else if (s_screen == UI_MESSAGE) s_screen = s_setup_mode ? UI_SETUP : UI_ROOT;
        s_dirty = true;
    }
    apply_live_marquee_settings();
    if (s_screen == UI_ROOT && s_root_selection >= root_menu_count()) s_root_selection = 0;
    handle_knobs();
    update_sleep_timer(esp_timer_get_time());
    const app_config_t *cfg = config_get();
    if (cfg->browse_return_s > 0 && s_screen != UI_PLAYER && s_last_user_action_us &&
        esp_timer_get_time() - s_last_user_action_us >= (int64_t)cfg->browse_return_s * 1000000LL) {
        player_status_t state;
        player_get_status(&state);
        if (state.state == PLAYER_PLAYING || state.state == PLAYER_BUFFERING) {
            s_screen = UI_PLAYER;
            s_dirty = true;
            s_last_user_action_us = 0;
        }
    }
    const uint32_t now = tick_ms();
    const bool saver_active = s_sleeping || power_screen_saver_active();
    const uint32_t render_interval = saver_active ? 125 : 500;
    if (s_dirty || now - s_last_render_ms >= render_interval) {
        render_screen();
        s_dirty = false;
    }
    lv_timer_handler();
}

void ui_show_message(const char *message)
{
    ui_show_message_locked(message);
}

void ui_show_message_locked(const char *message)
{
    portENTER_CRITICAL(&s_message_lock);
    text_copy(s_message, sizeof(s_message), message);
    s_message_pending = true;
    portEXIT_CRITICAL(&s_message_lock);
}

void ui_show_notice(const char *icon, const char *message)
{
    (void)icon;
    ui_show_message_locked(message);
}

void ui_toggle_play(void)
{
    s_sleeping = false;
    player_toggle();
    s_player_return = UI_ROOT;
    s_screen = UI_PLAYER;
    s_dirty = true;
}

void ui_show_player(void)
{
    player_status_t state;
    player_get_status(&state);
    if (!state.item_id[0]) return;
    s_sleeping = false;
    if (s_screen != UI_PLAYER) s_player_return = s_screen;
    s_screen = UI_PLAYER;
    s_action_selection = 2;
    s_last_user_action_us = 0;
    power_note_activity();
    s_dirty = true;
}

void ui_set_books(const abs_book_t *books, int count)
{
    s_books = books;
    s_book_count = count > UI_MAX_BOOKS ? UI_MAX_BOOKS : count;
    s_root_selection = 0;
    s_item_selection = 0;
    s_group_count = 0;
    build_continue_items();
    ++s_library_revision;
    if (!s_library_revision) ++s_library_revision;
    const app_config_t *cfg = config_get();
    if (!s_startup_resume_done && cfg->resume_last_at_startup && s_books && s_book_count > 0) {
        int latest = -1;
        for (int i = 0; i < s_book_count; ++i) {
            const abs_book_t *book = &s_books[i];
            if (book->podcast || book->finished || book->current_time <= 0 || book->progress >= 0.995f) continue;
            if (latest < 0 || book->last_update > s_books[latest].last_update) latest = i;
        }
        // If a cached library has no saved progress, leave this pending so a later network load
        // can supply the account's most recent book. Otherwise, only auto-resume once per boot.
        if (latest >= 0) {
            s_startup_resume_done = true;
            s_player_return = UI_ROOT;
            player_open(&s_books[latest]);
            s_action_selection = 2;
            s_screen = UI_PLAYER;
            // A boot-time connection/loading message should not replace the requested resume
            // screen before the first UI frame is rendered.
            portENTER_CRITICAL(&s_message_lock);
            s_message[0] = 0;
            s_message_pending = true;
            portEXIT_CRITICAL(&s_message_lock);
        }
    }
    s_dirty = true;
}

void ui_set_source(bool from_cache)
{
    s_from_cache = from_cache;
}

void ui_get_source(uint32_t *library_revision, bool *from_cache)
{
    if (library_revision) *library_revision = s_library_revision;
    if (from_cache) *from_cache = s_from_cache;
}

void ui_set_libraries(const abs_library_t *libraries, int count, const char *selected_id)
{
    s_library_count = count > (int)(sizeof(s_libraries) / sizeof(s_libraries[0]))
                          ? sizeof(s_libraries) / sizeof(s_libraries[0]) : count;
    if (libraries && s_library_count > 0) memcpy(s_libraries, libraries, s_library_count * sizeof(s_libraries[0]));
    strlcpy(s_selected_library, selected_id ? selected_id : "", sizeof(s_selected_library));
}

void ui_request_library(const char *library_id)
{
    strlcpy(s_library_request, library_id ? library_id : "", sizeof(s_library_request));
}

bool ui_take_library_request(char *library_id, size_t length)
{
    if (!s_library_request[0]) return false;
    strlcpy(library_id, s_library_request, length);
    s_library_request[0] = 0;
    return true;
}

void ui_setup_show(void)
{
    char ssid[40] = "Chapter setup";
    char password[24] = "";
    portal_network(ssid, sizeof(ssid), password, sizeof(password));
    snprintf(s_message, sizeof(s_message), "Wi-Fi: %.40s\nPass: %.24s\nOpen 192.168.4.1", ssid, password);
    s_setup_mode = true;
    s_screen = UI_SETUP;
    s_dirty = true;
}

void ui_request_refresh(void)
{
    s_refresh_requested = true;
}

bool ui_take_refresh_request(void)
{
    const bool requested = s_refresh_requested;
    s_refresh_requested = false;
    return requested;
}

bool ui_take_shutdown_request(void)
{
    const bool requested = s_shutdown_requested;
    s_shutdown_requested = false;
    return requested;
}

