#include "audio_out.h"

#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_vocat.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "audio_out";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} audio_buffer_t;

typedef struct {
    bool initialized;
    bool opened;
    bool stream_task_started;
    uint32_t sample_rate;
    esp_codec_dev_handle_t speaker_handle;
    RingbufHandle_t jitter_ringbuf;
    TaskHandle_t stream_task;
    bool stream_task_done;
    esp_err_t stream_task_result;
    size_t jitter_total_in;
    size_t jitter_total_out;
    size_t jitter_min_level;
    size_t jitter_max_level;
    uint32_t jitter_underrun_count;
    int64_t jitter_underrun_us;
    int64_t jitter_first_input_us;
    int64_t jitter_playback_start_us;
    int64_t jitter_prebuffer_wait_us;
    bool jitter_playback_started;
} audio_out_state_t;

typedef struct {
    const uint8_t *pcm;
    size_t pcm_bytes;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
} wav_pcm_view_t;

static audio_out_state_t s_audio_out_state = {
    .initialized = false,
    .opened = false,
    .stream_task_started = false,
    .sample_rate = 0,
    .speaker_handle = NULL,
    .jitter_ringbuf = NULL,
    .stream_task = NULL,
    .stream_task_done = true,
    .stream_task_result = ESP_OK,
    .jitter_total_in = 0,
    .jitter_total_out = 0,
    .jitter_min_level = 0,
    .jitter_max_level = 0,
    .jitter_underrun_count = 0,
    .jitter_underrun_us = 0,
    .jitter_first_input_us = 0,
    .jitter_playback_start_us = 0,
    .jitter_prebuffer_wait_us = 0,
    .jitter_playback_started = false,
};

