#ifndef DIGITAL_LOCK_H
#define DIGITAL_LOCK_H

#include <stdbool.h>

#define LOCK_CODE_SIZE           4
#define LOCK_MAX_FAILED_ATTEMPTS 3

// Lock operating state
typedef enum {
    LOCK_STATE_ACTIVE,
    LOCK_STATE_DISABLED
} lock_state_t;

// Knob rotational direction state
typedef enum {
    DIRECTION_NONE,
    DIRECTION_RIGHT,
    DIRECTION_LEFT
} lock_direction_t;

// Digital Lock instance instance structure
typedef struct {
    int code[LOCK_CODE_SIZE];
    int input[LOCK_CODE_SIZE];
    int counter;
    int value;
    int failed_attempts;
    lock_state_t state;
    lock_direction_t direction;
} digital_lock_t;

/**
 * @brief Initialize a digital lock instance with a target code.
 *
 * @param lock Pointer to digital_lock_t instance
 * @param preset_code Array containing secret combination of size LOCK_CODE_SIZE
 */
void digital_lock_init(digital_lock_t *lock, const int preset_code[LOCK_CODE_SIZE]);

/**
 * @brief Reset input state and reactivate the lock.
 *
 * @param lock Pointer to digital_lock_t instance
 */
void digital_lock_reset(digital_lock_t *lock);

#endif // DIGITAL_LOCK_H