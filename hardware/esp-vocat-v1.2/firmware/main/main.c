#include "config.h"
#include "trigger_input.h"
#include "audio_in.h"
#include "audio_out.h"
#include "cloud_client.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "esp_idf_demo";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count = 0;
static TaskHandle_t s_pipeline_task_handle = NULL;

#define APP_WIFI_CONNECTED_BIT BIT0
#define APP_WIFI_FAILED_BIT    BIT1

typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_PLAYING_PROMPT,
    APP_STATE_WAITING_SPEECH,
    APP_STATE_RECORDING,
    APP_STATE_POSTING_SESSION,
    APP_STATE_POLLING,
    APP_STATE_DOWNLOADING,
    APP_STATE_OPENING_AUDIO,
    APP_STATE_STREAMING_AUDIO,
    APP_STATE_PLAYING,
    APP_STATE_DONE,
    APP_STATE_ERROR,
} app_state_t;

static app_state_t s_app_state = APP_STATE_ERROR;

static const char *app_state_to_string(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:
        return "idle";
    case APP_STATE_PLAYING_PROMPT:
        return "playing_prompt";
    case APP_STATE_WAITING_SPEECH:
        return "waiting_speech";
    case APP_STATE_RECORDING:
        return "recording";
    case APP_STATE_POSTING_SESSION:
        return "posting_session";
    case APP_STATE_POLLING:
        return "polling";
    case APP_STATE_DOWNLOADING:
        return "downloading";
    case APP_STATE_OPENING_AUDIO:
        return "opening_audio";
    case APP_STATE_STREAMING_AUDIO:
        return "streaming_audio";
    case APP_STATE_PLAYING:
        return "playing";
    case APP_STATE_DONE:
        return "done";
    case APP_STATE_ERROR:
    default:
        return "error";
    }
}

static const char *app_audio_mode_name(void)
{
    switch (DEMO_AUDIO_MODE) {
    case DEMO_AUDIO_MODE_V3_REALTIME:
        return "v3_realtime";
    case DEMO_AUDIO_MODE_V2_ASYNC:
    default:
        return "v2_async";
    }
}

static void app_set_state(app_state_t *current_state, app_state_t next_state)
{
    if (*current_state == next_state) {
        return;
    }

    ESP_LOGI(TAG, "State transition: %s -> %s",
             app_state_to_string(*current_state),
             app_state_to_string(next_state));
    *current_state = next_state;
}

typedef struct {
    trigger_event_t event;
} app_pipeline_task_args_t;

static void app_cleanup_pipeline_resources(void)
{
    audio_in_deinit();
    audio_out_deinit();
}

