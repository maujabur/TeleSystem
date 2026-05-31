#include "wav_writer.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "wav-writer";

struct wav_writer {
    FILE *file;
    uint32_t data_bytes;
    wav_writer_config_t config;
};

static void write_u16_le(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t)(value & 0xff);
    dest[1] = (uint8_t)((value >> 8) & 0xff);
}

static void write_u32_le(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t)(value & 0xff);
    dest[1] = (uint8_t)((value >> 8) & 0xff);
    dest[2] = (uint8_t)((value >> 16) & 0xff);
    dest[3] = (uint8_t)((value >> 24) & 0xff);
}

void wav_writer_build_header(uint8_t header[WAV_WRITER_HEADER_SIZE],
                             const wav_writer_config_t *config,
                             uint32_t data_bytes)
{
    uint16_t audio_format = 1;
    uint16_t channels = (uint16_t)config->channels;
    uint16_t bits_per_sample = (uint16_t)config->bits_per_sample;
    uint32_t sample_rate = (uint32_t)config->sample_rate_hz;
    uint16_t block_align = (uint16_t)(channels * bits_per_sample / 8);
    uint32_t byte_rate = sample_rate * block_align;

    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    write_u32_le(&header[4], 36 + data_bytes);
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    write_u32_le(&header[16], 16);
    write_u16_le(&header[20], audio_format);
    write_u16_le(&header[22], channels);
    write_u32_le(&header[24], sample_rate);
    write_u32_le(&header[28], byte_rate);
    write_u16_le(&header[32], block_align);
    write_u16_le(&header[34], bits_per_sample);
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    write_u32_le(&header[40], data_bytes);
}

esp_err_t wav_writer_open(wav_writer_t **writer,
                          const char *path,
                          const wav_writer_config_t *config)
{
    ESP_RETURN_ON_FALSE(writer != NULL && path != NULL && config != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "argumento invalido");
    ESP_RETURN_ON_FALSE(config->sample_rate_hz > 0 &&
                            config->channels > 0 &&
                            config->bits_per_sample > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "configuracao WAV invalida");

    wav_writer_t *new_writer = calloc(1, sizeof(*new_writer));
    ESP_RETURN_ON_FALSE(new_writer != NULL, ESP_ERR_NO_MEM, TAG, "sem memoria");

    new_writer->file = fopen(path, "wb+");
    if (!new_writer->file) {
        free(new_writer);
        ESP_LOGE(TAG, "nao foi possivel abrir %s", path);
        return ESP_FAIL;
    }

    new_writer->config = *config;

    uint8_t header[WAV_WRITER_HEADER_SIZE] = {0};
    wav_writer_build_header(header, config, 0);
    if (fwrite(header, 1, sizeof(header), new_writer->file) != sizeof(header)) {
        fclose(new_writer->file);
        free(new_writer);
        ESP_LOGE(TAG, "falha ao escrever header WAV");
        return ESP_FAIL;
    }

    *writer = new_writer;
    return ESP_OK;
}

esp_err_t wav_writer_write_pcm(wav_writer_t *writer, const void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(writer != NULL && writer->file != NULL && data != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "argumento invalido");

    if (size == 0) {
        return ESP_OK;
    }

    if (fwrite(data, 1, size, writer->file) != size) {
        ESP_LOGE(TAG, "falha ao escrever PCM");
        return ESP_FAIL;
    }

    writer->data_bytes += (uint32_t)size;
    return ESP_OK;
}

esp_err_t wav_writer_close(wav_writer_t *writer)
{
    ESP_RETURN_ON_FALSE(writer != NULL, ESP_ERR_INVALID_ARG, TAG, "writer nulo");

    uint8_t header[WAV_WRITER_HEADER_SIZE] = {0};
    wav_writer_build_header(header, &writer->config, writer->data_bytes);

    esp_err_t err = ESP_OK;
    if (fseek(writer->file, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), writer->file) != sizeof(header) ||
        fflush(writer->file) != 0) {
        ESP_LOGE(TAG, "falha ao finalizar header WAV");
        err = ESP_FAIL;
    }

    if (fclose(writer->file) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "falha ao fechar arquivo WAV");
        err = ESP_FAIL;
    }

    ESP_LOGI(TAG, "WAV fechado com %" PRIu32 " bytes de PCM", writer->data_bytes);
    free(writer);
    return err;
}
