#include "cloud_client.h"

#include "config.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_audio_dec.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "cloud_client";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cloud_response_buffer_t;

typedef struct {
    cloud_response_buffer_t *buffer;
} cloud_http_ctx_t;

typedef struct {
    char content_type[64];
    char audio_format[16];
    char audio_packetization[16];
    char sample_rate[16];
    char sample_width[16];
    char channels[16];
    char endian[16];
    char opus_sample_rate[16];
    char opus_channels[16];
    char opus_frame_duration_ms[16];
} cloud_audio_headers_t;

typedef enum {
    CLOUD_AUDIO_FORMAT_PCM = 0,
    CLOUD_AUDIO_FORMAT_OPUS = 1,
} cloud_audio_format_t;

typedef struct {
    void *decoder;
    uint8_t *pending;
    size_t pending_len;
    size_t pending_cap;
    uint8_t *pcm_buffer;
    size_t pcm_buffer_size;
} cloud_opus_decoder_state_t;

typedef struct {
    uint8_t *pending;
    size_t pending_len;
    size_t pending_cap;
    uint32_t expected_seq;
    bool expected_seq_valid;
} cloud_frame_stream_state_t;

typedef enum {
    CLOUD_STREAM_PACKET = 0,
    CLOUD_STREAM_EOF = 1,
    CLOUD_STREAM_ERROR = 2,
} cloud_stream_packet_type_t;

typedef struct {
    cloud_stream_packet_type_t type;
    cloud_audio_format_t audio_format;
    uint32_t sequence;
    uint8_t *payload;
    size_t payload_len;
    esp_err_t error;
} cloud_encoded_packet_t;

typedef struct {
    cloud_stream_packet_type_t type;
    uint32_t sequence;
    uint8_t *payload;
    size_t payload_len;
    uint32_t duration_ms;
    int64_t decode_elapsed_us;
    esp_err_t error;
} cloud_pcm_packet_t;

typedef struct {
    QueueHandle_t encoded_queue;
    QueueHandle_t pcm_queue;
    TaskHandle_t decode_task;
    TaskHandle_t playback_task;
    cloud_realtime_audio_chunk_callback_t callback;
    void *user_ctx;
    cloud_realtime_audio_metrics_t *metrics;
    cloud_audio_format_t audio_format;
    cloud_opus_decoder_state_t opus_decoder;
    volatile bool decode_done;
    volatile bool playback_done;
    esp_err_t decode_result;
    esp_err_t playback_result;
    int64_t decode_eof_us;
} cloud_stream_runtime_t;

static esp_err_t cloud_opus_pending_append(cloud_opus_decoder_state_t *state,
                                           const uint8_t *data,
                                           size_t data_len);
static esp_err_t cloud_decode_opus_pending_packets(cloud_opus_decoder_state_t *state,
                                                   cloud_realtime_audio_chunk_callback_t callback,
                                                   void *user_ctx);

static esp_err_t cloud_response_buffer_init(cloud_response_buffer_t *buffer, size_t initial_cap)
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

static void cloud_response_buffer_free(cloud_response_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static esp_err_t cloud_response_buffer_append(cloud_response_buffer_t *buffer,
                                             const char *data,
                                             size_t data_len)
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

static esp_err_t cloud_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_FAIL;
    }

    cloud_http_ctx_t *ctx = (cloud_http_ctx_t *)evt->user_data;
    if (ctx == NULL || ctx->buffer == NULL) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        return cloud_response_buffer_append(ctx->buffer, (const char *)evt->data, (size_t)evt->data_len);
    default:
        return ESP_OK;
    }
}