static void app_log_runtime_config(void)
{
    ESP_LOGI(TAG, "Runtime config:");
    ESP_LOGI(TAG, "  board=%s %s", DEMO_BOARD_NAME, DEMO_BOARD_REVISION);
    ESP_LOGI(TAG, "  trigger_source=%s", trigger_input_source_name(trigger_input_configured_source()));
    ESP_LOGI(TAG, "  wifi_ssid=%s", DEMO_WIFI_SSID);
    ESP_LOGI(TAG, "  server_base_url=%s", DEMO_SERVER_BASE_URL);
    ESP_LOGI(TAG, "  device_id=%s", DEMO_DEVICE_ID);
    ESP_LOGI(TAG, "  audio_mode=%s", app_audio_mode_name());
    ESP_LOGI(TAG, "  wifi_power_save_none=%d", DEMO_WIFI_POWER_SAVE_NONE);
    ESP_LOGI(TAG, "  audio_format=%dHz/%d-bit/%dch", DEMO_AUDIO_SAMPLE_RATE,
             DEMO_AUDIO_BITS_PER_SAMPLE, DEMO_AUDIO_CHANNELS);
    ESP_LOGI(TAG, "  record_duration_sec=%d", DEMO_RECORD_DURATION_SEC);
    ESP_LOGI(TAG, "  record_prompt_enabled=%d", DEMO_RECORD_PROMPT_ENABLED);
    ESP_LOGI(TAG, "  record_prompt_path=%s", DEMO_RECORD_PROMPT_PATH);
    ESP_LOGI(TAG, "  wait_for_speech_timeout_ms=%d", DEMO_WAIT_FOR_SPEECH_TIMEOUT_MS);
    ESP_LOGI(TAG, "  waiting_speech_arm_ms=%d", DEMO_WAITING_SPEECH_ARM_MS);
    ESP_LOGI(TAG, "  speech_start_hold_ms=%d", DEMO_SPEECH_START_HOLD_MS);
    ESP_LOGI(TAG, "  record_after_speech_max_ms=%d", DEMO_RECORD_AFTER_SPEECH_MAX_MS);
    ESP_LOGI(TAG, "  audio_input_gain_db=%.1f", (double)DEMO_AUDIO_INPUT_GAIN_DB);
    ESP_LOGI(TAG, "  audio_output_volume=%d", DEMO_AUDIO_OUTPUT_VOLUME);
    ESP_LOGI(TAG, "  cloud_poll_timeout_ms=%d", DEMO_CLOUD_POLL_TIMEOUT_MS);
    ESP_LOGI(TAG, "  cloud_poll_interval_ms=%d", DEMO_CLOUD_POLL_INTERVAL_MS);
    ESP_LOGI(TAG, "  realtime_session_timeout_ms=%d", DEMO_REALTIME_SESSION_TIMEOUT_MS);
    ESP_LOGI(TAG, "  realtime_audio_open_timeout_ms=%d", DEMO_REALTIME_AUDIO_OPEN_TIMEOUT_MS);
    ESP_LOGI(TAG, "  realtime_audio_read_timeout_ms=%d", DEMO_REALTIME_AUDIO_READ_TIMEOUT_MS);
    ESP_LOGI(TAG, "  realtime_audio_jitter_buffer_bytes=%d", DEMO_REALTIME_AUDIO_JITTER_BUFFER_BYTES);
    ESP_LOGI(TAG, "  realtime_audio_jitter_prebuffer_bytes=%d", DEMO_REALTIME_AUDIO_JITTER_PREBUFFER_BYTES);
    ESP_LOGI(TAG, "  realtime_intro_enabled=%d", DEMO_REALTIME_INTRO_ENABLED);
    ESP_LOGI(TAG, "  realtime_intro_path=%s", DEMO_REALTIME_INTRO_PATH);
    ESP_LOGI(TAG, "  trigger_poll_interval_ms=%d", DEMO_TRIGGER_POLL_INTERVAL_MS);
}

