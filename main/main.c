#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_attr.h"

#include <stdbool.h>
#include <stdint.h>

#include "DigitalLock.h"

// ==================================================
// GPIO Configuration
// ==================================================

#define ENCODER_CLK GPIO_NUM_4
#define ENCODER_DT  GPIO_NUM_5
#define ENCODER_SW  GPIO_NUM_6

#define BUZZER_GPIO GPIO_NUM_10

#define LED_1 GPIO_NUM_15
#define LED_2 GPIO_NUM_16
#define LED_3 GPIO_NUM_17
#define LED_4 GPIO_NUM_18

static const char *TAG = "MAIN";

static digital_lock_t lock;

typedef enum {
    ENCODER_EVENT_RIGHT,
    ENCODER_EVENT_LEFT
} encoder_event_t;

// Encoder state is shared between ISR and main task
static volatile uint8_t previous_encoder_state;
static volatile int8_t encoder_accumulator;

static volatile encoder_event_t encoder_event;
static volatile bool encoder_event_pending;

// When true, encoder events are discarded
static volatile bool encoder_ignore_events;

// Quadrature transition table
static const DRAM_ATTR int8_t transition_table[4][4] = {
    {  0, -1, +1,  0 },
    { +1,  0,  0, -1 },
    { -1,  0,  0, +1 },
    {  0, +1, -1,  0 }
};

// ==================================================
// Buzzer
// ==================================================

static void buzzer_set_frequency(int frequency)
{
    ledc_set_freq(
        LEDC_LOW_SPEED_MODE,
        LEDC_TIMER_0,
        frequency
    );
}

static void buzzer_on(void)
{
    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0,
        512
    );

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0
    );
}

static void buzzer_off(void)
{
    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0,
        0
    );

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        LEDC_CHANNEL_0
    );
}

static void buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

// ==================================================
// Encoder Event Blocking
// ==================================================

static void encoder_ignore_start(void)
{
    encoder_ignore_events = true;

    // Throw away anything that might already be waiting.
    encoder_event_pending = false;
}

static void encoder_ignore_stop(void)
{
    encoder_event_pending = false;
    encoder_ignore_events = false;
}

// ==================================================
// Buzzer Sounds
// ==================================================

static void beep(void)
{
    encoder_ignore_start();

    buzzer_set_frequency(2000);
    buzzer_on();

    vTaskDelay(pdMS_TO_TICKS(30));

    buzzer_off();

    encoder_ignore_stop();
}

static void angry_buzz(void)
{
    encoder_ignore_start();

    buzzer_set_frequency(180);
    buzzer_on();

    vTaskDelay(pdMS_TO_TICKS(180));

    buzzer_set_frequency(120);

    vTaskDelay(pdMS_TO_TICKS(220));

    buzzer_off();

    vTaskDelay(pdMS_TO_TICKS(80));

    buzzer_set_frequency(100);
    buzzer_on();

    vTaskDelay(pdMS_TO_TICKS(300));

    buzzer_off();

    encoder_ignore_stop();
}

static void play_note(int frequency, int duration_ms)
{
    buzzer_set_frequency(frequency);
    buzzer_on();

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    buzzer_off();

    vTaskDelay(pdMS_TO_TICKS(30));
}

static void success_melody(void)
{
    encoder_ignore_start();

    // C5 D5 E5 G5 C6
    play_note(523, 100);
    play_note(587, 100);
    play_note(659, 100);
    play_note(784, 130);
    play_note(1047, 250);

    vTaskDelay(pdMS_TO_TICKS(50));

    // Flourish
    play_note(784, 80);
    play_note(1047, 300);

    encoder_ignore_stop();
}

// ==================================================
// LED Functions
// ==================================================

static void update_leds(void)
{
    gpio_set_level(LED_1, lock.counter >= 1);
    gpio_set_level(LED_2, lock.counter >= 2);
    gpio_set_level(LED_3, lock.counter >= 3);
    gpio_set_level(LED_4, lock.counter >= 4);
}

static void leds_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask =
            (1ULL << LED_1) |
            (1ULL << LED_2) |
            (1ULL << LED_3) |
            (1ULL << LED_4),

        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    update_leds();
}

// ==================================================
// Encoder Hardware
// ==================================================

static uint8_t encoder_read_state(void)
{
    int clk = gpio_get_level(ENCODER_CLK);
    int dt  = gpio_get_level(ENCODER_DT);

    return (clk << 1) | dt;
}

// ==================================================
// Encoder ISR
// ==================================================

static void IRAM_ATTR encoder_isr_handler(void *arg)
{
    uint8_t current_state = encoder_read_state();

    if (current_state == previous_encoder_state) {
        return;
    }

    int8_t movement =
        transition_table[
            previous_encoder_state
        ][
            current_state
        ];

    if (movement != 0) {

        encoder_accumulator += movement;

        // One complete encoder step
        if (encoder_accumulator >= 4) {

            encoder_accumulator = 0;

            if (!encoder_ignore_events &&
                !encoder_event_pending) {

                encoder_event = ENCODER_EVENT_RIGHT;
                encoder_event_pending = true;
            }
        }
        else if (encoder_accumulator <= -4) {

            encoder_accumulator = 0;

            if (!encoder_ignore_events &&
                !encoder_event_pending) {

                encoder_event = ENCODER_EVENT_LEFT;
                encoder_event_pending = true;
            }
        }
    }

    previous_encoder_state = current_state;
}

// ==================================================
// Encoder Initialization
// ==================================================