static bool cloud_is_unreserved_path_char(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static esp_err_t cloud_url_encode_path_segment(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static const char hex[] = "0123456789ABCDEF";

    size_t in_len = strlen(input);
    if (in_len == 0 || in_len >= DEMO_CLOUD_TASK_ID_MAX_LEN) {
        return DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }

    size_t out_len = 0;
    for (size_t i = 0; i < in_len; ++i) {
        const unsigned char c = (unsigned char)input[i];
        size_t needed = cloud_is_unreserved_path_char(c) ? 1 : 3;
        if (out_len + needed + 1 > output_size) {
            return ESP_ERR_NO_MEM;
        }

        if (cloud_is_unreserved_path_char(c)) {
            output[out_len++] = (char)c;
        } else {
            output[out_len++] = '%';
            output[out_len++] = hex[(c >> 4) & 0x0F];
            output[out_len++] = hex[c & 0x0F];
        }
    }

    output[out_len] = '\0';
    return ESP_OK;
}

static esp_err_t cloud_json_copy_string(const cJSON *item, char *out, size_t out_size)
{
    if (item == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }

    const int written = snprintf(out, out_size, "%s", item->valuestring);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cloud_json_get_string(const cJSON *object,
                                       const char *key,
                                       bool required,
                                       char *out,
                                       size_t out_size)
{
    if (object == NULL || key == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return required ? DEMO_CLOUD_ERR_INVALID_RESPONSE : ESP_OK;
    }

    return cloud_json_copy_string(item, out, out_size);
}

static esp_err_t cloud_json_parse_task_id(const char *json, char *task_id, size_t task_id_size)
{
    if (json == NULL || task_id == NULL || task_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = cloud_json_get_string(root, "task_id", true, task_id, task_id_size);
    if (ret == ESP_OK && task_id[0] == '\0') {
        ret = DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t cloud_json_parse_task_result(const char *json, cloud_task_result_t *result)
{
    if (json == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = cloud_json_get_string(root, "status", true, result->status, sizeof(result->status));
    if (ret == ESP_OK) {
        ret = cloud_json_get_string(root, "audio_url", false, result->audio_url, sizeof(result->audio_url));
    }
    if (ret == ESP_OK) {
        ret = cloud_json_get_string(root, "question_text", false, result->question_text, sizeof(result->question_text));
    }
    if (ret == ESP_OK) {
        ret = cloud_json_get_string(root, "error_code", false, result->error_code, sizeof(result->error_code));
    }

    cJSON_Delete(root);
    return ret;
}

static esp_err_t cloud_json_parse_realtime_session(const char *json, cloud_realtime_session_t *session)
{
    if (json == NULL || session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(session, 0, sizeof(*session));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return DEMO_CLOUD_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = cloud_json_get_string(root, "session_id", true, session->session_id, sizeof(session->session_id));
    if (ret == ESP_OK) {
        ret = cloud_json_get_string(root, "status", true, session->status, sizeof(session->status));
    }
    if (ret == ESP_OK) {
        ret = cloud_json_get_string(root, "audio_stream_url", true, session->audio_stream_url, sizeof(session->audio_stream_url));
    }

    cJSON_Delete(root);
    return ret;
}

static esp_err_t cloud_build_url(char *out, size_t out_size, const char *path)
{
    if (out == NULL || out_size == 0 || path == NULL || DEMO_SERVER_BASE_URL[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(DEMO_SERVER_BASE_URL);
    while (base_len > 0 && DEMO_SERVER_BASE_URL[base_len - 1] == '/') {
        --base_len;
    }

    int written = snprintf(out, out_size, "%.*s/%s", (int)base_len, DEMO_SERVER_BASE_URL, path);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cloud_http_execute(const esp_http_client_config_t *config,
                                    const uint8_t *body,
                                    size_t body_len,
                                    cloud_response_buffer_t *response,
                                    int *status_code)
{
    if (config == NULL || status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cloud_http_ctx_t ctx = {
        .buffer = response,
    };

    esp_http_client_config_t local_config = *config;
    local_config.event_handler = cloud_http_event_handler;
    local_config.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&local_config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (body != NULL && body_len > 0) {
        ret = esp_http_client_set_post_field(client, (const char *)body, (int)body_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set POST body: %s", esp_err_to_name(ret));
            esp_http_client_cleanup(client);
            return ret;
        }
    }

    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static bool cloud_is_retryable_poll_error(esp_err_t err)
{
    return err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT;
}

static void cloud_copy_header_value(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static esp_err_t cloud_realtime_audio_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == NULL ||
        evt->header_key == NULL || evt->header_value == NULL) {
        return ESP_OK;
    }

    cloud_audio_headers_t *headers = (cloud_audio_headers_t *)evt->user_data;
    if (strcasecmp(evt->header_key, "Content-Type") == 0) {
        cloud_copy_header_value(headers->content_type, sizeof(headers->content_type), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Format") == 0) {
        cloud_copy_header_value(headers->audio_format, sizeof(headers->audio_format), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Packetization") == 0) {
        cloud_copy_header_value(headers->audio_packetization, sizeof(headers->audio_packetization), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Sample-Rate") == 0) {
        cloud_copy_header_value(headers->sample_rate, sizeof(headers->sample_rate), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Sample-Width") == 0) {
        cloud_copy_header_value(headers->sample_width, sizeof(headers->sample_width), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Channels") == 0) {
        cloud_copy_header_value(headers->channels, sizeof(headers->channels), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Audio-Endian") == 0) {
        cloud_copy_header_value(headers->endian, sizeof(headers->endian), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Opus-Sample-Rate") == 0) {
        cloud_copy_header_value(headers->opus_sample_rate, sizeof(headers->opus_sample_rate), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Opus-Channels") == 0) {
        cloud_copy_header_value(headers->opus_channels, sizeof(headers->opus_channels), evt->header_value);
    } else if (strcasecmp(evt->header_key, "X-Opus-Frame-Duration-Ms") == 0) {
        cloud_copy_header_value(headers->opus_frame_duration_ms, sizeof(headers->opus_frame_duration_ms), evt->header_value);
    }
    return ESP_OK;
}

static void cloud_ascii_lowercase(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0) {
        return;
    }

    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_size; ++i) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (char)tolower((int)c);
    }
    dst[i] = '\0';
}

static esp_err_t cloud_get_header_case_insensitive(esp_http_client_handle_t client,
                                                   const char *header_key,
                                                   char **actual_value)
{
    if (client == NULL || header_key == NULL || actual_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *actual_value = NULL;
    esp_err_t ret = esp_http_client_get_header(client, header_key, actual_value);
    if (ret == ESP_OK && *actual_value != NULL) {
        return ESP_OK;
    }

    char lower_key[64];
    cloud_ascii_lowercase(header_key, lower_key, sizeof(lower_key));
    if (lower_key[0] == '\0') {
        return ret;
    }

    return esp_http_client_get_header(client, lower_key, actual_value);
}

static bool cloud_header_value_matches(const char *actual_value, const char *expected_value)
{
    if (actual_value == NULL || expected_value == NULL) {
        return false;
    }

    const size_t expected_len = strlen(expected_value);
    if (expected_len == 0) {
        return false;
    }

    // Accept exact value and common parameterized form: "application/octet-stream; ..."
    if (strncasecmp(actual_value, expected_value, expected_len) != 0) {
        return false;
    }
    return actual_value[expected_len] == '\0' || actual_value[expected_len] == ';';
}

static const char *cloud_audio_header_lookup(const cloud_audio_headers_t *headers,
                                             const char *header_key)
{
    if (headers == NULL || header_key == NULL) {
        return NULL;
    }
    if (strcasecmp(header_key, "Content-Type") == 0) {
        return headers->content_type;
    }
    if (strcasecmp(header_key, "X-Audio-Format") == 0) {
        return headers->audio_format;
    }
    if (strcasecmp(header_key, "X-Audio-Packetization") == 0) {
        return headers->audio_packetization;
    }
    if (strcasecmp(header_key, "X-Audio-Sample-Rate") == 0) {
        return headers->sample_rate;
    }
    if (strcasecmp(header_key, "X-Audio-Sample-Width") == 0) {
        return headers->sample_width;
    }
    if (strcasecmp(header_key, "X-Audio-Channels") == 0) {
        return headers->channels;
    }
    if (strcasecmp(header_key, "X-Audio-Endian") == 0) {
        return headers->endian;
    }
    if (strcasecmp(header_key, "X-Opus-Sample-Rate") == 0) {
        return headers->opus_sample_rate;
    }
    if (strcasecmp(header_key, "X-Opus-Channels") == 0) {
        return headers->opus_channels;
    }
    if (strcasecmp(header_key, "X-Opus-Frame-Duration-Ms") == 0) {
        return headers->opus_frame_duration_ms;
    }
    return NULL;
}

static esp_err_t cloud_validate_audio_header_exact(const cloud_audio_headers_t *headers,
                                                   esp_http_client_handle_t client,
                                                   const char *header_key,
                                                   const char *expected_value)
{
    const char *captured_value = cloud_audio_header_lookup(headers, header_key);
    if (captured_value != NULL && captured_value[0] != '\0') {
        if (!cloud_header_value_matches(captured_value, expected_value)) {
            ESP_LOGE(TAG, "Unexpected audio header: %s actual=%s expected=%s",
                     header_key,
                     captured_value,
                     expected_value);
            return DEMO_CLOUD_ERR_AUDIO_HEADER_MISMATCH;
        }
        return ESP_OK;
    }

    char *actual_value = NULL;
    esp_err_t ret = cloud_get_header_case_insensitive(client, header_key, &actual_value);
    if (ret != ESP_OK || actual_value == NULL) {
        ESP_LOGE(TAG, "Missing audio header: %s", header_key);
        return DEMO_CLOUD_ERR_AUDIO_HEADER_MISMATCH;
    }
    if (!cloud_header_value_matches(actual_value, expected_value)) {
        ESP_LOGE(TAG, "Unexpected audio header: %s actual=%s expected=%s",
                 header_key,
                 actual_value,
                 expected_value);
        return DEMO_CLOUD_ERR_AUDIO_HEADER_MISMATCH;
    }
    return ESP_OK;
}

static cloud_audio_format_t cloud_parse_audio_format(const cloud_audio_headers_t *headers,
                                                     esp_http_client_handle_t client,
                                                     esp_err_t *out_ret)
{
    if (out_ret != NULL) {
        *out_ret = ESP_OK;
    }
    char *actual_value = NULL;
    const char *captured_value = cloud_audio_header_lookup(headers, "X-Audio-Format");
    if (captured_value == NULL || captured_value[0] == '\0') {
        if (cloud_get_header_case_insensitive(client, "X-Audio-Format", &actual_value) != ESP_OK ||
            actual_value == NULL || actual_value[0] == '\0') {
            return CLOUD_AUDIO_FORMAT_PCM;
        }
        captured_value = actual_value;
    }

    if (cloud_header_value_matches(captured_value, "pcm")) {
        return CLOUD_AUDIO_FORMAT_PCM;
    }
    if (cloud_header_value_matches(captured_value, "opus")) {
        return CLOUD_AUDIO_FORMAT_OPUS;
    }
    ESP_LOGE(TAG, "Unexpected audio format header: %s", captured_value);
    if (out_ret != NULL) {
        *out_ret = DEMO_CLOUD_ERR_AUDIO_HEADER_MISMATCH;
    }
    return CLOUD_AUDIO_FORMAT_PCM;
}

static bool cloud_audio_packetization_is_framed_v1(const cloud_audio_headers_t *headers,
                                                   esp_http_client_handle_t client)
{
    char *actual_value = NULL;
    const char *captured_value = cloud_audio_header_lookup(headers, "X-Audio-Packetization");
    if (captured_value == NULL || captured_value[0] == '\0') {
        if (cloud_get_header_case_insensitive(client, "X-Audio-Packetization", &actual_value) != ESP_OK ||
            actual_value == NULL || actual_value[0] == '\0') {
            return false;
        }
        captured_value = actual_value;
    }
    return cloud_header_value_matches(captured_value, "framed-v1");
}

static void cloud_frame_stream_cleanup(cloud_frame_stream_state_t *state)
{
    if (state == NULL) {
        return;
    }
    free(state->pending);
    state->pending = NULL;
    state->pending_len = 0;
    state->pending_cap = 0;
    state->expected_seq = 0;
    state->expected_seq_valid = false;
}

static cloud_encoded_packet_t *cloud_encoded_packet_alloc(cloud_stream_packet_type_t type)
{
    cloud_encoded_packet_t *packet = calloc(1, sizeof(*packet));
    if (packet != NULL) {
        packet->type = type;
    }
    return packet;
}

static void cloud_encoded_packet_free(cloud_encoded_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }
    free(packet->payload);
    free(packet);
}

static cloud_pcm_packet_t *cloud_pcm_packet_alloc(cloud_stream_packet_type_t type)
{
    cloud_pcm_packet_t *packet = calloc(1, sizeof(*packet));
    if (packet != NULL) {
        packet->type = type;
    }
    return packet;
}

static void cloud_pcm_packet_free(cloud_pcm_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }
    free(packet->payload);
    free(packet);
}

static void cloud_metrics_update_queue_peak(size_t *peak, QueueHandle_t queue)
{
    if (peak == NULL || queue == NULL) {
        return;
    }
    const size_t used = (size_t)uxQueueMessagesWaiting(queue);
    if (used > *peak) {
        *peak = used;
    }
}

static esp_err_t cloud_queue_send_ptr(QueueHandle_t queue,
                                      void *item_ptr,
                                      int timeout_ms)
{
    if (queue == NULL || item_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(queue, &item_ptr, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t cloud_frame_stream_append(cloud_frame_stream_state_t *state,
                                           const uint8_t *data,
                                           size_t data_len)
{
    if (state == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len == 0) {
        return ESP_OK;
    }
    if (state->pending_len + data_len > state->pending_cap) {
        size_t new_cap = state->pending_cap == 0 ? 4096 : state->pending_cap;
        while (state->pending_len + data_len > new_cap) {
            new_cap *= 2;
        }
        uint8_t *new_data = realloc(state->pending, new_cap);
        if (new_data == NULL) {
            return ESP_ERR_NO_MEM;
        }
        state->pending = new_data;
        state->pending_cap = new_cap;
    }
    memcpy(state->pending + state->pending_len, data, data_len);
    state->pending_len += data_len;
    return ESP_OK;
}

static uint32_t cloud_read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static esp_err_t cloud_consume_framed_audio_packets(cloud_frame_stream_state_t *frame_state,
                                                    cloud_audio_format_t audio_format,
                                                    QueueHandle_t encoded_queue,
                                                    cloud_realtime_audio_metrics_t *metrics,
                                                    int64_t connect_start_us,
                                                    int64_t *last_packet_us,
                                                    bool *saw_first_packet)
{
    if (frame_state == NULL || encoded_queue == NULL || last_packet_us == NULL || saw_first_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (frame_state->pending_len - offset >= 8) {
        const uint8_t *header = frame_state->pending + offset;
        const uint32_t sequence = cloud_read_be32(header);
        const uint32_t payload_len = cloud_read_be32(header + 4);
        const size_t frame_len = (size_t)payload_len + 8;
        if (frame_state->pending_len - offset < frame_len) {
            break;
        }

        const uint8_t *payload = header + 8;
        const int64_t now_us = esp_timer_get_time();
        if (!(*saw_first_packet)) {
            *saw_first_packet = true;
            if (metrics != NULL) {
                metrics->first_chunk_elapsed_us = now_us - connect_start_us;
                metrics->first_chunk_bytes = payload_len;
            }
        } else if (metrics != NULL && *last_packet_us > 0) {
            const int64_t gap_us = now_us - *last_packet_us;
            metrics->total_inter_chunk_gap_us += gap_us;
            if (gap_us > metrics->max_inter_chunk_gap_us) {
                metrics->max_inter_chunk_gap_us = gap_us;
            }
        }
        *last_packet_us = now_us;

        if (frame_state->expected_seq_valid && sequence != frame_state->expected_seq) {
            if (metrics != NULL) {
                metrics->seq_gap_count++;
            }
            ESP_LOGW(TAG,
                     "Realtime audio packet sequence gap expected=%u actual=%u",
                     (unsigned)frame_state->expected_seq,
                     (unsigned)sequence);
        }
        frame_state->expected_seq = sequence + 1;
        frame_state->expected_seq_valid = true;

        if (metrics != NULL) {
            metrics->packet_count++;
            metrics->chunk_count++;
            metrics->total_audio_bytes += payload_len;
            metrics->last_chunk_bytes = payload_len;
        }

        cloud_encoded_packet_t *packet = cloud_encoded_packet_alloc(CLOUD_STREAM_PACKET);
        if (packet == NULL) {
            return ESP_ERR_NO_MEM;
        }
        packet->audio_format = audio_format;
        packet->sequence = sequence;
        packet->payload = malloc(payload_len);
        if (packet->payload == NULL) {
            cloud_encoded_packet_free(packet);
            return ESP_ERR_NO_MEM;
        }
        memcpy(packet->payload, payload, payload_len);
        packet->payload_len = payload_len;

        esp_err_t ret = cloud_queue_send_ptr(encoded_queue,
                                             packet,
                                             DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS);
        if (ret != ESP_OK) {
            if (metrics != NULL) {
                metrics->receive_queue_full_count++;
            }
            cloud_encoded_packet_free(packet);
            return ret;
        }
        if (metrics != NULL) {
            cloud_metrics_update_queue_peak(&metrics->receive_queue_peak, encoded_queue);
        }
        offset += frame_len;
    }

    if (offset > 0) {
        memmove(frame_state->pending, frame_state->pending + offset, frame_state->pending_len - offset);
        frame_state->pending_len -= offset;
    }
    return ESP_OK;
}

static void cloud_opus_decoder_cleanup(cloud_opus_decoder_state_t *state)
{
    if (state == NULL) {
        return;
    }
    if (state->decoder != NULL) {
        esp_opus_dec_close(state->decoder);
        state->decoder = NULL;
    }
    free(state->pending);
    state->pending = NULL;
    state->pending_len = 0;
    state->pending_cap = 0;
    free(state->pcm_buffer);
    state->pcm_buffer = NULL;
    state->pcm_buffer_size = 0;
}

static esp_err_t cloud_opus_decoder_init(cloud_opus_decoder_state_t *state,
                                         uint32_t sample_rate,
                                         uint16_t channels,
                                         uint16_t frame_duration_ms)
{
    if (state == NULL || sample_rate == 0 || channels != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = sample_rate;
    dec_cfg.channel = channels;
    dec_cfg.self_delimited = false;
    switch (frame_duration_ms) {
    case 10:
        dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        break;
    case 20:
        dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        break;
    case 40:
        dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        break;
    case 60:
        dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &state->decoder);
    if (ret != ESP_AUDIO_ERR_OK || state->decoder == NULL) {
        ESP_LOGE(TAG, "Failed to open Opus decoder: %s", esp_err_to_name(ret));
        cloud_opus_decoder_cleanup(state);
        return ESP_FAIL;
    }

    const size_t frame_samples = (sample_rate * frame_duration_ms) / 1000U;
    state->pcm_buffer_size = frame_samples * channels * sizeof(int16_t);
    state->pcm_buffer = calloc(1, state->pcm_buffer_size);
    if (state->pcm_buffer == NULL) {
        cloud_opus_decoder_cleanup(state);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t cloud_opus_pending_append(cloud_opus_decoder_state_t *state,
                                           const uint8_t *data,
                                           size_t data_len)
{
    if (state == NULL || data == NULL || data_len == 0) {
        return data_len == 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (state->pending_len + data_len > state->pending_cap) {
        size_t new_cap = state->pending_cap == 0 ? 4096 : state->pending_cap;
        while (new_cap < state->pending_len + data_len) {
            new_cap *= 2;
        }
        uint8_t *new_buf = realloc(state->pending, new_cap);
        if (new_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        state->pending = new_buf;
        state->pending_cap = new_cap;
    }
    memcpy(state->pending + state->pending_len, data, data_len);
    state->pending_len += data_len;
    return ESP_OK;
}

static esp_err_t cloud_decode_opus_pending_packets(cloud_opus_decoder_state_t *state,
                                                   cloud_realtime_audio_chunk_callback_t callback,
                                                   void *user_ctx)
{
    if (state == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    while (state->pending_len >= 2) {
        const size_t packet_len =
            ((size_t)state->pending[0] << 8) | (size_t)state->pending[1];
        if (packet_len == 0) {
            return ESP_FAIL;
        }
        if (state->pending_len < packet_len + 2) {
            return ESP_OK;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer = state->pending + 2,
            .len = (uint32_t)packet_len,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out_frame = {
            .buffer = state->pcm_buffer,
            .len = (uint32_t)state->pcm_buffer_size,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dec_info = {0};
        esp_err_t ret = esp_opus_dec_decode(state->decoder, &raw, &out_frame, &dec_info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Opus decode failed: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
        if (out_frame.decoded_size > 0) {
            ret = callback(state->pcm_buffer, out_frame.decoded_size, user_ctx);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        memmove(state->pending, state->pending + packet_len + 2, state->pending_len - packet_len - 2);
        state->pending_len -= packet_len + 2;
    }
    return ESP_OK;
}

typedef struct {
    cloud_stream_runtime_t *runtime;
    uint32_t sequence;
    int64_t decode_elapsed_us;
} cloud_pcm_enqueue_ctx_t;

static esp_err_t cloud_enqueue_pcm_callback(const uint8_t *chunk,
                                            size_t chunk_bytes,
                                            void *user_ctx)
{
    if (chunk == NULL || chunk_bytes == 0 || user_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_pcm_enqueue_ctx_t *ctx = (cloud_pcm_enqueue_ctx_t *)user_ctx;
    cloud_stream_runtime_t *runtime = ctx->runtime;
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_pcm_packet_t *packet = cloud_pcm_packet_alloc(CLOUD_STREAM_PACKET);
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }
    packet->payload = malloc(chunk_bytes);
    if (packet->payload == NULL) {
        cloud_pcm_packet_free(packet);
        return ESP_ERR_NO_MEM;
    }
    memcpy(packet->payload, chunk, chunk_bytes);
    packet->payload_len = chunk_bytes;
    packet->sequence = ctx->sequence;
    packet->decode_elapsed_us = ctx->decode_elapsed_us;
    packet->duration_ms = (uint32_t)((chunk_bytes * 1000U) /
                                     (DEMO_AUDIO_SAMPLE_RATE * DEMO_AUDIO_CHANNELS * DEMO_AUDIO_BYTES_PER_SAMPLE));
    esp_err_t ret = cloud_queue_send_ptr(runtime->pcm_queue,
                                         packet,
                                         DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        if (runtime->metrics != NULL) {
            runtime->metrics->decode_queue_full_count++;
        }
        cloud_pcm_packet_free(packet);
        return ret;
    }
    if (runtime->metrics != NULL) {
        cloud_metrics_update_queue_peak(&runtime->metrics->decode_queue_peak, runtime->pcm_queue);
    }
    return ESP_OK;
}

static esp_err_t cloud_forward_terminal_packet(QueueHandle_t queue,
                                               cloud_stream_packet_type_t type,
                                               esp_err_t error)
{
    cloud_pcm_packet_t *packet = cloud_pcm_packet_alloc(type);
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }
    packet->error = error;
    esp_err_t ret = cloud_queue_send_ptr(queue, packet, DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        cloud_pcm_packet_free(packet);
    }
    return ret;
}

static void cloud_decode_task(void *arg)
{
    cloud_stream_runtime_t *runtime = (cloud_stream_runtime_t *)arg;
    if (runtime == NULL || runtime->encoded_queue == NULL || runtime->pcm_queue == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        cloud_encoded_packet_t *packet = NULL;
        if (xQueueReceive(runtime->encoded_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (packet == NULL) {
            continue;
        }

        if (packet->type == CLOUD_STREAM_EOF) {
            runtime->decode_eof_us = esp_timer_get_time();
            runtime->decode_result = cloud_forward_terminal_packet(runtime->pcm_queue, CLOUD_STREAM_EOF, ESP_OK);
            cloud_encoded_packet_free(packet);
            break;
        }
        if (packet->type == CLOUD_STREAM_ERROR) {
            runtime->decode_result = cloud_forward_terminal_packet(runtime->pcm_queue,
                                                                   CLOUD_STREAM_ERROR,
                                                                   packet->error != ESP_OK ? packet->error : ESP_FAIL);
            cloud_encoded_packet_free(packet);
            break;
        }

        const int64_t decode_start_us = esp_timer_get_time();
        esp_err_t ret = ESP_OK;
        if (packet->audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
            cloud_pcm_enqueue_ctx_t ctx = {
                .runtime = runtime,
                .sequence = packet->sequence,
                .decode_elapsed_us = 0,
            };
            ret = cloud_opus_pending_append(&runtime->opus_decoder, packet->payload, packet->payload_len);
            if (ret == ESP_OK) {
                ctx.decode_elapsed_us = esp_timer_get_time() - decode_start_us;
                ret = cloud_decode_opus_pending_packets(&runtime->opus_decoder,
                                                        cloud_enqueue_pcm_callback,
                                                        &ctx);
            }
        } else {
            cloud_pcm_packet_t *pcm_packet = cloud_pcm_packet_alloc(CLOUD_STREAM_PACKET);
            if (pcm_packet == NULL) {
                ret = ESP_ERR_NO_MEM;
            } else {
                pcm_packet->payload = packet->payload;
                pcm_packet->payload_len = packet->payload_len;
                pcm_packet->sequence = packet->sequence;
                pcm_packet->duration_ms = (uint32_t)((packet->payload_len * 1000U) /
                                                     (DEMO_AUDIO_SAMPLE_RATE * DEMO_AUDIO_CHANNELS * DEMO_AUDIO_BYTES_PER_SAMPLE));
                pcm_packet->decode_elapsed_us = esp_timer_get_time() - decode_start_us;
                packet->payload = NULL;
                packet->payload_len = 0;
                ret = cloud_queue_send_ptr(runtime->pcm_queue,
                                           pcm_packet,
                                           DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS);
                if (ret != ESP_OK) {
                    if (runtime->metrics != NULL) {
                        runtime->metrics->decode_queue_full_count++;
                    }
                    cloud_pcm_packet_free(pcm_packet);
                } else if (runtime->metrics != NULL) {
                    cloud_metrics_update_queue_peak(&runtime->metrics->decode_queue_peak, runtime->pcm_queue);
                }
            }
        }

        const int64_t decode_elapsed_us = esp_timer_get_time() - decode_start_us;
        if (runtime->metrics != NULL) {
            runtime->metrics->decode_packet_count++;
            runtime->metrics->decode_total_us += decode_elapsed_us;
            if (decode_elapsed_us > runtime->metrics->decode_max_us) {
                runtime->metrics->decode_max_us = decode_elapsed_us;
            }
        }
        if (ret != ESP_OK) {
            if (runtime->metrics != NULL) {
                runtime->metrics->decode_fail_count++;
            }
            runtime->decode_result = cloud_forward_terminal_packet(runtime->pcm_queue, CLOUD_STREAM_ERROR, ret);
            cloud_encoded_packet_free(packet);
            break;
        }
        cloud_encoded_packet_free(packet);
    }

    runtime->decode_done = true;
    runtime->decode_task = NULL;
    vTaskDelete(NULL);
}

static void cloud_playback_task(void *arg)
{
    cloud_stream_runtime_t *runtime = (cloud_stream_runtime_t *)arg;
    if (runtime == NULL || runtime->pcm_queue == NULL || runtime->callback == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        cloud_pcm_packet_t *packet = NULL;
        if (xQueueReceive(runtime->pcm_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (packet == NULL) {
            continue;
        }

        if (packet->type == CLOUD_STREAM_EOF) {
            if (runtime->metrics != NULL && runtime->decode_eof_us > 0) {
                runtime->metrics->pcm_queue_drain_us = esp_timer_get_time() - runtime->decode_eof_us;
            }
            cloud_pcm_packet_free(packet);
            runtime->playback_result = ESP_OK;
            break;
        }
        if (packet->type == CLOUD_STREAM_ERROR) {
            runtime->playback_result = packet->error != ESP_OK ? packet->error : ESP_FAIL;
            cloud_pcm_packet_free(packet);
            break;
        }

        esp_err_t ret = runtime->callback(packet->payload, packet->payload_len, runtime->user_ctx);
        if (ret != ESP_OK) {
            runtime->playback_result = ret;
            cloud_pcm_packet_free(packet);
            break;
        }
        cloud_pcm_packet_free(packet);
    }

    runtime->playback_done = true;
    runtime->playback_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t cloud_client_submit_pcm_task(const uint8_t *pcm,
                                       size_t pcm_bytes,
                                       char *task_id,
                                       size_t task_id_size)
{
    if (pcm == NULL || pcm_bytes == 0 || task_id == NULL || task_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (DEMO_SERVER_BASE_URL[0] == '\0') {
        ESP_LOGE(TAG, "DEMO_SERVER_BASE_URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    esp_err_t ret = cloud_build_url(url, sizeof(url), "api/v2/tasks");
    if (ret != ESP_OK) {
        return ret;
    }

    cloud_response_buffer_t response = {0};
    ret = cloud_response_buffer_init(&response, 2048);
    if (ret != ESP_OK) {
        return ret;
    }

    char sample_rate[16];
    char sample_width[16];
    char channels[16];
    snprintf(sample_rate, sizeof(sample_rate), "%d", DEMO_AUDIO_SAMPLE_RATE);
    snprintf(sample_width, sizeof(sample_width), "%d", DEMO_AUDIO_BITS_PER_SAMPLE);
    snprintf(channels, sizeof(channels), "%d", DEMO_AUDIO_CHANNELS);

    cloud_http_ctx_t ctx = {
        .buffer = &response,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DEMO_CLOUD_SUBMIT_TIMEOUT_MS,
        .event_handler = cloud_http_event_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cloud_response_buffer_free(&response);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "content-type", "application/octet-stream");
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "x-device-id", DEMO_DEVICE_ID);
    esp_http_client_set_header(client, "x-sample-rate", sample_rate);
    esp_http_client_set_header(client, "x-sample-width", sample_width);
    esp_http_client_set_header(client, "x-channels", channels);

    ESP_LOGI(TAG,
             "Submitting PCM: bytes=%u device_id=%s rate=%s width=%s channels=%s url=%s",
             (unsigned)pcm_bytes,
             DEMO_DEVICE_ID,
             sample_rate,
             sample_width,
             channels,
             url);

    ret = esp_http_client_set_post_field(client, (const char *)pcm, (int)pcm_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set submit body: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ret;
    }

    const int64_t submit_start_us = esp_timer_get_time();
    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Submit request failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ret;
    }
    const int64_t submit_elapsed_us = esp_timer_get_time() - submit_start_us;

    const int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Submit returned HTTP %d: %s", status_code,
                 response.data != NULL ? response.data : "");
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ESP_FAIL;
    }

    ret = cloud_json_parse_task_id(response.data, task_id, task_id_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse task_id from submit response: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG,
                 "Submit accepted: task_id=%s http_status=%d response_bytes=%u elapsed_ms=%.1f",
                 task_id,
                 status_code,
                 (unsigned)response.len,
                 (double)submit_elapsed_us / 1000.0);
    }

    esp_http_client_cleanup(client);
    cloud_response_buffer_free(&response);
    return ret;
}

esp_err_t cloud_client_submit_realtime_session(const uint8_t *pcm,
                                               size_t pcm_bytes,
                                               cloud_realtime_session_t *session)
{
    if (pcm == NULL || pcm_bytes == 0 || session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    esp_err_t ret = cloud_build_url(url, sizeof(url), "api/v3/realtime/sessions");
    if (ret != ESP_OK) {
        return ret;
    }

    cloud_response_buffer_t response = {0};
    ret = cloud_response_buffer_init(&response, 2048);
    if (ret != ESP_OK) {
        return ret;
    }

    char sample_rate[16];
    char sample_width[16];
    char channels[16];
    snprintf(sample_rate, sizeof(sample_rate), "%d", DEMO_AUDIO_SAMPLE_RATE);
    snprintf(sample_width, sizeof(sample_width), "%d", DEMO_AUDIO_BITS_PER_SAMPLE);
    snprintf(channels, sizeof(channels), "%d", DEMO_AUDIO_CHANNELS);

    cloud_http_ctx_t ctx = {
        .buffer = &response,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DEMO_REALTIME_SESSION_TIMEOUT_MS,
        .event_handler = cloud_http_event_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        cloud_response_buffer_free(&response);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "content-type", "application/octet-stream");
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "x-device-id", DEMO_DEVICE_ID);
    esp_http_client_set_header(client, "x-sample-rate", sample_rate);
    esp_http_client_set_header(client, "x-sample-width", sample_width);
    esp_http_client_set_header(client, "x-channels", channels);

    ret = esp_http_client_set_post_field(client, (const char *)pcm, (int)pcm_bytes);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ret;
    }

    const int64_t start_us = esp_timer_get_time();
    ret = esp_http_client_perform(client);
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Realtime session create failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ret;
    }

    const int status_code = esp_http_client_get_status_code(client);
    if (status_code != 202) {
        ESP_LOGE(TAG, "Realtime session HTTP %d: %s", status_code, response.data != NULL ? response.data : "");
        esp_http_client_cleanup(client);
        cloud_response_buffer_free(&response);
        return ESP_FAIL;
    }

    ret = cloud_json_parse_realtime_session(response.data != NULL ? response.data : "", session);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "Realtime session accepted: session_id=%s elapsed_ms=%.1f",
                 session->session_id,
                 (double)elapsed_us / 1000.0);
    } else {
        ESP_LOGE(TAG, "Failed to parse realtime session response: %s", esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    cloud_response_buffer_free(&response);
    return ret;
}

esp_err_t cloud_client_poll_task(const char *task_id,
                                 cloud_task_result_t *result,
                                 int timeout_ms,
                                 int poll_interval_ms)
{
    if (task_id == NULL || task_id[0] == '\0' || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (DEMO_SERVER_BASE_URL[0] == '\0') {
        ESP_LOGE(TAG, "DEMO_SERVER_BASE_URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms <= 0) {
        timeout_ms = DEMO_CLOUD_POLL_TIMEOUT_MS;
    }
    if (poll_interval_ms <= 0) {
        poll_interval_ms = DEMO_CLOUD_POLL_INTERVAL_MS;
    }

    memset(result, 0, sizeof(*result));

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t poll_ticks = pdMS_TO_TICKS(poll_interval_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    char url[320];
    char path[256];
    char encoded_task_id[(DEMO_CLOUD_TASK_ID_MAX_LEN * 3) + 1];
    esp_err_t ret = cloud_url_encode_path_segment(task_id, encoded_task_id, sizeof(encoded_task_id));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to encode task_id for poll URL: %s", esp_err_to_name(ret));
        return ret;
    }

    int written = snprintf(path, sizeof(path), "api/v2/tasks/%s", encoded_task_id);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_NO_MEM;
    }

    int poll_attempt = 0;
    esp_err_t last_ret = ESP_OK;
    while ((xTaskGetTickCount() - start_tick) <= timeout_ticks) {
        poll_attempt++;
        cloud_response_buffer_t response = {0};
        last_ret = cloud_response_buffer_init(&response, 2048);
        if (last_ret != ESP_OK) {
            return last_ret;
        }

        last_ret = cloud_build_url(url, sizeof(url), path);
        if (last_ret != ESP_OK) {
            cloud_response_buffer_free(&response);
            return last_ret;
        }

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = DEMO_CLOUD_POLL_REQUEST_TIMEOUT_MS,
        };

        int status_code = 0;
        const int64_t poll_start_us = esp_timer_get_time();
        last_ret = cloud_http_execute(&config, NULL, 0, &response, &status_code);
        const int64_t poll_elapsed_us = esp_timer_get_time() - poll_start_us;
        if (last_ret != ESP_OK) {
            if (cloud_is_retryable_poll_error(last_ret)) {
                ESP_LOGW(TAG,
                         "Poll attempt=%d task_id=%s transient error=%s elapsed_ms=%.1f, retrying",
                         poll_attempt,
                         task_id,
                         esp_err_to_name(last_ret),
                         (double)poll_elapsed_us / 1000.0);
                cloud_response_buffer_free(&response);
                vTaskDelay(poll_ticks);
                continue;
            }
            cloud_response_buffer_free(&response);
            return last_ret;
        }

        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "Poll returned HTTP %d: %s", status_code,
                     response.data != NULL ? response.data : "");
            cloud_response_buffer_free(&response);
            return ESP_FAIL;
        }

        last_ret = cloud_json_parse_task_result(response.data != NULL ? response.data : "", result);
        cloud_response_buffer_free(&response);
        if (last_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to parse poll response: %s", esp_err_to_name(last_ret));
            return last_ret;
        }

        ESP_LOGI(TAG,
                 "Poll attempt=%d task_id=%s status=%s http_status=%d elapsed_ms=%.1f",
                 poll_attempt,
                 task_id,
                 result->status[0] != '\0' ? result->status : "(empty)",
                 status_code,
                 (double)poll_elapsed_us / 1000.0);

        if (strcmp(result->status, "done") == 0 || strcmp(result->status, "failed") == 0) {
            ESP_LOGI(TAG,
                     "Task %s finished with status=%s question_text=%s error_code=%s",
                     task_id,
                     result->status,
                     result->question_text[0] != '\0' ? result->question_text : "(empty)",
                     result->error_code[0] != '\0' ? result->error_code : "(empty)");
            return ESP_OK;
        }

        vTaskDelay(poll_ticks);
    }

    ESP_LOGE(TAG, "Timed out waiting for task %s", task_id);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cloud_client_stream_realtime_audio(const char *audio_stream_url,
                                             cloud_realtime_audio_chunk_callback_t callback,
                                             void *user_ctx,
                                             cloud_realtime_audio_metrics_t *metrics)
{
    if (audio_stream_url == NULL || audio_stream_url[0] == '\0' || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (metrics != NULL) {
        memset(metrics, 0, sizeof(*metrics));
    }

    cloud_audio_headers_t *audio_headers = calloc(1, sizeof(*audio_headers));
    if (audio_headers == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t config = {
        .url = audio_stream_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DEMO_REALTIME_AUDIO_OPEN_TIMEOUT_MS,
        .event_handler = cloud_realtime_audio_event_handler,
        .user_data = audio_headers,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(audio_headers);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "X-Accept-Audio-Format", DEMO_REALTIME_AUDIO_ACCEPT_FORMATS);
    ESP_LOGI(TAG,
             "Opening realtime audio stream url=%s accept_audio_format=%s",
             audio_stream_url,
             DEMO_REALTIME_AUDIO_ACCEPT_FORMATS);

    const int64_t connect_start_us = esp_timer_get_time();
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        free(audio_headers);
        return ret;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (metrics != NULL) {
        metrics->http_status = status_code;
        metrics->connect_elapsed_us = esp_timer_get_time() - connect_start_us;
    }

    if (content_length < 0) {
        ESP_LOGI(TAG, "Realtime audio stream uses chunked/unknown content length");
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Realtime audio returned HTTP %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(audio_headers);
        return ESP_FAIL;
    }

    ret = cloud_validate_audio_header_exact(audio_headers, client, "Content-Type", "application/octet-stream");
    cloud_audio_format_t audio_format = CLOUD_AUDIO_FORMAT_PCM;
    cloud_frame_stream_state_t *frame_state = calloc(1, sizeof(*frame_state));
    uint8_t *chunk = malloc(DEMO_REALTIME_AUDIO_HTTP_READ_CHUNK_BYTES);
    cloud_stream_runtime_t runtime = {0};
    runtime.callback = callback;
    runtime.user_ctx = user_ctx;
    runtime.metrics = metrics;
    runtime.audio_format = CLOUD_AUDIO_FORMAT_PCM;
    runtime.decode_result = ESP_OK;
    runtime.playback_result = ESP_OK;
    runtime.encoded_queue = xQueueCreate(DEMO_REALTIME_AUDIO_ENCODED_QUEUE_LENGTH, sizeof(cloud_encoded_packet_t *));
    runtime.pcm_queue = xQueueCreate(DEMO_REALTIME_AUDIO_PCM_QUEUE_LENGTH, sizeof(cloud_pcm_packet_t *));
    if (frame_state == NULL || chunk == NULL || runtime.encoded_queue == NULL || runtime.pcm_queue == NULL) {
        ret = ESP_ERR_NO_MEM;
    }
    const bool framed_packetization = cloud_audio_packetization_is_framed_v1(audio_headers, client);
    if (ret == ESP_OK) {
        audio_format = cloud_parse_audio_format(audio_headers, client, &ret);
        runtime.audio_format = audio_format;
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_PCM) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Audio-Sample-Rate", "16000");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_PCM) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Audio-Sample-Width", "16");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_PCM) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Audio-Channels", "1");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_PCM) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Audio-Endian", "little");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Opus-Sample-Rate", "16000");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Opus-Channels", "1");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
        ret = cloud_validate_audio_header_exact(audio_headers, client, "X-Opus-Frame-Duration-Ms", "60");
    }
    if (ret == ESP_OK && audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
        ret = cloud_opus_decoder_init(&runtime.opus_decoder, 16000, 1, 60);
    }
    if (ret != ESP_OK) {
        cloud_opus_decoder_cleanup(&runtime.opus_decoder);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk);
        free(frame_state);
        if (runtime.encoded_queue != NULL) {
            vQueueDelete(runtime.encoded_queue);
        }
        if (runtime.pcm_queue != NULL) {
            vQueueDelete(runtime.pcm_queue);
        }
        free(audio_headers);
        return ret;
    }
    if (metrics != NULL) {
        metrics->headers_validated = true;
        snprintf(metrics->audio_format, sizeof(metrics->audio_format), "%s",
                 audio_format == CLOUD_AUDIO_FORMAT_OPUS ? "opus" : "pcm");
        snprintf(metrics->audio_packetization,
                 sizeof(metrics->audio_packetization),
                 "%s",
                 framed_packetization ? "framed-v1" : "legacy");
    }

    if (xTaskCreate(cloud_decode_task,
                    "cloud_decode",
                    DEMO_REALTIME_AUDIO_DECODE_TASK_STACK_SIZE,
                    &runtime,
                    DEMO_PIPELINE_TASK_PRIORITY + 1,
                    &runtime.decode_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK &&
        xTaskCreate(cloud_playback_task,
                    "cloud_playback",
                    DEMO_REALTIME_AUDIO_PLAYBACK_TASK_STACK_SIZE,
                    &runtime,
                    DEMO_PIPELINE_TASK_PRIORITY,
                    &runtime.playback_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK) {
        cloud_opus_decoder_cleanup(&runtime.opus_decoder);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(chunk);
        free(frame_state);
        if (runtime.encoded_queue != NULL) {
            vQueueDelete(runtime.encoded_queue);
        }
        if (runtime.pcm_queue != NULL) {
            vQueueDelete(runtime.pcm_queue);
        }
        free(audio_headers);
        return ret;
    }

    bool saw_first_chunk = false;
    int64_t last_chunk_us = 0;
    esp_err_t stream_result = ESP_OK;
    while (true) {
        int read_len = esp_http_client_read(client, (char *)chunk, DEMO_REALTIME_AUDIO_HTTP_READ_CHUNK_BYTES);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Realtime audio read failed: %d", read_len);
            stream_result = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            continue;
        }

        if (framed_packetization) {
            ret = cloud_frame_stream_append(frame_state, chunk, (size_t)read_len);
            if (ret == ESP_OK) {
                ret = cloud_consume_framed_audio_packets(frame_state,
                                                        audio_format,
                                                        runtime.encoded_queue,
                                                        metrics,
                                                        connect_start_us,
                                                        &last_chunk_us,
                                                        &saw_first_chunk);
            }
        } else {
            const int64_t now_us = esp_timer_get_time();
            if (!saw_first_chunk) {
                saw_first_chunk = true;
                if (metrics != NULL) {
                    metrics->first_chunk_elapsed_us = now_us - connect_start_us;
                    metrics->first_chunk_bytes = (size_t)read_len;
                }
            } else if (metrics != NULL && last_chunk_us > 0) {
                const int64_t gap_us = now_us - last_chunk_us;
                metrics->total_inter_chunk_gap_us += gap_us;
                if (gap_us > metrics->max_inter_chunk_gap_us) {
                    metrics->max_inter_chunk_gap_us = gap_us;
                }
            }
            last_chunk_us = now_us;
            if (metrics != NULL) {
                metrics->chunk_count++;
                metrics->packet_count++;
                metrics->total_audio_bytes += (size_t)read_len;
                metrics->last_chunk_bytes = (size_t)read_len;
            }
            cloud_encoded_packet_t *packet = cloud_encoded_packet_alloc(CLOUD_STREAM_PACKET);
            if (packet == NULL) {
                ret = ESP_ERR_NO_MEM;
            } else {
                packet->audio_format = audio_format;
                packet->sequence = metrics != NULL ? (uint32_t)metrics->packet_count : 0;
                packet->payload = malloc((size_t)read_len);
                if (packet->payload == NULL) {
                    cloud_encoded_packet_free(packet);
                    ret = ESP_ERR_NO_MEM;
                } else {
                    memcpy(packet->payload, chunk, (size_t)read_len);
                    packet->payload_len = (size_t)read_len;
                    ret = cloud_queue_send_ptr(runtime.encoded_queue,
                                               packet,
                                               DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS);
                    if (ret == ESP_OK && metrics != NULL) {
                        cloud_metrics_update_queue_peak(&metrics->receive_queue_peak, runtime.encoded_queue);
                    }
                    if (ret != ESP_OK) {
                        if (metrics != NULL) {
                            metrics->receive_queue_full_count++;
                        }
                        cloud_encoded_packet_free(packet);
                    }
                }
            }
        }
        if (ret != ESP_OK) {
            stream_result = ret;
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (framed_packetization && frame_state->pending_len != 0 && stream_result == ESP_OK) {
        stream_result = ESP_FAIL;
    }

    cloud_encoded_packet_t *terminal_packet =
        cloud_encoded_packet_alloc(stream_result == ESP_OK ? CLOUD_STREAM_EOF : CLOUD_STREAM_ERROR);
    if (terminal_packet != NULL) {
        terminal_packet->error = stream_result;
        if (cloud_queue_send_ptr(runtime.encoded_queue,
                                 terminal_packet,
                                 DEMO_REALTIME_AUDIO_QUEUE_SEND_TIMEOUT_MS) != ESP_OK) {
            cloud_encoded_packet_free(terminal_packet);
            stream_result = ESP_FAIL;
        }
    } else {
        stream_result = ESP_ERR_NO_MEM;
    }

    const int64_t join_start_us = esp_timer_get_time();
    while ((!runtime.decode_done || !runtime.playback_done) &&
           (esp_timer_get_time() - join_start_us) <
               ((int64_t)DEMO_REALTIME_AUDIO_TASK_JOIN_TIMEOUT_MS * 1000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!runtime.decode_done || !runtime.playback_done) {
        ESP_LOGE(TAG,
                 "Realtime audio join timeout decode_done=%d playback_done=%d decode_result=%s playback_result=%s",
                 runtime.decode_done ? 1 : 0,
                 runtime.playback_done ? 1 : 0,
                 esp_err_to_name(runtime.decode_result),
                 esp_err_to_name(runtime.playback_result));
        stream_result = ESP_ERR_TIMEOUT;
        if (runtime.decode_task != NULL && !runtime.decode_done) {
            vTaskDelete(runtime.decode_task);
        }
        if (runtime.playback_task != NULL && !runtime.playback_done) {
            vTaskDelete(runtime.playback_task);
        }
    }

    cloud_frame_stream_cleanup(frame_state);
    cloud_opus_decoder_cleanup(&runtime.opus_decoder);
    if (runtime.encoded_queue != NULL) {
        cloud_encoded_packet_t *pending_packet = NULL;
        while (xQueueReceive(runtime.encoded_queue, &pending_packet, 0) == pdTRUE) {
            cloud_encoded_packet_free(pending_packet);
        }
        vQueueDelete(runtime.encoded_queue);
    }
    if (runtime.pcm_queue != NULL) {
        cloud_pcm_packet_t *pending_packet = NULL;
        while (xQueueReceive(runtime.pcm_queue, &pending_packet, 0) == pdTRUE) {
            cloud_pcm_packet_free(pending_packet);
        }
        vQueueDelete(runtime.pcm_queue);
    }
    if (audio_format == CLOUD_AUDIO_FORMAT_OPUS) {
        if (runtime.opus_decoder.pending_len != 0 && stream_result == ESP_OK) {
            stream_result = ESP_FAIL;
        }
    }
    free(chunk);
    free(frame_state);
    free(audio_headers);

    if (!saw_first_chunk) {
        return DEMO_CLOUD_ERR_AUDIO_STREAM_EARLY_EOF;
    }
    if (stream_result != ESP_OK) {
        return stream_result;
    }
    if (runtime.decode_result != ESP_OK) {
        return runtime.decode_result;
    }
    if (runtime.playback_result != ESP_OK) {
        return runtime.playback_result;
    }
    return ESP_OK;
}
