#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate_hz;
    int channels;
    int bits_per_sample;
} wav_writer_config_t;

typedef struct wav_writer wav_writer_t;

#define WAV_WRITER_HEADER_SIZE 44

void wav_writer_build_header(uint8_t header[WAV_WRITER_HEADER_SIZE],
                             const wav_writer_config_t *config,
                             uint32_t data_bytes);
esp_err_t wav_writer_open(wav_writer_t **writer,
                          const char *path,
                          const wav_writer_config_t *config);
esp_err_t wav_writer_write_pcm(wav_writer_t *writer, const void *data, size_t size);
esp_err_t wav_writer_close(wav_writer_t *writer);

#ifdef __cplusplus
}
#endif
