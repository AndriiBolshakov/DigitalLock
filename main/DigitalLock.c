#include "DigitalLock.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "DIGITAL_LOCK";

void digital_lock_init(digital_lock_t *lock, const int preset_code[LOCK_CODE_SIZE])
{
    if (!lock) return;

    if (preset_code) {
        memcpy(lock->code, preset_code, sizeof(int) * LOCK_CODE_SIZE);
    } else {
        // Fallback default combination
        const int default_code[LOCK_CODE_SIZE] = {3, 5, 8, 1};
        memcpy(lock->code, default_code, sizeof(int) * LOCK_CODE_SIZE);
    }

    digital_lock_reset(lock);
    ESP_LOGI(TAG, "Digital lock initialized");
}

void digital_lock_reset(digital_lock_t *lock)
{
    if (!lock) return;

    lock->counter = 0;
    lock->value = 0;
    lock->direction = DIRECTION_NONE;
    lock->failed_attempts = 0;
    lock->state = LOCK_STATE_ACTIVE;

    memset(lock->input, 0, sizeof(lock->input));

    ESP_LOGI(TAG, "Digital lock state reset");
}