static void app_log_heap_snapshot(const char *stage)
{
    ESP_LOGI(TAG,
             "heap stage=%s free_8bit=%u largest_8bit=%u free_spiram=%u largest_spiram=%u",
             stage != NULL ? stage : "unknown",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static esp_err_t app_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = DEMO_SPIFFS_BASE_PATH,
        .partition_label = DEMO_SPIFFS_PARTITION_LABEL,
        .max_files = DEMO_SPIFFS_MAX_FILES,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: label=%s base=%s err=%s",
                 DEMO_SPIFFS_PARTITION_LABEL,
                 DEMO_SPIFFS_BASE_PATH,
                 esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(DEMO_SPIFFS_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: label=%s total=%u used=%u",
                 DEMO_SPIFFS_PARTITION_LABEL,
                 (unsigned)total,
                 (unsigned)used);
    } else {
        ESP_LOGW(TAG, "SPIFFS mounted but info failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

typedef struct {
    bool saw_first_chunk;
    size_t total_bytes;
    int64_t pipeline_start_us;
    int64_t first_chunk_us;
    uint32_t callback_count;
    int64_t total_enqueue_us;
    int64_t max_enqueue_us;
} app_realtime_stream_ctx_t;

static double app_elapsed_ms_since(int64_t start_us, int64_t end_us)
{
    return (double)(end_us - start_us) / 1000.0;
}

static void app_log_pipeline_stack_watermark(const char *stage)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "pipeline_stack stage=%s high_water_words=%u high_water_bytes=%u configured_stack_bytes=%u",
             stage != NULL ? stage : "unknown",
             (unsigned)words,
             (unsigned)(words * sizeof(StackType_t)),
             (unsigned)DEMO_PIPELINE_TASK_STACK_SIZE);
}

static esp_err_t app_realtime_audio_chunk_sink(const uint8_t *chunk,
                                               size_t chunk_bytes,
                                               void *user_ctx)
{
    if (chunk == NULL || chunk_bytes == 0 || user_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_realtime_stream_ctx_t *ctx = (app_realtime_stream_ctx_t *)user_ctx;
    size_t written_bytes = 0;
    const int64_t enqueue_start_us = esp_timer_get_time();
    esp_err_t ret = audio_out_write_pcm_chunk_buffered(chunk, chunk_bytes, &written_bytes);
    const int64_t enqueue_us = esp_timer_get_time() - enqueue_start_us;
    ctx->callback_count++;
    ctx->total_enqueue_us += enqueue_us;
    if (enqueue_us > ctx->max_enqueue_us) {
        ctx->max_enqueue_us = enqueue_us;
    }
    if (enqueue_us > (int64_t)DEMO_REALTIME_AUDIO_ENQUEUE_WARN_MS * 1000) {
        ESP_LOGW(TAG,
                 "realtime_audio_enqueue_slow chunk_bytes=%u enqueue_ms=%.1f callback_count=%u total_bytes=%u",
                 (unsigned)chunk_bytes,
                 (double)enqueue_us / 1000.0,
                 (unsigned)ctx->callback_count,
                 (unsigned)ctx->total_bytes);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "error_code=codec_write_failed err=%s", esp_err_to_name(ret));
        return ret;
    }

    if (!ctx->saw_first_chunk) {
        ctx->saw_first_chunk = true;
        ctx->first_chunk_us = esp_timer_get_time();
        ESP_LOGI(TAG,
                 "first_audio_byte_local_ms=%.1f",
                 (double)(ctx->first_chunk_us - ctx->pipeline_start_us) / 1000.0);
    }
    ctx->total_bytes += written_bytes;
    return ESP_OK;
}

static void app_log_stage_start(const char *stage)
{
    ESP_LOGI(TAG, "stage=%s event=start", stage);
}

static void app_log_stage_result(const char *stage, esp_err_t ret, int64_t elapsed_us)
{
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "stage=%s event=done elapsed_ms=%.1f", stage, (double)elapsed_us / 1000.0);
    } else {
        ESP_LOGE(TAG, "stage=%s event=failed err=%s elapsed_ms=%.1f",
                 stage, esp_err_to_name(ret), (double)elapsed_us / 1000.0);
    }
}

static esp_err_t app_validate_runtime_config(void)
{
    if (DEMO_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "DEMO_WIFI_SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (DEMO_SERVER_BASE_URL[0] == '\0') {
        ESP_LOGE(TAG, "DEMO_SERVER_BASE_URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (DEMO_DEVICE_ID[0] == '\0') {
        ESP_LOGE(TAG, "DEMO_DEVICE_ID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void app_wifi_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
        if (s_wifi_retry_count < DEMO_WIFI_MAXIMUM_RETRY) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)",
                     s_wifi_retry_count,
                     DEMO_WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, APP_WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t app_wifi_connect(void)
{
    esp_err_t ret = app_validate_runtime_config();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init esp_netif: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi station");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Wi-Fi: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT | APP_WIFI_FAILED_BIT);

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", DEMO_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", DEMO_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode =
        DEMO_WIFI_PASSWORD[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) {
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    }
    if (ret == ESP_OK) {
    ret = esp_wifi_start();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi station: %s", esp_err_to_name(ret));
        return ret;
    }

#if DEMO_WIFI_POWER_SAVE_NONE
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for realtime audio");
    }
#endif

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID %s", DEMO_WIFI_SSID);

    const EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                 APP_WIFI_CONNECTED_BIT | APP_WIFI_FAILED_BIT,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(DEMO_WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & APP_WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    if ((bits & APP_WIFI_FAILED_BIT) != 0) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi after %d retries", DEMO_WIFI_MAXIMUM_RETRY);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Timed out waiting for Wi-Fi connection");
    return ESP_ERR_TIMEOUT;
}

static bool app_wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi_event_group) & APP_WIFI_CONNECTED_BIT) != 0;
}

