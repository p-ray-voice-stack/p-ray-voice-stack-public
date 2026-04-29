#include "trigger_input.h"

#include "config.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trigger_input";

const char *trigger_input_source_name(trigger_event_type_t type)
{
    switch (type) {
    case TRIGGER_EVENT_BUTTON:
        return "button";
    case TRIGGER_EVENT_TOUCH:
        return "touch";
    case TRIGGER_EVENT_WAKE_WORD:
        return "wake_word";
    case TRIGGER_EVENT_NONE:
    default:
        return "none";
    }
}

trigger_event_type_t trigger_input_configured_source(void)
{
    switch (DEMO_TRIGGER_SOURCE) {
    case DEMO_TRIGGER_SOURCE_BUTTON:
        return TRIGGER_EVENT_BUTTON;
    case DEMO_TRIGGER_SOURCE_TOUCH:
        return TRIGGER_EVENT_TOUCH;
    case DEMO_TRIGGER_SOURCE_WAKE_WORD:
        return TRIGGER_EVENT_WAKE_WORD;
    default:
        return TRIGGER_EVENT_NONE;
    }
}

static esp_err_t trigger_input_init_button(trigger_input_t *trigger)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << DEMO_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = DEMO_BUTTON_PULL_UP_ENABLE ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = DEMO_BUTTON_PULL_DOWN_ENABLE ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure trigger GPIO %d: %s",
                 DEMO_BUTTON_GPIO, esp_err_to_name(ret));
        return ret;
    }

    const int initial_level = gpio_get_level(DEMO_BUTTON_GPIO);
    trigger->initialized = true;
    trigger->active_level = DEMO_BUTTON_ACTIVE_LEVEL;
    trigger->debounced_level = initial_level;
    trigger->last_sample_level = initial_level;
    trigger->last_change_tick = xTaskGetTickCount();
    trigger->debounce_ticks = pdMS_TO_TICKS(DEMO_BUTTON_DEBOUNCE_MS);

    ESP_LOGI(TAG,
             "Initialized GPIO trigger on GPIO %d (active level %d initial_level=%d)",
             DEMO_BUTTON_GPIO,
             DEMO_BUTTON_ACTIVE_LEVEL,
             initial_level);
    return ESP_OK;
}

esp_err_t trigger_input_init(trigger_input_t *trigger)
{
    if (trigger == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    trigger->configured_source = trigger_input_configured_source();
    switch (trigger->configured_source) {
    case TRIGGER_EVENT_BUTTON:
        return trigger_input_init_button(trigger);
    case TRIGGER_EVENT_TOUCH:
        ESP_LOGW(TAG,
                 "Touch trigger is disabled in this firmware; use GPIO trigger instead on %s %s",
                 DEMO_BOARD_NAME,
                 DEMO_BOARD_REVISION);
        return ESP_ERR_NOT_SUPPORTED;
    case TRIGGER_EVENT_WAKE_WORD:
        ESP_LOGW(TAG,
                 "Wake-word trigger selected for %s %s, but wake-word integration is not implemented yet",
                 DEMO_BOARD_NAME,
                 DEMO_BOARD_REVISION);
        return ESP_ERR_NOT_SUPPORTED;
    case TRIGGER_EVENT_NONE:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

bool trigger_input_poll(trigger_input_t *trigger, trigger_event_t *out_event)
{
    if (trigger == NULL || out_event == NULL) {
        return false;
    }

    out_event->type = TRIGGER_EVENT_NONE;
    out_event->x = 0;
    out_event->y = 0;

    if (!trigger->initialized) {
        if (trigger_input_init(trigger) != ESP_OK) {
            return false;
        }
    }

    if (trigger->configured_source != TRIGGER_EVENT_BUTTON) {
        if (!trigger->warned_unsupported) {
            ESP_LOGW(TAG,
                     "Trigger source %s is configured but not active in this build",
                     trigger_input_source_name(trigger->configured_source));
            trigger->warned_unsupported = true;
        }
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    const int current_level = gpio_get_level(DEMO_BUTTON_GPIO);

    if (current_level != trigger->last_sample_level) {
        ESP_LOGI(TAG, "GPIO trigger raw level change: gpio=%d level=%d", DEMO_BUTTON_GPIO, current_level);
        trigger->last_sample_level = current_level;
        trigger->last_change_tick = now;
        return false;
    }

    if ((now - trigger->last_change_tick) < trigger->debounce_ticks) {
        return false;
    }

    if (trigger->debounced_level == current_level) {
        return false;
    }

    trigger->debounced_level = current_level;
    ESP_LOGI(TAG, "GPIO trigger debounced level: gpio=%d level=%d", DEMO_BUTTON_GPIO, current_level);
    if (current_level == trigger->active_level) {
        out_event->type = TRIGGER_EVENT_BUTTON;
        ESP_LOGI(TAG, "Button trigger event: gpio=%d level=%d", DEMO_BUTTON_GPIO, current_level);
        return true;
    }

    return false;
}
