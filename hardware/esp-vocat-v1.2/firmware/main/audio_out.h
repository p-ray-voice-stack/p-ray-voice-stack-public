#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

// Downloads a WAV file from the provided URL into heap memory.
// The caller owns the returned buffer and must free() it.
esp_err_t audio_out_download_wav_url(const char *audio_url,
                                     uint8_t **out_wav_bytes,
                                     size_t *out_wav_size);

// Parses a WAV buffer and streams the PCM payload to MAX98357A.
esp_err_t audio_out_play_wav_buffer(const uint8_t *wav_bytes,
                                    size_t wav_bytes_size);

// Convenience helper that downloads and plays the WAV in one step.
esp_err_t audio_out_play_wav_url(const char *audio_url);

esp_err_t audio_out_play_pcm_file(const char *path,
                                  uint32_t sample_rate,
                                  uint16_t channels,
                                  uint16_t bits_per_sample,
                                  size_t max_bytes);

esp_err_t audio_out_open_pcm_stream(uint32_t sample_rate,
                                    uint16_t channels,
                                    uint16_t bits_per_sample);

esp_err_t audio_out_write_pcm_chunk(const uint8_t *pcm_bytes,
                                    size_t pcm_bytes_size,
                                    size_t *written_bytes);

esp_err_t audio_out_write_pcm_chunk_buffered(const uint8_t *pcm_bytes,
                                             size_t pcm_bytes_size,
                                             size_t *written_bytes);

typedef struct {
    size_t total_in;
    size_t total_out;
    size_t min_level;
    size_t max_level;
    uint32_t underrun_count;
    int64_t underrun_us;
    int64_t first_input_us;
    int64_t playback_start_us;
    int64_t prebuffer_wait_us;
    bool playback_started;
} audio_out_jitter_metrics_t;

esp_err_t audio_out_close_pcm_stream(void);
esp_err_t audio_out_close_pcm_stream_with_metrics(audio_out_jitter_metrics_t *metrics);

void audio_out_deinit(void);