static esp_err_t run_trigger_pipeline(app_state_t *state)
{
    esp_err_t ret = ESP_OK;
    int64_t pipeline_start_us = esp_timer_get_time();
    uint8_t *pcm_buffer = NULL;
    uint8_t *speech_prefix = NULL;
    size_t speech_prefix_bytes = 0;
    size_t pcm_bytes = 0;
    uint8_t *wav_buffer = NULL;
    size_t wav_bytes = 0;
    char task_id[DEMO_CLOUD_TASK_ID_MAX_LEN] = {0};
    cloud_task_result_t result = {0};
    cloud_realtime_session_t realtime_session = {0};
    cloud_realtime_audio_metrics_t realtime_metrics = {0};
    app_realtime_stream_ctx_t realtime_stream_ctx = {
        .saw_first_chunk = false,
        .total_bytes = 0,
        .pipeline_start_us = pipeline_start_us,
        .first_chunk_us = 0,
    };
    audio_in_wait_metrics_t wait_metrics = {0};
    audio_in_record_metrics_t record_metrics = {0};
    const char *pipeline_error_code = "unknown";
    int64_t stage_start_us = 0;

    if (!app_wifi_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi is not connected; refusing to start pipeline");
        return ESP_ERR_INVALID_STATE;
    }

    app_log_pipeline_stack_watermark("pipeline_start");
    app_log_heap_snapshot("pipeline_start");

#if DEMO_RECORD_PROMPT_ENABLED
    app_set_state(state, APP_STATE_PLAYING_PROMPT);
    app_log_stage_start("record_prompt");
    stage_start_us = esp_timer_get_time();
    esp_err_t prompt_ret = audio_out_play_pcm_file(DEMO_RECORD_PROMPT_PATH,
                                                   DEMO_AUDIO_SAMPLE_RATE,
                                                   DEMO_AUDIO_CHANNELS,
                                                   DEMO_AUDIO_BITS_PER_SAMPLE,
                                                   DEMO_RECORD_PROMPT_MAX_BYTES);
    app_log_stage_result("record_prompt", prompt_ret, esp_timer_get_time() - stage_start_us);
    if (prompt_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "record_prompt_failed path=%s err=%s",
                 DEMO_RECORD_PROMPT_PATH,
                 esp_err_to_name(prompt_ret));
    }