static StaticSemaphore_t s_audio_out_mutex_storage;
static SemaphoreHandle_t s_audio_out_mutex = NULL;
static portMUX_TYPE s_audio_out_mutex_guard = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t audio_out_lock(void)
{
    if (s_audio_out_mutex == NULL) {
        taskENTER_CRITICAL(&s_audio_out_mutex_guard);
        if (s_audio_out_mutex == NULL) {
            s_audio_out_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_audio_out_mutex_storage);
            if (s_audio_out_mutex == NULL) {
                ESP_LOGE(TAG, "Failed to create audio_out mutex");
                taskEXIT_CRITICAL(&s_audio_out_mutex_guard);
                return ESP_ERR_NO_MEM;
            }
        }
        taskEXIT_CRITICAL(&s_audio_out_mutex_guard);
    }

    if (xSemaphoreTakeRecursive(s_audio_out_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void audio_out_unlock(void)
{
    if (s_audio_out_mutex != NULL) {
        xSemaphoreGiveRecursive(s_audio_out_mutex);
    }
}

static esp_err_t audio_buffer_init(audio_buffer_t *buffer, size_t initial_cap)
{
    if (buffer == NULL || initial_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer->data = calloc(1, initial_cap);
    if (buffer->data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    buffer->len = 0;
    buffer->cap = initial_cap;
    return ESP_OK;
}

static void audio_buffer_free(audio_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static esp_err_t audio_buffer_append(audio_buffer_t *buffer, const char *data, size_t data_len)
{
    if (buffer == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len == 0) {
        return ESP_OK;
    }

    if (buffer->len + data_len + 1 > buffer->cap) {
        size_t new_cap = buffer->cap;
        while (buffer->len + data_len + 1 > new_cap) {
            new_cap *= 2;
            if (new_cap > DEMO_WAV_DOWNLOAD_MAX_BYTES) {
                new_cap = DEMO_WAV_DOWNLOAD_MAX_BYTES;
                break;
            }
        }

        if (buffer->len + data_len + 1 > new_cap) {
            return ESP_ERR_NO_MEM;
        }

        char *new_data = realloc(buffer->data, new_cap);
        if (new_data == NULL) {
            return ESP_ERR_NO_MEM;
        }

        memset(new_data + buffer->cap, 0, new_cap - buffer->cap);
        buffer->data = new_data;
        buffer->cap = new_cap;
    }

    memcpy(buffer->data + buffer->len, data, data_len);
    buffer->len += data_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static uint16_t audio_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t audio_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool audio_chunk_id_matches(const uint8_t *id, const char *tag)
{
    return memcmp(id, tag, 4) == 0;
}

static esp_err_t audio_validate_wav_pcm_layout(wav_pcm_view_t *view)
{
    if (view == NULL || view->pcm == NULL || view->pcm_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (view->channels == 0 || view->bits_per_sample == 0) {
        return ESP_FAIL;
    }

    if ((view->bits_per_sample % 8) != 0) {
        ESP_LOGE(TAG, "WAV bit depth is not byte aligned: %u", (unsigned)view->bits_per_sample);
        return ESP_FAIL;
    }

    const size_t bytes_per_sample = view->bits_per_sample / 8;
    const size_t frame_bytes = (size_t)view->channels * bytes_per_sample;
    if (bytes_per_sample == 0 || frame_bytes == 0) {
        return ESP_FAIL;
    }

    if ((view->pcm_bytes % frame_bytes) != 0) {
        const size_t aligned_bytes = view->pcm_bytes - (view->pcm_bytes % frame_bytes);
        ESP_LOGW(TAG,
                 "WAV payload is not frame aligned: pcm_bytes=%u frame_bytes=%u, clamping to %u",
                 (unsigned)view->pcm_bytes,
                 (unsigned)frame_bytes,
                 (unsigned)aligned_bytes);
        if (aligned_bytes == 0) {
            return ESP_FAIL;
        }
        view->pcm_bytes = aligned_bytes;
    }

    return ESP_OK;
}

static esp_err_t audio_http_download(const char *audio_url, audio_buffer_t *buffer)
{
    if (audio_url == NULL || audio_url[0] == '\0' || buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = audio_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DEMO_AUDIO_DOWNLOAD_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio download failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch audio response headers: %lld", (long long)content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    const int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Audio download returned HTTP %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length > 0) {
        ESP_LOGI(TAG,
                 "Downloading WAV: url=%s content_length=%lld current_cap=%u max_bytes=%u",
                 audio_url,
                 (long long)content_length,
                 (unsigned)buffer->cap,
                 (unsigned)DEMO_WAV_DOWNLOAD_MAX_BYTES);
        if ((size_t)content_length + 1 > DEMO_WAV_DOWNLOAD_MAX_BYTES) {
            ESP_LOGE(TAG,
                     "Audio response exceeds max download bytes: content_length=%lld max_bytes=%u",
                     (long long)content_length,
                     (unsigned)DEMO_WAV_DOWNLOAD_MAX_BYTES);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGI(TAG,
                 "Downloading WAV: url=%s content_length=%lld (streamed/unknown)",
                 audio_url,
                 (long long)content_length);
    }

    char chunk[4096];
    while (true) {
        const int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Audio download read failed: %d", read_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            continue;
        }

        ret = audio_buffer_append(buffer, chunk, (size_t)read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to append audio chunk: read_len=%d downloaded=%u cap=%u err=%s",
                     read_len,
                     (unsigned)buffer->len,
                     (unsigned)buffer->cap,
                     esp_err_to_name(ret));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ret;
        }
    }

    ESP_LOGI(TAG,
             "Audio download complete: status=%d bytes=%u complete=%s",
             status_code,
             (unsigned)buffer->len,
             esp_http_client_is_complete_data_received(client) ? "true" : "false");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t audio_install_output_locked(uint32_t sample_rate)
{
    if (sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = bsp_audio_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP-VoCat audio bus: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_audio_out_state.initialized) {
        s_audio_out_state.speaker_handle = bsp_audio_codec_speaker_init();
        if (s_audio_out_state.speaker_handle == NULL) {
            ESP_LOGE(TAG, "Failed to initialize ESP-VoCat speaker codec");
            return ESP_FAIL;
        }

        ret = esp_codec_dev_set_out_vol(s_audio_out_state.speaker_handle, DEMO_AUDIO_OUTPUT_VOLUME);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Failed to set speaker volume to %d: %s",
                     DEMO_AUDIO_OUTPUT_VOLUME, esp_err_to_name(ret));
        }

        s_audio_out_state.initialized = true;
    }

    s_audio_out_state.sample_rate = sample_rate;
    ESP_LOGI(TAG, "Configured ESP-VoCat speaker: rate=%u volume=%d",
             (unsigned)sample_rate, DEMO_AUDIO_OUTPUT_VOLUME);
    return ESP_OK;
}

static esp_err_t audio_out_close_locked(void)
{
    if (!s_audio_out_state.opened || s_audio_out_state.speaker_handle == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = esp_codec_dev_close(s_audio_out_state.speaker_handle);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to close speaker codec: %s", esp_err_to_name(ret));
        return ret;
    }

    s_audio_out_state.opened = false;
    return ESP_OK;
}

static void audio_out_deinit_locked(void)
{
    if (s_audio_out_state.opened) {
        (void)audio_out_close_locked();
    }

    if (!s_audio_out_state.initialized) {
        return;
    }

    s_audio_out_state.speaker_handle = NULL;
    s_audio_out_state.initialized = false;
    s_audio_out_state.sample_rate = 0;
}

static esp_err_t audio_parse_wav(const uint8_t *wav_bytes,
                                 size_t wav_bytes_size,
                                 wav_pcm_view_t *view)
{
    if (wav_bytes == NULL || view == NULL || wav_bytes_size < 12) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(view, 0, sizeof(*view));

    if (!audio_chunk_id_matches(wav_bytes, "RIFF") ||
        !audio_chunk_id_matches(wav_bytes + 8, "WAVE")) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return ESP_FAIL;
    }

    size_t offset = 12;
    bool have_fmt = false;
    bool have_data = false;

    while (offset + 8 <= wav_bytes_size) {
        const uint8_t *chunk = wav_bytes + offset;
        uint32_t chunk_size = audio_read_le32(chunk + 4);
        const size_t chunk_data_offset = offset + 8;
        if (chunk_size > wav_bytes_size - chunk_data_offset) {
            if (audio_chunk_id_matches(chunk, "data")) {
                const size_t clamped_size = wav_bytes_size - chunk_data_offset;
                ESP_LOGW(TAG,
                         "WAV data chunk exceeds file size: header=%u remaining=%u, clamping",
                         (unsigned)chunk_size,
                         (unsigned)clamped_size);
                chunk_size = (uint32_t)clamped_size;
            } else {
                ESP_LOGE(TAG, "Truncated WAV chunk");
                return ESP_FAIL;
            }
        }

        const size_t chunk_end = chunk_data_offset + chunk_size;

        if (audio_chunk_id_matches(chunk, "fmt ")) {
            if (chunk_size < 16) {
                ESP_LOGE(TAG, "WAV fmt chunk too small");
                return ESP_FAIL;
            }

            view->channels = audio_read_le16(chunk + 10);
            view->sample_rate = audio_read_le32(chunk + 12);
            const uint32_t byte_rate = audio_read_le32(chunk + 16);
            const uint16_t block_align = audio_read_le16(chunk + 20);
            view->bits_per_sample = audio_read_le16(chunk + 22);
            const uint16_t audio_format = audio_read_le16(chunk + 8);

            if (audio_format != 1) {
                ESP_LOGE(TAG, "Unsupported WAV format %u", (unsigned)audio_format);
                return ESP_FAIL;
            }

            if (view->channels == 0 || view->channels > 2) {
                ESP_LOGE(TAG, "Unsupported WAV channel count %u", (unsigned)view->channels);
                return ESP_FAIL;
            }

            if (view->bits_per_sample != 8 && view->bits_per_sample != 16) {
                ESP_LOGE(TAG, "Unsupported WAV bit depth %u", (unsigned)view->bits_per_sample);
                return ESP_FAIL;
            }

            const uint32_t expected_byte_rate =
                view->sample_rate * (uint32_t)view->channels * (uint32_t)(view->bits_per_sample / 8);
            const uint16_t expected_block_align =
                (uint16_t)(view->channels * (view->bits_per_sample / 8));
            if (byte_rate != expected_byte_rate || block_align != expected_block_align) {
                ESP_LOGE(TAG,
                         "Invalid WAV fmt alignment: byte_rate=%u expected=%u block_align=%u expected=%u",
                         (unsigned)byte_rate,
                         (unsigned)expected_byte_rate,
                         (unsigned)block_align,
                         (unsigned)expected_block_align);
                return ESP_FAIL;
            }

            have_fmt = true;
        } else if (audio_chunk_id_matches(chunk, "data")) {
            view->pcm = wav_bytes + chunk_data_offset;
            view->pcm_bytes = chunk_size;
            have_data = true;
        }

        offset = chunk_end + (chunk_size & 1U);
    }

    if (!have_fmt || !have_data) {
        ESP_LOGE(TAG, "Missing fmt or data chunk in WAV");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t audio_write_pcm16(const int16_t *samples,
                                   size_t sample_count,
                                   size_t *written_bytes)
{
    if (samples == NULL || written_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_to_write = sample_count * sizeof(int16_t);
    esp_err_t ret = esp_codec_dev_write(s_audio_out_state.speaker_handle, (void *)samples, (int)bytes_to_write);
    if (ret != ESP_CODEC_DEV_OK) {
        return ret;
    }

    *written_bytes = bytes_to_write;
    return ESP_OK;
}

static size_t audio_jitter_level_locked(void)
{
    if (s_audio_out_state.jitter_total_in < s_audio_out_state.jitter_total_out) {
        return 0;
    }
    return s_audio_out_state.jitter_total_in - s_audio_out_state.jitter_total_out;
}

static bool audio_jitter_tail_within_tolerance_locked(void)
{
    return audio_jitter_level_locked() <= DEMO_REALTIME_AUDIO_CLOSE_TAIL_TOLERANCE_BYTES;
}

static void audio_jitter_update_level_stats_locked(size_t level)
{
    if (!s_audio_out_state.jitter_playback_started) {
        return;
    }
    if (level < s_audio_out_state.jitter_min_level) {
        s_audio_out_state.jitter_min_level = level;
    }
    if (level > s_audio_out_state.jitter_max_level) {
        s_audio_out_state.jitter_max_level = level;
    }
}

static void audio_jitter_copy_metrics_locked(audio_out_jitter_metrics_t *metrics)
{
    if (metrics == NULL) {
        return;
    }
    metrics->total_in = s_audio_out_state.jitter_total_in;
    metrics->total_out = s_audio_out_state.jitter_total_out;
    metrics->min_level = s_audio_out_state.jitter_min_level == SIZE_MAX
                             ? 0
                             : s_audio_out_state.jitter_min_level;
    metrics->max_level = s_audio_out_state.jitter_max_level;
    metrics->underrun_count = s_audio_out_state.jitter_underrun_count;
    metrics->underrun_us = s_audio_out_state.jitter_underrun_us;
    metrics->first_input_us = s_audio_out_state.jitter_first_input_us;
    metrics->playback_start_us = s_audio_out_state.jitter_playback_start_us;
    metrics->prebuffer_wait_us = s_audio_out_state.jitter_prebuffer_wait_us;
    metrics->playback_started = s_audio_out_state.jitter_playback_started;
}

static void audio_stream_task(void *arg)
{
    (void)arg;

    uint8_t scratch[DEMO_REALTIME_AUDIO_JITTER_READ_BYTES];
    size_t scratch_len = 0;
    bool prebuffer_done = false;

    while (true) {
        if (!prebuffer_done) {
            esp_err_t lock_ret = audio_out_lock();
            if (lock_ret == ESP_OK) {
                const size_t level = audio_jitter_level_locked();
                prebuffer_done = level >= DEMO_REALTIME_AUDIO_JITTER_PREBUFFER_BYTES ||
                                 !s_audio_out_state.stream_task_started;
                audio_out_unlock();
            }
            if (!prebuffer_done) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        size_t item_size = 0;
        const int64_t receive_start_us = esp_timer_get_time();
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_audio_out_state.jitter_ringbuf,
                                                      &item_size,
                                                      pdMS_TO_TICKS(20));
        if (item == NULL) {
            const int64_t receive_elapsed_us = esp_timer_get_time() - receive_start_us;
            esp_err_t lock_ret = audio_out_lock();
            bool should_exit = false;
            if (lock_ret == ESP_OK) {
                should_exit = !s_audio_out_state.stream_task_started &&
                              audio_jitter_level_locked() == 0;
                if (!should_exit && s_audio_out_state.jitter_playback_started) {
                    s_audio_out_state.jitter_underrun_count++;
                    s_audio_out_state.jitter_underrun_us += receive_elapsed_us;
                    audio_jitter_update_level_stats_locked(0);
                }
                audio_out_unlock();
            }
            if (should_exit) {
                break;
            }
            continue;
        }

        size_t item_offset = 0;
        while (item_offset < item_size) {
            const size_t copy_bytes = item_size - item_offset <
                                              (sizeof(scratch) - scratch_len)
                                          ? item_size - item_offset
                                          : (sizeof(scratch) - scratch_len);
            memcpy(scratch + scratch_len, item + item_offset, copy_bytes);
            scratch_len += copy_bytes;
            item_offset += copy_bytes;

            if (scratch_len == sizeof(scratch)) {
                size_t written_bytes = 0;
                const int64_t write_start_us = esp_timer_get_time();
                esp_err_t ret = audio_out_write_pcm_chunk(scratch, scratch_len, &written_bytes);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Buffered PCM write failed: %s", esp_err_to_name(ret));
                    esp_err_t lock_ret = audio_out_lock();
                    if (lock_ret == ESP_OK) {
                        s_audio_out_state.stream_task_result = ret;
                        audio_out_unlock();
                    }
                    vRingbufferReturnItem(s_audio_out_state.jitter_ringbuf, item);
                    goto done;
                }

                esp_err_t lock_ret = audio_out_lock();
                if (lock_ret == ESP_OK) {
                    if (!s_audio_out_state.jitter_playback_started) {
                        s_audio_out_state.jitter_playback_started = true;
                        s_audio_out_state.jitter_playback_start_us = write_start_us;
                        if (s_audio_out_state.jitter_first_input_us > 0) {
                            s_audio_out_state.jitter_prebuffer_wait_us =
                                write_start_us - s_audio_out_state.jitter_first_input_us;
                        }
                    }
                    s_audio_out_state.jitter_total_out += written_bytes;
                    audio_jitter_update_level_stats_locked(audio_jitter_level_locked());
                    audio_out_unlock();
                }
                scratch_len = 0;
            }
        }
        vRingbufferReturnItem(s_audio_out_state.jitter_ringbuf, item);
    }

    if (scratch_len > 0) {
        size_t written_bytes = 0;
        const int64_t write_start_us = esp_timer_get_time();
        esp_err_t ret = audio_out_write_pcm_chunk(scratch, scratch_len, &written_bytes);
        esp_err_t lock_ret = audio_out_lock();
        if (lock_ret == ESP_OK) {
            if (ret != ESP_OK) {
                s_audio_out_state.stream_task_result = ret;
            } else {
                if (!s_audio_out_state.jitter_playback_started) {
                    s_audio_out_state.jitter_playback_started = true;
                    s_audio_out_state.jitter_playback_start_us = write_start_us;
                    if (s_audio_out_state.jitter_first_input_us > 0) {
                        s_audio_out_state.jitter_prebuffer_wait_us =
                            write_start_us - s_audio_out_state.jitter_first_input_us;
                    }
                }
                s_audio_out_state.jitter_total_out += written_bytes;
                audio_jitter_update_level_stats_locked(audio_jitter_level_locked());
            }
            audio_out_unlock();
        }
    }

done:
    size_t total_in = 0;
    size_t total_out = 0;
    size_t min_level = 0;
    size_t max_level = 0;
    uint32_t underrun_count = 0;
    int64_t underrun_us = 0;
    int64_t prebuffer_wait_us = 0;
    esp_err_t task_result = ESP_OK;
    if (audio_out_lock() == ESP_OK) {
        total_in = s_audio_out_state.jitter_total_in;
        total_out = s_audio_out_state.jitter_total_out;
        min_level = s_audio_out_state.jitter_min_level == SIZE_MAX
                        ? 0
                        : s_audio_out_state.jitter_min_level;
        max_level = s_audio_out_state.jitter_max_level;
        underrun_count = s_audio_out_state.jitter_underrun_count;
        underrun_us = s_audio_out_state.jitter_underrun_us;
        prebuffer_wait_us = s_audio_out_state.jitter_prebuffer_wait_us;
        task_result = s_audio_out_state.stream_task_result;
        s_audio_out_state.stream_task_done = true;
        audio_out_unlock();
    }
    ESP_LOGI(TAG,
             "Realtime jitter playback task done total_in=%u total_out=%u min_level=%u max_level=%u underrun_count=%u underrun_ms=%.1f prebuffer_wait_ms=%.1f result=%s",
             (unsigned)total_in,
             (unsigned)total_out,
             (unsigned)min_level,
             (unsigned)max_level,
             (unsigned)underrun_count,
             (double)underrun_us / 1000.0,
             (double)prebuffer_wait_us / 1000.0,
             esp_err_to_name(task_result));
    vTaskDelete(NULL);
}

esp_err_t audio_out_open_pcm_stream(uint32_t sample_rate,
                                    uint16_t channels,
                                    uint16_t bits_per_sample)
{
    if (sample_rate == 0 || channels != 1 || bits_per_sample != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_install_output_locked(sample_rate);
    if (ret != ESP_OK) {
        audio_out_unlock();
        return ret;
    }

    if (s_audio_out_state.opened) {
        ret = audio_out_close_locked();
        if (ret != ESP_OK) {
            audio_out_unlock();
            return ret;
        }
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 1,
        .channel_mask = 0,
        .bits_per_sample = 16,
        .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(s_audio_out_state.speaker_handle, &fs);
    if (ret == ESP_CODEC_DEV_OK) {
        s_audio_out_state.opened = true;
        s_audio_out_state.stream_task_started = false;
        s_audio_out_state.stream_task = NULL;
        s_audio_out_state.stream_task_done = true;
        s_audio_out_state.stream_task_result = ESP_OK;
        s_audio_out_state.jitter_total_in = 0;
        s_audio_out_state.jitter_total_out = 0;
        s_audio_out_state.jitter_min_level = SIZE_MAX;
        s_audio_out_state.jitter_max_level = 0;
        s_audio_out_state.jitter_underrun_count = 0;
        s_audio_out_state.jitter_underrun_us = 0;
        s_audio_out_state.jitter_first_input_us = 0;
        s_audio_out_state.jitter_playback_start_us = 0;
        s_audio_out_state.jitter_prebuffer_wait_us = 0;
        s_audio_out_state.jitter_playback_started = false;
        if (s_audio_out_state.jitter_ringbuf != NULL) {
            vRingbufferDelete(s_audio_out_state.jitter_ringbuf);
            s_audio_out_state.jitter_ringbuf = NULL;
        }
    } else {
        ESP_LOGE(TAG, "Failed to open PCM stream: %s", esp_err_to_name(ret));
    }

    audio_out_unlock();
    return ret;
}

esp_err_t audio_out_write_pcm_chunk(const uint8_t *pcm_bytes,
                                    size_t pcm_bytes_size,
                                    size_t *written_bytes)
{
    if (pcm_bytes == NULL || pcm_bytes_size == 0 || written_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((pcm_bytes_size % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_audio_out_state.opened || s_audio_out_state.speaker_handle == NULL) {
        audio_out_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_write_pcm16((const int16_t *)pcm_bytes, pcm_bytes_size / sizeof(int16_t), written_bytes);
    audio_out_unlock();
    return ret;
}

esp_err_t audio_out_write_pcm_chunk_buffered(const uint8_t *pcm_bytes,
                                             size_t pcm_bytes_size,
                                             size_t *written_bytes)
{
    if (pcm_bytes == NULL || pcm_bytes_size == 0 || written_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((pcm_bytes_size % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!s_audio_out_state.opened || s_audio_out_state.speaker_handle == NULL) {
        audio_out_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_audio_out_state.jitter_ringbuf == NULL) {
        s_audio_out_state.jitter_ringbuf =
            xRingbufferCreate(DEMO_REALTIME_AUDIO_JITTER_BUFFER_BYTES, RINGBUF_TYPE_BYTEBUF);
        if (s_audio_out_state.jitter_ringbuf == NULL) {
            audio_out_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_audio_out_state.stream_task_started) {
        s_audio_out_state.stream_task_started = true;
        s_audio_out_state.stream_task_done = false;
        BaseType_t task_ret = xTaskCreate(audio_stream_task,
                                          "audio_stream",
                                          4096,
                                          NULL,
                                          DEMO_PIPELINE_TASK_PRIORITY + 1,
                                          &s_audio_out_state.stream_task);
        if (task_ret != pdPASS) {
            s_audio_out_state.stream_task_started = false;
            s_audio_out_state.stream_task = NULL;
            s_audio_out_state.stream_task_done = true;
            audio_out_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    ret = s_audio_out_state.stream_task_result;
    audio_out_unlock();
    if (ret != ESP_OK) {
        return ret;
    }

    const int64_t input_us = esp_timer_get_time();
    ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_audio_out_state.jitter_first_input_us == 0) {
        s_audio_out_state.jitter_first_input_us = input_us;
    }
    audio_out_unlock();

    if (xRingbufferSend(s_audio_out_state.jitter_ringbuf,
                        (void *)pcm_bytes,
                        pcm_bytes_size,
                        pdMS_TO_TICKS(DEMO_AUDIO_PLAY_WRITE_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    s_audio_out_state.jitter_total_in += pcm_bytes_size;
    const size_t level = audio_jitter_level_locked();
    audio_jitter_update_level_stats_locked(level);
    if (!s_audio_out_state.jitter_playback_started && level > s_audio_out_state.jitter_max_level) {
        s_audio_out_state.jitter_max_level = level;
    }
    audio_out_unlock();

    *written_bytes = pcm_bytes_size;
    return ESP_OK;
}

esp_err_t audio_out_close_pcm_stream_with_metrics(audio_out_jitter_metrics_t *metrics)
{
    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_audio_out_state.stream_task != NULL) {
        s_audio_out_state.stream_task_started = false;
        audio_out_unlock();
        TickType_t last_log_tick = 0;
        const int64_t wait_start_us = esp_timer_get_time();
        while (true) {
            ret = audio_out_lock();
            if (ret != ESP_OK) {
                return ret;
            }
            const bool stream_task_done = s_audio_out_state.stream_task_done;
            const size_t total_in = s_audio_out_state.jitter_total_in;
            const size_t total_out = s_audio_out_state.jitter_total_out;
            const size_t level = audio_jitter_level_locked();
            const uint32_t underrun_count = s_audio_out_state.jitter_underrun_count;
            const int64_t underrun_us = s_audio_out_state.jitter_underrun_us;
            const bool playback_started = s_audio_out_state.jitter_playback_started;
            const bool tail_within_tolerance =
                playback_started && audio_jitter_tail_within_tolerance_locked();
            TaskHandle_t stream_task = s_audio_out_state.stream_task;
            audio_out_unlock();
            if (stream_task_done) {
                break;
            }
            if (tail_within_tolerance) {
                ESP_LOGW(TAG,
                         "Realtime jitter close accepting tail remainder total_in=%u total_out=%u level=%u tolerance=%u",
                         (unsigned)total_in,
                         (unsigned)total_out,
                         (unsigned)level,
                         (unsigned)DEMO_REALTIME_AUDIO_CLOSE_TAIL_TOLERANCE_BYTES);
                ret = audio_out_lock();
                if (ret != ESP_OK) {
                    return ret;
                }
                s_audio_out_state.stream_task_result = ESP_OK;
                if (stream_task != NULL) {
                    vTaskDelete(stream_task);
                }
                s_audio_out_state.stream_task = NULL;
                s_audio_out_state.stream_task_done = true;
                audio_out_unlock();
                break;
            }
            const int64_t wait_elapsed_us = esp_timer_get_time() - wait_start_us;
            if (DEMO_REALTIME_AUDIO_CLOSE_WAIT_TIMEOUT_MS > 0 &&
                wait_elapsed_us >= ((int64_t)DEMO_REALTIME_AUDIO_CLOSE_WAIT_TIMEOUT_MS * 1000)) {
                ESP_LOGE(TAG,
                         "Realtime jitter close timeout forcing task delete total_in=%u total_out=%u level=%u playback_started=%d underrun_count=%u underrun_ms=%.1f wait_ms=%.1f",
                         (unsigned)total_in,
                         (unsigned)total_out,
                         (unsigned)level,
                         playback_started ? 1 : 0,
                         (unsigned)underrun_count,
                         (double)underrun_us / 1000.0,
                         (double)wait_elapsed_us / 1000.0);
                if (stream_task != NULL) {
                    vTaskDelete(stream_task);
                }
                ret = audio_out_lock();
                if (ret != ESP_OK) {
                    return ret;
                }
                s_audio_out_state.stream_task = NULL;
                s_audio_out_state.stream_task_done = true;
                s_audio_out_state.stream_task_result = ESP_ERR_TIMEOUT;
                audio_out_unlock();
                break;
            }
            const TickType_t log_interval_ticks =
                pdMS_TO_TICKS(DEMO_REALTIME_AUDIO_CLOSE_WAIT_LOG_MS);
            const TickType_t now_tick = xTaskGetTickCount();
            if (log_interval_ticks == 0 || now_tick - last_log_tick >= log_interval_ticks) {
                last_log_tick = now_tick;
                ESP_LOGW(TAG,
                         "Realtime jitter close waiting total_in=%u total_out=%u level=%u playback_started=%d underrun_count=%u underrun_ms=%.1f",
                         (unsigned)total_in,
                         (unsigned)total_out,
                         (unsigned)level,
                         playback_started ? 1 : 0,
                         (unsigned)underrun_count,
                         (double)underrun_us / 1000.0);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ret = audio_out_lock();
        if (ret != ESP_OK) {
            return ret;
        }
        s_audio_out_state.stream_task = NULL;
        s_audio_out_state.stream_task_done = true;
    }

    if (s_audio_out_state.stream_task_result != ESP_OK) {
        ret = s_audio_out_state.stream_task_result;
    } else {
        ret = ESP_OK;
    }
    audio_jitter_copy_metrics_locked(metrics);
    if (s_audio_out_state.jitter_ringbuf != NULL) {
        vRingbufferDelete(s_audio_out_state.jitter_ringbuf);
        s_audio_out_state.jitter_ringbuf = NULL;
    }
    s_audio_out_state.jitter_total_in = 0;
    s_audio_out_state.jitter_total_out = 0;
    s_audio_out_state.jitter_min_level = 0;
    s_audio_out_state.jitter_max_level = 0;
    s_audio_out_state.jitter_underrun_count = 0;
    s_audio_out_state.jitter_underrun_us = 0;
    s_audio_out_state.jitter_first_input_us = 0;
    s_audio_out_state.jitter_playback_start_us = 0;
    s_audio_out_state.jitter_prebuffer_wait_us = 0;
    s_audio_out_state.jitter_playback_started = false;

    if (ret != ESP_OK) {
        (void)audio_out_close_locked();
        audio_out_unlock();
        return ret;
    }

    ret = audio_out_close_locked();
    audio_out_unlock();
    return ret;
}

esp_err_t audio_out_close_pcm_stream(void)
{
    return audio_out_close_pcm_stream_with_metrics(NULL);
}

static esp_err_t audio_play_view(const wav_pcm_view_t *view)
{
    if (view == NULL || view->pcm == NULL || view->pcm_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_install_output_locked(view->sample_rate);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = view->sample_rate,
        .channel = 1,
        .channel_mask = 0,
        .bits_per_sample = 16,
        .mclk_multiple = 0,
    };
    ret = esp_codec_dev_open(s_audio_out_state.speaker_handle, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open speaker codec: %s", esp_err_to_name(ret));
        return ret;
    }
    s_audio_out_state.opened = true;

    ESP_LOGI(TAG,
             "Playing WAV: rate=%u Hz channels=%u bits=%u pcm_bytes=%u",
             (unsigned)view->sample_rate,
             (unsigned)view->channels,
             (unsigned)view->bits_per_sample,
             (unsigned)view->pcm_bytes);

    const int64_t start_us = esp_timer_get_time();
    uint8_t tx_buffer[DEMO_AUDIO_PLAY_CHUNK_BYTES];
    size_t src_offset = 0;
    size_t total_written = 0;

    while (src_offset < view->pcm_bytes) {
        int16_t *out_samples = (int16_t *)tx_buffer;
        size_t out_sample_count = 0;

        if (view->bits_per_sample == 16 && view->channels == 1) {
            size_t bytes_remaining = view->pcm_bytes - src_offset;
            size_t to_copy = bytes_remaining;
            if (to_copy > sizeof(tx_buffer)) {
                to_copy = sizeof(tx_buffer);
            }
            to_copy -= to_copy % sizeof(int16_t);
            memcpy(tx_buffer, view->pcm + src_offset, to_copy);
            src_offset += to_copy;
            out_sample_count = to_copy / sizeof(int16_t);
        } else if (view->bits_per_sample == 16 && view->channels == 2) {
            size_t frames_available = (view->pcm_bytes - src_offset) / (sizeof(int16_t) * 2);
            size_t frames_to_copy = sizeof(tx_buffer) / sizeof(int16_t);
            if (frames_to_copy > frames_available) {
                frames_to_copy = frames_available;
            }

            for (size_t i = 0; i < frames_to_copy; ++i) {
                const int16_t left = (int16_t)audio_read_le16(view->pcm + src_offset);
                src_offset += sizeof(int16_t) * 2;
                out_samples[i] = left;
            }
            out_sample_count = frames_to_copy;
        } else if (view->bits_per_sample == 8 && view->channels == 1) {
            size_t samples_available = view->pcm_bytes - src_offset;
            size_t samples_to_copy = sizeof(tx_buffer) / sizeof(int16_t);
            if (samples_to_copy > samples_available) {
                samples_to_copy = samples_available;
            }

            for (size_t i = 0; i < samples_to_copy; ++i) {
                out_samples[i] = (int16_t)(((int)view->pcm[src_offset + i] - 128) << 8);
            }
            src_offset += samples_to_copy;
            out_sample_count = samples_to_copy;
        } else if (view->bits_per_sample == 8 && view->channels == 2) {
            size_t frames_available = (view->pcm_bytes - src_offset) / 2;
            size_t frames_to_copy = sizeof(tx_buffer) / sizeof(int16_t);
            if (frames_to_copy > frames_available) {
                frames_to_copy = frames_available;
            }

            for (size_t i = 0; i < frames_to_copy; ++i) {
                out_samples[i] = (int16_t)(((int)view->pcm[src_offset] - 128) << 8);
                src_offset += 2;
            }
            out_sample_count = frames_to_copy;
        } else {
            ESP_LOGE(TAG, "Unsupported WAV stream layout");
            (void)audio_out_close_locked();
            return ESP_FAIL;
        }

        if (out_sample_count == 0) {
            break;
        }

        size_t bytes_written = 0;
        ret = audio_write_pcm16(out_samples, out_sample_count, &bytes_written);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker write failed: %s", esp_err_to_name(ret));
            (void)audio_out_close_locked();
            return ret;
        }

        total_written += bytes_written;
    }

    ret = audio_out_close_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    ESP_LOGI(TAG, "Playback finished: %u bytes in %.3f s",
             (unsigned)total_written,
             (double)elapsed_us / 1000000.0);
    return ESP_OK;
}

esp_err_t audio_out_download_wav_url(const char *audio_url,
                                     uint8_t **out_wav_bytes,
                                     size_t *out_wav_size)
{
    if (audio_url == NULL || out_wav_bytes == NULL || out_wav_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    *out_wav_bytes = NULL;
    *out_wav_size = 0;

    audio_buffer_t buffer = {0};
    ret = audio_buffer_init(&buffer, DEMO_WAV_DOWNLOAD_INITIAL_BYTES);
    if (ret != ESP_OK) {
        audio_out_unlock();
        return ret;
    }

    ret = audio_http_download(audio_url, &buffer);
    if (ret != ESP_OK) {
        audio_buffer_free(&buffer);
        audio_out_unlock();
        return ret;
    }

    if (buffer.len == 0) {
        audio_buffer_free(&buffer);
        audio_out_unlock();
        return ESP_FAIL;
    }

    *out_wav_bytes = (uint8_t *)buffer.data;
    *out_wav_size = buffer.len;
    audio_out_unlock();
    return ESP_OK;
}

esp_err_t audio_out_play_wav_buffer(const uint8_t *wav_bytes,
                                    size_t wav_bytes_size)
{
    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    wav_pcm_view_t view = {0};
    ret = audio_parse_wav(wav_bytes, wav_bytes_size, &view);
    if (ret != ESP_OK) {
        audio_out_unlock();
        return ret;
    }

    ret = audio_validate_wav_pcm_layout(&view);
    if (ret != ESP_OK) {
        audio_out_unlock();
        return ret;
    }

    ret = audio_play_view(&view);
    audio_out_unlock();
    return ret;
}

esp_err_t audio_out_play_wav_url(const char *audio_url)
{
    esp_err_t ret = audio_out_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *wav_bytes = NULL;
    size_t wav_bytes_size = 0;

    ret = audio_out_download_wav_url(audio_url, &wav_bytes, &wav_bytes_size);
    if (ret != ESP_OK) {
        audio_out_unlock();
        return ret;
    }

    ret = audio_out_play_wav_buffer(wav_bytes, wav_bytes_size);
    free(wav_bytes);
    audio_out_unlock();
    return ret;
}

esp_err_t audio_out_play_pcm_file(const char *path,
                                  uint32_t sample_rate,
                                  uint16_t channels,
                                  uint16_t bits_per_sample,
                                  size_t max_bytes)
{
    if (path == NULL || path[0] == '\0' || sample_rate == 0 ||
        channels != 1 || bits_per_sample != 16 || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open PCM file: %s", path);
        return ESP_FAIL;
    }

    esp_err_t ret = audio_out_open_pcm_stream(sample_rate, channels, bits_per_sample);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    ESP_LOGI(TAG, "Playing PCM file: path=%s rate=%u max_bytes=%u",
             path, (unsigned)sample_rate, (unsigned)max_bytes);

    uint8_t chunk[DEMO_AUDIO_PLAY_CHUNK_BYTES];
    size_t total_read = 0;
    size_t total_written = 0;
    const int64_t start_us = esp_timer_get_time();

    while (total_read < max_bytes) {
        size_t to_read = sizeof(chunk);
        if (to_read > max_bytes - total_read) {
            to_read = max_bytes - total_read;
        }
        to_read -= to_read % sizeof(int16_t);
        if (to_read == 0) {
            break;
        }

        const size_t read_bytes = fread(chunk, 1, to_read, file);
        if (read_bytes == 0) {
            if (ferror(file)) {
                ESP_LOGE(TAG, "Failed to read PCM file: %s", path);
                ret = ESP_FAIL;
            }
            break;
        }

        const size_t aligned_read_bytes = read_bytes - (read_bytes % sizeof(int16_t));
        if (aligned_read_bytes == 0) {
            break;
        }

        size_t written_bytes = 0;
        ret = audio_out_write_pcm_chunk(chunk, aligned_read_bytes, &written_bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PCM file speaker write failed: %s", esp_err_to_name(ret));
            break;
        }

        total_read += aligned_read_bytes;
        total_written += written_bytes;
        if (read_bytes < to_read) {
            break;
        }
    }

    esp_err_t close_ret = audio_out_close_pcm_stream();
    fclose(file);
    if (ret == ESP_OK && close_ret != ESP_OK) {
        ret = close_ret;
    }

    ESP_LOGI(TAG,
             "PCM file playback done: path=%s read_bytes=%u written_bytes=%u elapsed_ms=%.1f",
             path,
             (unsigned)total_read,
             (unsigned)total_written,
             (double)(esp_timer_get_time() - start_us) / 1000.0);
    return ret;
}

void audio_out_deinit(void)
{
    if (audio_out_lock() != ESP_OK) {
        return;
    }

    audio_out_deinit_locked();
    audio_out_unlock();
}