static void encoder_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask =
            (1ULL << ENCODER_CLK) |
            (1ULL << ENCODER_DT),

        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        // Interrupt on every transition
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    previous_encoder_state = encoder_read_state();
    encoder_accumulator = 0;

    encoder_event_pending = false;
    encoder_ignore_events = false;

    ESP_ERROR_CHECK(
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM)
    );

    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            ENCODER_CLK,
            encoder_isr_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            ENCODER_DT,
            encoder_isr_handler,
            NULL
        )
    );
}

// ==================================================
// Button
// ==================================================

static int previous_button_state;
static TickType_t last_button_time;

static void button_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << ENCODER_SW),

        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,

        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    previous_button_state =
        gpio_get_level(ENCODER_SW);

    last_button_time = 0;
}

static void reset_lock_app(void)
{
    digital_lock_reset(&lock);

    update_leds();

    beep();
}

static void button_process(void)
{
    int current_button_state =
        gpio_get_level(ENCODER_SW);

    if (current_button_state == previous_button_state) {
        return;
    }

    TickType_t now = xTaskGetTickCount();

    if ((now - last_button_time) <= pdMS_TO_TICKS(50)) {
        return;
    }

    last_button_time = now;

    if (current_button_state == 0) {
        reset_lock_app();
    }

    previous_button_state = current_button_state;
}

static void check_code(void)
{
    bool correct = true;

    for (int i = 0; i < LOCK_CODE_SIZE; i++) {

        ESP_LOGI(
            TAG,
            "input[%d] = %d",
            i,
            lock.input[i]
        );

        if (lock.input[i] != lock.code[i]) {
            correct = false;
        }
    }

    if (correct) {

        ESP_LOGI(TAG, "====================");
        ESP_LOGI(TAG, "CODE CORRECT!");
        ESP_LOGI(TAG, "LOCK DISABLED");
        ESP_LOGI(TAG, "====================");

        lock.state = LOCK_STATE_DISABLED;

        success_melody();

        return;
    }

    lock.failed_attempts++;

    ESP_LOGI(
        TAG,
        "CODE INCORRECT! Failed attempts: %d/%d",
        lock.failed_attempts,
        LOCK_MAX_FAILED_ATTEMPTS
    );

    angry_buzz();
    lock.counter = 0;
    update_leds();
    if (lock.failed_attempts >= LOCK_MAX_FAILED_ATTEMPTS) {

        ESP_LOGI(TAG, "====================");
        ESP_LOGI(TAG, "TOO MANY FAILURES");
        ESP_LOGI(TAG, "LOCK DISABLED");
        ESP_LOGI(TAG, "====================");

        lock.state = LOCK_STATE_DISABLED;
    }
}

static void record_value(void)
{
    if (lock.counter >= LOCK_CODE_SIZE) {
        return;
    }

    lock.input[lock.counter] = lock.value;

    ESP_LOGI(
        TAG,
        "Recorded input[%d] = %d",
        lock.counter,
        lock.value
    );

    lock.counter++;

    update_leds();

    beep();

    lock.value = 0;

    if (lock.counter == LOCK_CODE_SIZE) {

        ESP_LOGI(TAG, "Input complete!");

        check_code();
    }
}

static void encoder_step(lock_direction_t new_direction)
{
    if (lock.state == LOCK_STATE_DISABLED) {
        return;
    }

    // First movement establishes direction
    if (lock.direction == DIRECTION_NONE) {

        lock.direction = new_direction;
        lock.value = 0;

        ESP_LOGI(
            TAG,
            "Initial direction: %s",
            lock.direction == DIRECTION_RIGHT
                ? "RIGHT"
                : "LEFT"
        );

        return;
    }

    // Continue in same direction
    if (new_direction == lock.direction) {

        lock.value++;

        ESP_LOGI(
            TAG,
            "Direction: %s, value = %d",
            lock.direction == DIRECTION_RIGHT
                ? "RIGHT"
                : "LEFT",
            lock.value
        );

        return;
    }

    // Direction changed
    // Record previous value
    ESP_LOGI(
        TAG,
        "Direction changed, recording value = %d",
        lock.value
    );

    record_value();

    if (lock.state == LOCK_STATE_DISABLED) {

        lock.direction = DIRECTION_NONE;
        lock.value = 0;

        return;
    }

    lock.direction = new_direction;
    lock.value = 0;

    ESP_LOGI(
        TAG,
        "New direction: %s",
        lock.direction == DIRECTION_RIGHT
            ? "RIGHT"
            : "LEFT"
    );
}

// ==================================================
// Encoder Event Processing
// ==================================================

static void encoder_process_events(void)
{
    if (!encoder_event_pending) {
        return;
    }

    // Copy event locally
    encoder_event_t event = encoder_event;

    // Mark it as consumed BEFORE processing it.
    encoder_event_pending = false;

    if (event == ENCODER_EVENT_RIGHT) {
        encoder_step(DIRECTION_RIGHT);
    }
    else {
        encoder_step(DIRECTION_LEFT);
    }
}

// ==================================================
// Application Initialization
// ==================================================

static void lock_app_init(void)
{
    const int secret_code[LOCK_CODE_SIZE] = {
        3, 5, 8, 1
    };

    digital_lock_init(
        &lock,
        secret_code
    );
}

static void hardware_init(void)
{
    encoder_init();
    button_init();

    buzzer_init();
    leds_init();
}

// ==================================================
// Main
// ==================================================

void app_main(void)
{
    lock_app_init();
    hardware_init();

    ESP_LOGI(
        TAG,
        "Digital lock started"
    );

    while (1) {

        encoder_process_events();

        // Button remains polling-based.
        button_process();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}