#endif

    app_set_state(state, APP_STATE_WAITING_SPEECH);
    app_log_stage_start("waiting_speech");
    stage_start_us = esp_timer_get_time();
    ret = audio_in_wait_for_speech_start(&speech_prefix, &speech_prefix_bytes, &wait_metrics);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "stage=waiting_speech event=speech_detected elapsed_ms=%u max_level=%u speech_prefix_bytes=%u",
                 (unsigned)wait_metrics.elapsed_ms,
                 (unsigned)wait_metrics.max_level,
                 (unsigned)wait_metrics.speech_prefix_bytes);
    } else if (ret == DEMO_AUDIO_IN_ERR_WAIT_TIMEOUT) {
        ESP_LOGW(TAG,
                 "stage=waiting_speech event=timeout elapsed_ms=%u max_level=%u",
                 (unsigned)wait_metrics.elapsed_ms,
                 (unsigned)wait_metrics.max_level);
    }
    app_log_stage_result("waiting_speech", ret == DEMO_AUDIO_IN_ERR_WAIT_TIMEOUT ? ESP_OK : ret,
                         esp_timer_get_time() - stage_start_us);
    if (ret == DEMO_AUDIO_IN_ERR_WAIT_TIMEOUT) {
        ret = ESP_OK;
        goto cleanup;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Waiting for speech failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    app_set_state(state, APP_STATE_RECORDING);
    ESP_LOGI(TAG,
             "stage=recording event=start reason=speech_detected record_budget_ms=%u",
             (unsigned)DEMO_RECORD_AFTER_SPEECH_MAX_MS);
    app_log_stage_start("record");
    stage_start_us = esp_timer_get_time();
    ret = audio_in_record_after_speech_start(speech_prefix,
                                             speech_prefix_bytes,
                                             &pcm_buffer,
                                             &pcm_bytes,
                                             &record_metrics);
    app_log_stage_result("record", ret, esp_timer_get_time() - stage_start_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Recording failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ESP_LOGI(TAG,
             "stage=record pcm_bytes=%u vad_stopped=%d voice_started=%d max_level=%u",
             (unsigned)pcm_bytes,
             record_metrics.vad_stopped,
             record_metrics.voice_started,
             (unsigned)record_metrics.max_level);

    app_set_state(state, APP_STATE_POSTING_SESSION);
    app_log_stage_start(DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME ? "post_session" : "upload");
    stage_start_us = esp_timer_get_time();
    if (DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME) {
        ret = cloud_client_submit_realtime_session(pcm_buffer, pcm_bytes, &realtime_session);
    } else {
        ret = cloud_client_submit_pcm_task(pcm_buffer, pcm_bytes, task_id, sizeof(task_id));
    }
    app_log_stage_result(DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME ? "post_session" : "upload",
                         ret,
                         esp_timer_get_time() - stage_start_us);
    if (ret != ESP_OK) {
        pipeline_error_code = "session_create_failed";
        ESP_LOGE(TAG, "error_code=%s err=%s", pipeline_error_code, esp_err_to_name(ret));
        goto cleanup;
    }
    if (DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME) {
        ESP_LOGI(TAG, "stage=post_session session_id=%s audio_stream_url=%s",
                 realtime_session.session_id,
                 realtime_session.audio_stream_url);
    } else {
        ESP_LOGI(TAG, "stage=upload task_id=%s", task_id);
    }
    free(pcm_buffer);
    pcm_buffer = NULL;
    app_log_heap_snapshot("after_record_buffer_free");

    if (DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME) {
#if DEMO_REALTIME_INTRO_ENABLED
        app_set_state(state, APP_STATE_PLAYING);
        app_log_stage_start("intro");
        stage_start_us = esp_timer_get_time();
        esp_err_t intro_ret = audio_out_play_pcm_file(DEMO_REALTIME_INTRO_PATH,
                                                      DEMO_AUDIO_SAMPLE_RATE,
                                                      DEMO_AUDIO_CHANNELS,
                                                      DEMO_AUDIO_BITS_PER_SAMPLE,
                                                      DEMO_REALTIME_INTRO_MAX_BYTES);
        app_log_stage_result("intro", intro_ret, esp_timer_get_time() - stage_start_us);
        if (intro_ret != ESP_OK) {
            ESP_LOGW(TAG, "realtime_intro_failed err=%s; continuing with cloud audio",
                     esp_err_to_name(intro_ret));
        }
#endif

        app_set_state(state, APP_STATE_OPENING_AUDIO);
        app_log_stage_start("open_audio");
        stage_start_us = esp_timer_get_time();
        ret = audio_out_open_pcm_stream(DEMO_AUDIO_SAMPLE_RATE,
                                        DEMO_AUDIO_CHANNELS,
                                        DEMO_AUDIO_BITS_PER_SAMPLE);
        app_log_stage_result("open_audio", ret, esp_timer_get_time() - stage_start_us);
        if (ret != ESP_OK) {
            pipeline_error_code = "codec_open_failed";
            ESP_LOGE(TAG, "error_code=%s err=%s", pipeline_error_code, esp_err_to_name(ret));
            goto cleanup;
        }

        app_set_state(state, APP_STATE_STREAMING_AUDIO);
        app_log_stage_start("stream_audio");
        app_log_pipeline_stack_watermark("before_stream_audio");
        stage_start_us = esp_timer_get_time();
        ret = cloud_client_stream_realtime_audio(realtime_session.audio_stream_url,
                                                 app_realtime_audio_chunk_sink,
                                                 &realtime_stream_ctx,
                                                 &realtime_metrics);
        const int64_t stream_end_us = esp_timer_get_time();
        app_log_pipeline_stack_watermark("after_stream_audio");
        app_log_stage_result("stream_audio", ret, stream_end_us - stage_start_us);
        const double avg_inter_chunk_gap_ms =
            realtime_metrics.chunk_count > 1
                ? (double)realtime_metrics.total_inter_chunk_gap_us /
                      (double)(realtime_metrics.chunk_count - 1) / 1000.0
                : 0.0;
        ESP_LOGI(TAG,
                 "session_id=%s http_status=%d audio_format=%s audio_packetization=%s headers_validated=%s audio_connect_ms=%.1f first_chunk_ms=%.1f first_audio_byte_local_ms=%.1f playback_end_ms=%.1f total_stream_bytes=%u chunk_count=%u packet_count=%u seq_gap_count=%u first_chunk_bytes=%u last_chunk_bytes=%u max_inter_chunk_gap_ms=%.1f avg_inter_chunk_gap_ms=%.1f max_enqueue_ms=%.1f avg_enqueue_ms=%.1f receive_queue_peak=%u receive_queue_full_count=%u decode_queue_peak=%u decode_queue_full_count=%u decode_packet_count=%u decode_fail_count=%u decode_avg_ms=%.1f decode_max_ms=%.1f pcm_queue_drain_ms=%.1f",
                 realtime_session.session_id,
                 realtime_metrics.http_status,
                 realtime_metrics.audio_format[0] != '\0' ? realtime_metrics.audio_format : "pcm",
                 realtime_metrics.audio_packetization[0] != '\0' ? realtime_metrics.audio_packetization : "legacy",
                 realtime_metrics.headers_validated ? "true" : "false",
                 (double)realtime_metrics.connect_elapsed_us / 1000.0,
                 (double)realtime_metrics.first_chunk_elapsed_us / 1000.0,
                 realtime_stream_ctx.saw_first_chunk
                     ? app_elapsed_ms_since(realtime_stream_ctx.pipeline_start_us,
                                            realtime_stream_ctx.first_chunk_us)
                     : -1.0,
                 realtime_stream_ctx.saw_first_chunk
                     ? app_elapsed_ms_since(realtime_stream_ctx.pipeline_start_us, stream_end_us)
                     : -1.0,
                 (unsigned)realtime_metrics.total_audio_bytes,
                 (unsigned)realtime_metrics.chunk_count,
                 (unsigned)realtime_metrics.packet_count,
                 (unsigned)realtime_metrics.seq_gap_count,
                 (unsigned)realtime_metrics.first_chunk_bytes,
                 (unsigned)realtime_metrics.last_chunk_bytes,
                 (double)realtime_metrics.max_inter_chunk_gap_us / 1000.0,
                 avg_inter_chunk_gap_ms,
                 (double)realtime_stream_ctx.max_enqueue_us / 1000.0,
                 realtime_stream_ctx.callback_count > 0
                     ? (double)realtime_stream_ctx.total_enqueue_us /
                           (double)realtime_stream_ctx.callback_count / 1000.0
                     : 0.0,
                 (unsigned)realtime_metrics.receive_queue_peak,
                 (unsigned)realtime_metrics.receive_queue_full_count,
                 (unsigned)realtime_metrics.decode_queue_peak,
                 (unsigned)realtime_metrics.decode_queue_full_count,
                 (unsigned)realtime_metrics.decode_packet_count,
                 (unsigned)realtime_metrics.decode_fail_count,
                 realtime_metrics.decode_packet_count > 0
                     ? (double)realtime_metrics.decode_total_us /
                           (double)realtime_metrics.decode_packet_count / 1000.0
                     : 0.0,
                 (double)realtime_metrics.decode_max_us / 1000.0,
                 (double)realtime_metrics.pcm_queue_drain_us / 1000.0);
        audio_out_jitter_metrics_t jitter_metrics = {0};
        esp_err_t close_ret = audio_out_close_pcm_stream_with_metrics(&jitter_metrics);
        ESP_LOGI(TAG,
                 "realtime_audio_summary session_id=%s total_bytes=%u chunk_count=%u max_gap_ms=%.1f avg_gap_ms=%.1f prebuffer_wait_ms=%.1f playback_actual_start_ms=%.1f underrun_count=%u underrun_ms=%.1f min_level=%u max_level=%u total_in=%u total_out=%u",
                 realtime_session.session_id,
                 (unsigned)realtime_metrics.total_audio_bytes,
                 (unsigned)realtime_metrics.chunk_count,
                 (double)realtime_metrics.max_inter_chunk_gap_us / 1000.0,
                 avg_inter_chunk_gap_ms,
                 (double)jitter_metrics.prebuffer_wait_us / 1000.0,
                 jitter_metrics.playback_started
                     ? app_elapsed_ms_since(realtime_stream_ctx.pipeline_start_us,
                                            jitter_metrics.playback_start_us)
                     : -1.0,
                 (unsigned)jitter_metrics.underrun_count,
                 (double)jitter_metrics.underrun_us / 1000.0,
                 (unsigned)jitter_metrics.min_level,
                 (unsigned)jitter_metrics.max_level,
                 (unsigned)jitter_metrics.total_in,
                 (unsigned)jitter_metrics.total_out);
        if (ret == ESP_OK && close_ret != ESP_OK) {
            ret = close_ret;
        }
        app_log_heap_snapshot("after_audio_close");
        if (ret != ESP_OK) {
            if (ret == DEMO_CLOUD_ERR_AUDIO_HEADER_MISMATCH) {
                pipeline_error_code = "audio_header_mismatch";
            } else if (ret == DEMO_CLOUD_ERR_AUDIO_STREAM_EARLY_EOF) {
                pipeline_error_code = "early_eof";
            } else if (realtime_stream_ctx.saw_first_chunk) {
                pipeline_error_code = "audio_stream_read_failed";
            } else {
                pipeline_error_code = "audio_connect_failed";
            }
            ESP_LOGE(TAG, "error_code=%s err=%s", pipeline_error_code, esp_err_to_name(ret));
            goto cleanup;
        }

        app_set_state(state, APP_STATE_DONE);
        goto cleanup;
    }

    app_set_state(state, APP_STATE_POLLING);
    app_log_stage_start("poll");
    stage_start_us = esp_timer_get_time();
    ret = cloud_client_poll_task(task_id,
                                 &result,
                                 DEMO_CLOUD_POLL_TIMEOUT_MS,
                                 DEMO_CLOUD_POLL_INTERVAL_MS);
    app_log_stage_result("poll", ret, esp_timer_get_time() - stage_start_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Polling failed for task %s: %s", task_id, esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG, "question_text: %s",
             result.question_text[0] != '\0' ? result.question_text : "(empty)");
    if (result.error_code[0] != '\0') {
        ESP_LOGI(TAG, "error_code: %s", result.error_code);
    }

    if (strcmp(result.status, "failed") == 0) {
        ESP_LOGE(TAG, "Cloud task reported failed status");
        ret = ESP_FAIL;
        pipeline_error_code = result.error_code[0] != '\0' ? result.error_code : "cloud_task_failed";
        goto cleanup;
    }

    if (result.audio_url[0] == '\0') {
        ESP_LOGE(TAG, "Cloud task completed without audio_url");
        ret = ESP_FAIL;
        pipeline_error_code = "audio_url_missing";
        goto cleanup;
    }

    app_set_state(state, APP_STATE_DOWNLOADING);
    app_log_stage_start("download");
    stage_start_us = esp_timer_get_time();
    ret = audio_out_download_wav_url(result.audio_url, &wav_buffer, &wav_bytes);
    app_log_stage_result("download", ret, esp_timer_get_time() - stage_start_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(ret));
        pipeline_error_code = "audio_download_failed";
        goto cleanup;
    }

    ESP_LOGI(TAG, "Downloaded WAV: %u bytes", (unsigned)wav_bytes);

    app_set_state(state, APP_STATE_PLAYING);
    app_log_stage_start("play");
    stage_start_us = esp_timer_get_time();
    ret = audio_out_play_wav_buffer(wav_buffer, wav_bytes);
    app_log_stage_result("play", ret, esp_timer_get_time() - stage_start_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(ret));
        pipeline_error_code = "codec_write_failed";
        goto cleanup;
    }

