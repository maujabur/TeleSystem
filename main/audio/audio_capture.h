#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_CAPTURE_DEFAULT_PATH "/data/capture.wav"
#define AUDIO_CAPTURE_DEFAULT_SECONDS 8
#define AUDIO_CAPTURE_SAMPLE_RATE_HZ 16000
#define AUDIO_CAPTURE_BITS_PER_SAMPLE 16
#define AUDIO_CAPTURE_CHANNELS 1
#define AUDIO_CAPTURE_MIN_SECONDS 1
#define AUDIO_CAPTURE_MAX_SECONDS 30

typedef struct {
    size_t bytes_written;
    int duration_seconds;
    int gain_q8;
    int silence_threshold_rms;
    uint32_t active_ms;
    double rms;
    int16_t peak_sample;
    bool clipped;
} audio_capture_result_t;

typedef struct {
    uint8_t *pcm_data;
    size_t pcm_size;
    int sample_rate_hz;
    int channels;
    int bits_per_sample;
} audio_capture_buffer_t;

esp_err_t audio_capture_record_wav(const char *path,
                                   int duration_seconds,
                                   int gain_q8,
                                   int silence_threshold_rms,
                                   int silence_hysteresis_rms,
                                   audio_capture_result_t *result);
esp_err_t audio_capture_record_pcm_to_buffer(int duration_seconds,
                                             int gain_q8,
                                             int silence_threshold_rms,
                                             int silence_hysteresis_rms,
                                             audio_capture_result_t *result,
                                             audio_capture_buffer_t *buffer);
void audio_capture_buffer_free(audio_capture_buffer_t *buffer);

#ifdef __cplusplus
}
#endif
