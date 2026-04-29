#include "audio_in.h"

#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_vocat.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio_in";

typedef struct {
    bool initialized;
    bool opened;
    esp_codec_dev_handle_t mic_handle;
} audio_in_state_t;

static audio_in_state_t s_audio_in_state = {
    .initialized = false,
    .opened = false,
    .mic_handle = NULL,
};

static void audio_in_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "heap stage=%s free_8bit=%u largest_8bit=%u free_spiram=%u largest_spiram=%u",
             stage != NULL ? stage : "unknown",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static esp_codec_dev_sample_info_t audio_in_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = DEMO_AUDIO_SAMPLE_RATE,
        .channel = DEMO_AUDIO_CHANNELS,
        .channel_mask = 0,
        .bits_per_sample = DEMO_AUDIO_BITS_PER_SAMPLE,
        .mclk_multiple = 0,
    };
    return fs;
}

static uint32_t audio_in_avg_abs_pcm16_le(const uint8_t *pcm, size_t pcm_bytes)
{
    if (pcm == NULL || pcm_bytes < DEMO_AUDIO_BYTES_PER_SAMPLE) {
        return 0;
    }

    const size_t sample_count = pcm_bytes / DEMO_AUDIO_BYTES_PER_SAMPLE;
    uint64_t sum_abs = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        const size_t offset = i * DEMO_AUDIO_BYTES_PER_SAMPLE;
        const uint16_t raw = (uint16_t)pcm[offset] | ((uint16_t)pcm[offset + 1] << 8);
        const int32_t sample = (int16_t)raw;
        sum_abs += (uint32_t)(sample < 0 ? -sample : sample);
    }
    return (uint32_t)(sum_abs / sample_count);
}

static esp_err_t audio_in_init_locked(void)
{
    if (s_audio_in_state.initialized) {
        return ESP_OK;
    }

    esp_err_t ret = bsp_audio_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP-VoCat audio bus: %s", esp_err_to_name(ret));
        return ret;
    }

    s_audio_in_state.mic_handle = bsp_audio_codec_microphone_init();
    if (s_audio_in_state.mic_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize ESP-VoCat microphone codec");
        return ESP_FAIL;
    }

    ret = esp_codec_dev_set_in_gain(s_audio_in_state.mic_handle, DEMO_AUDIO_INPUT_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set microphone gain to %.1f dB: %s",
                 (double)DEMO_AUDIO_INPUT_GAIN_DB,
                 esp_err_to_name(ret));
    }

    s_audio_in_state.initialized = true;
    ESP_LOGI(TAG,
             "Configured ESP-VoCat microphone: rate=%d bits=%d channels=%d gain=%.1f dB",
             DEMO_AUDIO_SAMPLE_RATE,
             DEMO_AUDIO_BITS_PER_SAMPLE,
             DEMO_AUDIO_CHANNELS,
             (double)DEMO_AUDIO_INPUT_GAIN_DB);
    return ESP_OK;
}

static esp_err_t audio_in_open_locked(void)
{
    esp_err_t ret = audio_in_init_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_audio_in_state.opened) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = audio_in_sample_info();
    ret = esp_codec_dev_open(s_audio_in_state.mic_handle, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open microphone codec: %s", esp_err_to_name(ret));
        return ret;
    }
    s_audio_in_state.opened = true;
    return ESP_OK;
}

static void audio_in_close_locked(void)
{
    if (s_audio_in_state.opened && s_audio_in_state.mic_handle != NULL) {
        (void)esp_codec_dev_close(s_audio_in_state.mic_handle);
        s_audio_in_state.opened = false;
    }
}

static uint8_t *audio_in_alloc_record_buffer(void)
{
    audio_in_log_heap("before_record_alloc");
    uint8_t *buffer = heap_caps_malloc(DEMO_AUDIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(DEMO_AUDIO_BUFFER_BYTES, MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte PCM buffer",
                 (unsigned)DEMO_AUDIO_BUFFER_BYTES);
        audio_in_log_heap("record_alloc_failed");
        return NULL;
    }
    audio_in_log_heap("after_record_alloc");
    return buffer;
}