cleanup:
    app_log_pipeline_stack_watermark("cleanup");
    app_log_heap_snapshot("cleanup");
    free(speech_prefix);
    free(pcm_buffer);
    free(wav_buffer);
    if (DEMO_AUDIO_MODE == DEMO_AUDIO_MODE_V3_REALTIME) {
        (void)audio_out_close_pcm_stream();
    }
    app_cleanup_pipeline_resources();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "pipeline result=ok total_elapsed_ms=%.1f",
                 (double)(esp_timer_get_time() - pipeline_start_us) / 1000.0);
        app_set_state(state, APP_STATE_IDLE);
    } else {
        ESP_LOGE(TAG, "pipeline result=failed error_code=%s total_elapsed_ms=%.1f",
                 pipeline_error_code,
                 (double)(esp_timer_get_time() - pipeline_start_us) / 1000.0);
        app_set_state(state, APP_STATE_ERROR);
        app_set_state(state, APP_STATE_IDLE);
    }
    return ret;
}

static void app_pipeline_task(void *arg)
{
    app_pipeline_task_args_t *task_args = (app_pipeline_task_args_t *)arg;
    if (task_args != NULL) {
        ESP_LOGI(TAG, "pipeline task started for trigger=%s x=%u y=%u",
                 trigger_input_source_name(task_args->event.type),
                 (unsigned)task_args->event.x,
                 (unsigned)task_args->event.y);
    }

    (void)run_trigger_pipeline(&s_app_state);

    free(task_args);
    s_pipeline_task_handle = NULL;
    ESP_LOGI(TAG, "pipeline task finished");
    vTaskDelete(NULL);
}

