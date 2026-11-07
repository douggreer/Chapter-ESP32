#include "device_history.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_attr.h"

// RTC slow memory is retained through deep sleep and cleared on a full power-on. That lets the
// local recent list survive battery-saving sleep without writing history to flash.
RTC_DATA_ATTR static device_history_entry_t s_entries[DEVICE_HISTORY_LIMIT];
RTC_DATA_ATTR static int s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void device_history_add(const char *id, const char *title, const char *author)
{
    if (!id || !id[0]) return;
    portENTER_CRITICAL(&s_lock);
    int found = -1;
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_entries[i].id, id) == 0) { found = i; break; }
    }
    if (found < 0) {
        if (s_count < DEVICE_HISTORY_LIMIT) ++s_count;
        found = s_count - 1;
    }
    for (int i = found; i > 0; --i) s_entries[i] = s_entries[i - 1];
    strlcpy(s_entries[0].id, id, sizeof(s_entries[0].id));
    strlcpy(s_entries[0].title, title ? title : "", sizeof(s_entries[0].title));
    strlcpy(s_entries[0].author, author ? author : "", sizeof(s_entries[0].author));
    portEXIT_CRITICAL(&s_lock);
}

int device_history_copy(device_history_entry_t *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    portENTER_CRITICAL(&s_lock);
    int count = s_count;
    if (count > (int)capacity) count = (int)capacity;
    memcpy(out, s_entries, count * sizeof(*out));
    portEXIT_CRITICAL(&s_lock);
    return count;
}