static esp_err_t audio_in_read_chunk(uint8_t *buffer, size_t chunk_bytes)
{
    esp_err_t ret = esp_codec_dev_read(s_audio_in_state.mic_handle, buffer, (int)chunk_bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Mic read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static esp_err_t audio_in_record_after_voice_start_impl(const uint8_t *speech_prefix,
                                                        size_t speech_prefix_bytes,
                                                        uint8_t **out_buffer,
                                                        size_t *out_bytes,
                                                        audio_in_record_metrics_t *out_metrics)
{
    if (out_buffer == NULL || out_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_buffer = NULL;
    *out_bytes = 0;
    if (out_metrics != NULL) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }

    if (!s_audio_in_state.opened || s_audio_in_state.mic_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (speech_prefix_bytes > DEMO_AUDIO_BUFFER_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buffer = audio_in_alloc_record_buffer();
    if (buffer == NULL) {
        audio_in_close_locked();
        return ESP_ERR_NO_MEM;
    }

    size_t pcm_offset = 0;
    if (speech_prefix != NULL && speech_prefix_bytes > 0) {
        memcpy(buffer, speech_prefix, speech_prefix_bytes);
        pcm_offset = speech_prefix_bytes;
    }

    uint32_t max_chunk_level = 0;
    if (speech_prefix != NULL && speech_prefix_bytes > 0) {
        max_chunk_level = audio_in_avg_abs_pcm16_le(speech_prefix, speech_prefix_bytes);
    }

    const int64_t start_us = esp_timer_get_time();
    size_t trailing_silence_bytes = 0;
    bool voice_started = speech_prefix_bytes > 0;
    bool vad_stopped = false;

    while (pcm_offset < DEMO_AUDIO_BUFFER_BYTES) {
        const size_t chunk_bytes = DEMO_AUDIO_BUFFER_BYTES - pcm_offset > DEMO_AUDIO_CHUNK_BYTES ?
                                   DEMO_AUDIO_CHUNK_BYTES :
                                   DEMO_AUDIO_BUFFER_BYTES - pcm_offset;
        esp_err_t ret = audio_in_read_chunk(buffer + pcm_offset, chunk_bytes);
        if (ret != ESP_OK) {
            free(buffer);
            audio_in_close_locked();
            return ret;
        }
        pcm_offset += chunk_bytes;

#if DEMO_RECORD_VAD_ENABLED
        const uint32_t chunk_level = audio_in_avg_abs_pcm16_le(buffer + pcm_offset - chunk_bytes, chunk_bytes);
        if (chunk_level > max_chunk_level) {
            max_chunk_level = chunk_level;
        }
        if (!voice_started && chunk_level >= DEMO_RECORD_VAD_START_THRESHOLD) {
            voice_started = true;
            trailing_silence_bytes = 0;
        } else if (voice_started) {
            if (chunk_level <= DEMO_RECORD_VAD_SILENCE_THRESHOLD) {
                trailing_silence_bytes += chunk_bytes;
            } else {
                trailing_silence_bytes = 0;
            }
        }

        if (voice_started &&
            pcm_offset >= DEMO_RECORD_MIN_BYTES &&
            trailing_silence_bytes >= DEMO_RECORD_VAD_SILENCE_BYTES) {
            vad_stopped = true;
            break;
        }
#endif
    }

    audio_in_close_locked();

    if (out_metrics != NULL) {
        out_metrics->vad_stopped = vad_stopped;
        out_metrics->voice_started = voice_started;
        out_metrics->max_level = max_chunk_level;
        out_metrics->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    }

    ESP_LOGI(TAG,
             "Captured %u bytes in %.3f s vad_enabled=%d vad_stopped=%d voice_started=%d max_level=%u",
             (unsigned)pcm_offset,
             (double)(esp_timer_get_time() - start_us) / 1000000.0,
             DEMO_RECORD_VAD_ENABLED,
             vad_stopped,
             voice_started,
             (unsigned)max_chunk_level);

    *out_buffer = buffer;
    *out_bytes = pcm_offset;
    return ESP_OK;
}

esp_err_t audio_in_wait_for_speech_start(uint8_t **out_speech_prefix,
                                         size_t *out_speech_prefix_bytes,
                                         audio_in_wait_metrics_t *out_metrics)
{
    if (out_speech_prefix == NULL || out_speech_prefix_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_speech_prefix = NULL;
    *out_speech_prefix_bytes = 0;
    if (out_metrics != NULL) {
        memset(out_metrics, 0, sizeof(*out_metrics));
    }

    esp_err_t ret = audio_in_open_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *chunk = malloc(DEMO_AUDIO_CHUNK_BYTES);
    uint8_t *speech_prefix = calloc(1, DEMO_SPEECH_START_HOLD_BYTES + DEMO_AUDIO_CHUNK_BYTES);
    if (chunk == NULL || speech_prefix == NULL) {
        free(chunk);
        free(speech_prefix);
        audio_in_close_locked();
        return ESP_ERR_NO_MEM;
    }

    const int64_t wait_start_us = esp_timer_get_time();
    const int64_t armed_at_us = wait_start_us + (int64_t)DEMO_WAITING_SPEECH_ARM_MS * 1000;
    const int64_t timeout_at_us = wait_start_us + (int64_t)DEMO_WAIT_FOR_SPEECH_TIMEOUT_MS * 1000;
    size_t hold_bytes = 0;
    size_t prefix_bytes = 0;
    uint32_t max_level = 0;
    bool armed_logged = false;

    while (esp_timer_get_time() < timeout_at_us) {
        ret = audio_in_read_chunk(chunk, DEMO_AUDIO_CHUNK_BYTES);
        if (ret != ESP_OK) {
            free(chunk);
            free(speech_prefix);
            audio_in_close_locked();
            return ret;
        }

        const int64_t now_us = esp_timer_get_time();
        const uint32_t chunk_level = audio_in_avg_abs_pcm16_le(chunk, DEMO_AUDIO_CHUNK_BYTES);
        if (chunk_level > max_level) {
            max_level = chunk_level;
        }

        if (now_us < armed_at_us) {
            continue;
        }

        if (!armed_logged) {
            ESP_LOGI(TAG,
                     "stage=waiting_speech event=armed elapsed_ms=%u",
                     (unsigned)((now_us - wait_start_us) / 1000));
            armed_logged = true;
        }

        if (chunk_level >= DEMO_RECORD_VAD_START_THRESHOLD) {
            const size_t prefix_capacity = DEMO_SPEECH_START_HOLD_BYTES + DEMO_AUDIO_CHUNK_BYTES;
            if (prefix_bytes + DEMO_AUDIO_CHUNK_BYTES > prefix_capacity) {
                memmove(speech_prefix,
                        speech_prefix + DEMO_AUDIO_CHUNK_BYTES,
                        prefix_bytes - DEMO_AUDIO_CHUNK_BYTES);
                prefix_bytes -= DEMO_AUDIO_CHUNK_BYTES;
            }
            memcpy(speech_prefix + prefix_bytes, chunk, DEMO_AUDIO_CHUNK_BYTES);
            prefix_bytes += DEMO_AUDIO_CHUNK_BYTES;
            hold_bytes += DEMO_AUDIO_CHUNK_BYTES;
            if (hold_bytes >= DEMO_SPEECH_START_HOLD_BYTES) {
                if (out_metrics != NULL) {
                    out_metrics->elapsed_ms = (uint32_t)((now_us - wait_start_us) / 1000);
                    out_metrics->max_level = max_level;
                    out_metrics->speech_prefix_bytes = prefix_bytes;
                }
                *out_speech_prefix = speech_prefix;
                *out_speech_prefix_bytes = prefix_bytes;
                free(chunk);
                return ESP_OK;
            }
        } else {
            hold_bytes = 0;
            prefix_bytes = 0;
        }
    }

    if (out_metrics != NULL) {
        out_metrics->elapsed_ms = (uint32_t)((esp_timer_get_time() - wait_start_us) / 1000);
        out_metrics->max_level = max_level;
    }
    free(chunk);
    free(speech_prefix);
    audio_in_close_locked();
    return DEMO_AUDIO_IN_ERR_WAIT_TIMEOUT;
}

esp_err_t audio_in_record_after_speech_start(const uint8_t *speech_prefix,
                                             size_t speech_prefix_bytes,
                                             uint8_t **out_buffer,
                                             size_t *out_bytes,
                                             audio_in_record_metrics_t *out_metrics)
{
    return audio_in_record_after_voice_start_impl(speech_prefix,
                                                  speech_prefix_bytes,
                                                  out_buffer,
                                                  out_bytes,
                                                  out_metrics);
}

void audio_in_deinit(void)
{
    audio_in_close_locked();

    if (!s_audio_in_state.initialized) {
        return;
    }

    s_audio_in_state.mic_handle = NULL;
    s_audio_in_state.initialized = false;
}

esp_err_t audio_in_record_fixed_duration(uint8_t **out_buffer, size_t *out_bytes)
{
    esp_err_t ret = audio_in_open_locked();
    if (ret != ESP_OK) {
        return ret;
    }
    return audio_in_record_after_voice_start_impl(NULL, 0, out_buffer, out_bytes, NULL);
}