static esp_err_t app_start_pipeline_task(const trigger_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_pipeline_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    app_pipeline_task_args_t *task_args = calloc(1, sizeof(*task_args));
    if (task_args == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task_args->event = *event;

    BaseType_t ok = xTaskCreate(app_pipeline_task,
                                "audio_pipeline",
                                DEMO_PIPELINE_TASK_STACK_SIZE,
                                task_args,
                                DEMO_PIPELINE_TASK_PRIORITY,
                                &s_pipeline_task_handle);
    if (ok != pdPASS) {
        free(task_args);
        s_pipeline_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void app_main(void)
{
    trigger_input_t trigger = {0};

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP-IDF audio demo starting");
    ESP_LOGI(TAG, "Board: %s %s", DEMO_BOARD_NAME, DEMO_BOARD_REVISION);
    app_log_runtime_config();
    ESP_LOGI(TAG, "========================================");

    if (DEMO_REALTIME_INTRO_ENABLED) {
        (void)app_mount_spiffs();
    }

    if (app_wifi_connect() != ESP_OK) {
        app_set_state(&s_app_state, APP_STATE_ERROR);
        ESP_LOGE(TAG, "Wi-Fi initialization failed; stopping demo");
        return;
    }

    if (trigger_input_init(&trigger) != ESP_OK) {
        app_set_state(&s_app_state, APP_STATE_ERROR);
        ESP_LOGE(TAG, "Trigger initialization failed; stopping demo");
        return;
    }
    app_set_state(&s_app_state, APP_STATE_IDLE);

    while (true) {
        trigger_event_t event = {0};
        if (trigger_input_poll(&trigger, &event)) {
            if (s_app_state == APP_STATE_IDLE && s_pipeline_task_handle == NULL) {
                ESP_LOGI(TAG, "trigger source=%s x=%u y=%u -> starting pipeline",
                         trigger_input_source_name(event.type),
                         (unsigned)event.x,
                         (unsigned)event.y);
                esp_err_t ret = app_start_pipeline_task(&event);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start pipeline task: %s", esp_err_to_name(ret));
                    app_set_state(&s_app_state, APP_STATE_ERROR);
                    app_set_state(&s_app_state, APP_STATE_IDLE);
                }
            } else {
                ESP_LOGW(TAG,
                         "Trigger ignored: source=%s state=%s pipeline_task=%s",
                         trigger_input_source_name(event.type),
                         app_state_to_string(s_app_state),
                         s_pipeline_task_handle == NULL ? "none" : "busy");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DEMO_TRIGGER_POLL_INTERVAL_MS));
    }
}
