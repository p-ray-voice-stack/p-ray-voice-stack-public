#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    TRIGGER_EVENT_NONE = 0,
    TRIGGER_EVENT_BUTTON,
    TRIGGER_EVENT_TOUCH,
    TRIGGER_EVENT_WAKE_WORD,
} trigger_event_type_t;

typedef struct {
    bool initialized;
    bool warned_unsupported;
    int active_level;
    int debounced_level;
    int last_sample_level;
    TickType_t last_change_tick;
    TickType_t debounce_ticks;
    trigger_event_type_t configured_source;
} trigger_input_t;

typedef struct {
    trigger_event_type_t type;
    uint16_t x;
    uint16_t y;
} trigger_event_t;

const char *trigger_input_source_name(trigger_event_type_t type);
trigger_event_type_t trigger_input_configured_source(void);
esp_err_t trigger_input_init(trigger_input_t *trigger);
bool trigger_input_poll(trigger_input_t *trigger, trigger_event_t *out_event);
