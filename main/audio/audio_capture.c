#include "audio_capture.h"

#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "wav_writer.h"

static const char *TAG = "audio-capture";

#define AUDIO_CAPTURE_BUFFER_FRAMES 2048
#define AUDIO_CAPTURE_READ_TIMEOUT_MS 1000
#define AUDIO_CAPTURE_PREROLL_DISCARD_MS 50
#define AUDIO_CAPTURE_DIGITAL_GAIN_Q8 1024

#ifndef CONFIG_AUDIO_I2S_MCLK_GPIO
#define CONFIG_AUDIO_I2S_MCLK_GPIO 1
#endif

#ifndef CONFIG_AUDIO_I2S_DIN_GPIO
#define CONFIG_AUDIO_I2S_DIN_GPIO 2
#endif

#ifndef CONFIG_AUDIO_I2S_WS_GPIO
#define CONFIG_AUDIO_I2S_WS_GPIO 3
#endif

#ifndef CONFIG_AUDIO_I2S_BCLK_GPIO
#define CONFIG_AUDIO_I2S_BCLK_GPIO 4
#endif

static int normalize_gain_q8(int gain_q8)
{
    if (gain_q8 < 64) {
        return 64;
    }
    if (gain_q8 > 4096) {
        return 4096;
    }
    return gain_q8;
}

static int normalize_silence_hysteresis_rms(int hysteresis)
{
    if (hysteresis < 0) {
        return 0;
    }
    if (hysteresis > INT16_MAX) {
        return INT16_MAX;
    }
    return hysteresis;
}

static bool silence_gate_accepts_block(bool current_active,
                                       double block_rms,
                                       int silence_threshold_rms,
                                       int silence_hysteresis_rms)
{
    if (silence_threshold_rms <= 0) {
        return true;
    }

    if (!current_active) {
        return block_rms >= (double)silence_threshold_rms;
    }

    int release_threshold = silence_threshold_rms - silence_hysteresis_rms;
    if (release_threshold < 0) {
        release_threshold = 0;
    }
    return block_rms >= (double)release_threshold;
}

static int16_t apply_gain_with_saturation(int16_t sample, int gain_q8, bool *clipped)
{
    int32_t amplified = ((int32_t)sample * gain_q8) / 256;
    if (amplified > INT16_MAX) {
        *clipped = true;
        return INT16_MAX;
    }
    if (amplified < INT16_MIN) {
        *clipped = true;
        return INT16_MIN;
    }

    return (int16_t)amplified;
}

static int16_t abs_i16_for_peak(int16_t sample)
{
    return sample == INT16_MIN ? INT16_MAX : (sample < 0 ? -sample : sample);
}

static esp_err_t init_i2s(i2s_chan_handle_t *rx_handle)
{
    const int mclk_cfg_gpio = CONFIG_AUDIO_I2S_MCLK_GPIO;
    const gpio_num_t mclk_gpio =
        mclk_cfg_gpio < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)mclk_cfg_gpio;
    const gpio_num_t bclk_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_BCLK_GPIO;
    const gpio_num_t ws_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_WS_GPIO;
    const gpio_num_t din_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_DIN_GPIO;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 12;
    chan_cfg.dma_frame_num = 512;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, rx_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "falha ao criar canal I2S RX");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = mclk_gpio,
            .bclk = bclk_gpio,
            .ws = ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_LOGI(TAG,
             "I2S GPIO cfg: mclk_cfg=%d (%s), bclk=%d, ws=%d, din=%d",
             mclk_cfg_gpio,
             mclk_gpio == I2S_GPIO_UNUSED ? "unused" : "enabled",
             (int)bclk_gpio,
             (int)ws_gpio,
             (int)din_gpio);

    err = i2s_channel_init_std_mode(*rx_handle, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(*rx_handle);
        *rx_handle = NULL;
        ESP_LOGE(TAG, "falha ao configurar I2S STD: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t audio_capture_record_wav(const char *path,
                                   int duration_seconds,
                                   int gain_q8,
                                   int silence_threshold_rms,
                                   int silence_hysteresis_rms,
                                   audio_capture_result_t *result)
{
    int64_t capture_fn_start_ms = esp_log_timestamp();
    int64_t open_start_ms = 0;
    int64_t open_end_ms = 0;
    int64_t i2s_init_start_ms = 0;
    int64_t i2s_init_end_ms = 0;
    int64_t alloc_start_ms = 0;
    int64_t alloc_end_ms = 0;
    int64_t enable_start_ms = 0;
    int64_t enable_end_ms = 0;
    int64_t preroll_start_ms = 0;
    int64_t preroll_end_ms = 0;
    int64_t cleanup_start_ms = 0;
    int64_t cleanup_end_ms = 0;
    int64_t close_start_ms = 0;
    int64_t close_end_ms = 0;
    int64_t capture_fn_end_ms = 0;
    uint32_t read_ms = 0;
    uint32_t process_ms = 0;
    uint32_t write_ms = 0;
    uint32_t read_max_ms = 0;
    uint32_t process_max_ms = 0;
    uint32_t write_max_ms = 0;
    uint32_t block_count = 0;

    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path nulo");
    ESP_RETURN_ON_FALSE(duration_seconds >= AUDIO_CAPTURE_MIN_SECONDS &&
                            duration_seconds <= AUDIO_CAPTURE_MAX_SECONDS,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "duracao invalida");

    gain_q8 = normalize_gain_q8(gain_q8 > 0 ? gain_q8 : AUDIO_CAPTURE_DIGITAL_GAIN_Q8);
    silence_hysteresis_rms = normalize_silence_hysteresis_rms(silence_hysteresis_rms);

    if (result) {
        memset(result, 0, sizeof(*result));
        result->duration_seconds = duration_seconds;
        result->gain_q8 = gain_q8;
        result->silence_threshold_rms = silence_threshold_rms > 0 ? silence_threshold_rms : 0;
    }

    wav_writer_t *writer = NULL;
    wav_writer_config_t wav_config = {
        .sample_rate_hz = AUDIO_CAPTURE_SAMPLE_RATE_HZ,
        .channels = AUDIO_CAPTURE_CHANNELS,
        .bits_per_sample = AUDIO_CAPTURE_BITS_PER_SAMPLE,
    };

    open_start_ms = esp_log_timestamp();
    esp_err_t err = wav_writer_open(&writer, path, &wav_config);
    open_end_ms = esp_log_timestamp();
    ESP_RETURN_ON_ERROR(err, TAG, "falha ao abrir WAV");

    i2s_chan_handle_t rx_handle = NULL;
    i2s_init_start_ms = esp_log_timestamp();
    err = init_i2s(&rx_handle);
    i2s_init_end_ms = esp_log_timestamp();
    if (err != ESP_OK) {
        wav_writer_close(writer);
        return err;
    }

    alloc_start_ms = esp_log_timestamp();
    int16_t *mono_buffer = calloc(AUDIO_CAPTURE_BUFFER_FRAMES, sizeof(*mono_buffer));
    alloc_end_ms = esp_log_timestamp();
    if (!mono_buffer) {
        free(mono_buffer);
        wav_writer_close(writer);
        return ESP_ERR_NO_MEM;
    }

    const size_t target_bytes = (size_t)AUDIO_CAPTURE_SAMPLE_RATE_HZ *
                                AUDIO_CAPTURE_CHANNELS *
                                (AUDIO_CAPTURE_BITS_PER_SAMPLE / 8) *
                                (size_t)duration_seconds;
    const size_t preroll_bytes = (size_t)AUDIO_CAPTURE_SAMPLE_RATE_HZ *
                                 AUDIO_CAPTURE_CHANNELS *
                                 (AUDIO_CAPTURE_BITS_PER_SAMPLE / 8) *
                                 AUDIO_CAPTURE_PREROLL_DISCARD_MS / 1000;
    const size_t sample_bytes = AUDIO_CAPTURE_BITS_PER_SAMPLE / 8;
    size_t total_written = 0;
    size_t total_discarded = 0;
    size_t total_samples = 0;
    size_t active_samples = 0;
    uint64_t total_squares = 0;
    int16_t peak_sample = 0;
    bool clipped = false;
    bool silence_gate_active = false;
    bool i2s_enabled = false;

    ESP_LOGI(TAG, "capturando %d s para %s (%u bytes esperados)",
             duration_seconds,
             path,
             (unsigned)target_bytes);

    enable_start_ms = esp_log_timestamp();
    err = i2s_channel_enable(rx_handle);
    enable_end_ms = esp_log_timestamp();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar I2S RX: %s", esp_err_to_name(err));
        goto cleanup;
    }
    i2s_enabled = true;

    preroll_start_ms = esp_log_timestamp();
    while (total_discarded < preroll_bytes) {
        size_t bytes_read = 0;
        size_t bytes_to_read = AUDIO_CAPTURE_BUFFER_FRAMES * sample_bytes;
        size_t remaining = preroll_bytes - total_discarded;
        if (bytes_to_read > remaining) {
            bytes_to_read = remaining;
        }

        err = i2s_channel_read(rx_handle,
                               mono_buffer,
                               bytes_to_read,
                               &bytes_read,
                               AUDIO_CAPTURE_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao descartar pre-roll I2S: %s", esp_err_to_name(err));
            break;
        }
        if (bytes_read == 0) {
            err = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "pre-roll I2S sem dados");
            break;
        }
        total_discarded += bytes_read;
    }
    preroll_end_ms = esp_log_timestamp();

    while (total_written < target_bytes) {
        size_t bytes_read = 0;
        size_t remaining_mono_samples = (target_bytes - total_written) / sizeof(mono_buffer[0]);
        size_t frames_to_read = AUDIO_CAPTURE_BUFFER_FRAMES;
        if (frames_to_read > remaining_mono_samples) {
            frames_to_read = remaining_mono_samples;
        }
        size_t bytes_to_read = frames_to_read * sample_bytes;

        int64_t read_start_ms = esp_log_timestamp();
        err = i2s_channel_read(rx_handle,
                               mono_buffer,
                               bytes_to_read,
                               &bytes_read,
                               AUDIO_CAPTURE_READ_TIMEOUT_MS);
        int64_t read_end_ms = esp_log_timestamp();
        uint32_t block_read_ms = (uint32_t)(read_end_ms - read_start_ms);
        read_ms += block_read_ms;
        if (block_read_ms > read_max_ms) {
            read_max_ms = block_read_ms;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao ler I2S: %s", esp_err_to_name(err));
            break;
        }
        if (bytes_read == 0) {
            err = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "leitura I2S sem dados");
            break;
        }

        size_t frames_read = bytes_read / sample_bytes;
        uint64_t block_squares = 0;
        int64_t process_start_ms = esp_log_timestamp();
        for (size_t i = 0; i < frames_read; ++i) {
            int16_t gained_sample = apply_gain_with_saturation(
                mono_buffer[i],
                gain_q8,
                &clipped);
            int64_t sample_for_square = gained_sample;
            int16_t abs_sample = abs_i16_for_peak(gained_sample);
            uint64_t square = (uint64_t)(sample_for_square * sample_for_square);
            block_squares += square;
            total_squares += square;
            if (abs_sample > peak_sample) {
                peak_sample = abs_sample;
            }
            mono_buffer[i] = gained_sample;
        }
        int64_t process_end_ms = esp_log_timestamp();
        uint32_t block_process_ms = (uint32_t)(process_end_ms - process_start_ms);
        process_ms += block_process_ms;
        if (block_process_ms > process_max_ms) {
            process_max_ms = block_process_ms;
        }
        total_samples += frames_read;
        if (frames_read > 0) {
            double block_rms = sqrt((double)block_squares / (double)frames_read);
            silence_gate_active = silence_gate_accepts_block(silence_gate_active,
                                                             block_rms,
                                                             silence_threshold_rms,
                                                             silence_hysteresis_rms);
            if (silence_gate_active) {
                active_samples += frames_read;
            }
        }

        int64_t write_start_ms = esp_log_timestamp();
        err = wav_writer_write_pcm(writer, mono_buffer, frames_read * sizeof(mono_buffer[0]));
        int64_t write_end_ms = esp_log_timestamp();
        uint32_t block_write_ms = (uint32_t)(write_end_ms - write_start_ms);
        write_ms += block_write_ms;
        if (block_write_ms > write_max_ms) {
            write_max_ms = block_write_ms;
        }
        block_count++;
        if (err != ESP_OK) {
            break;
        }
        total_written += frames_read * sizeof(mono_buffer[0]);
    }

    if (result) {
        result->bytes_written = total_written;
        result->peak_sample = peak_sample;
        result->clipped = clipped;
        result->rms = total_samples > 0 ?
            sqrt((double)total_squares / (double)total_samples) : 0.0;
        result->active_ms = (uint32_t)(((uint64_t)active_samples * 1000ULL) /
                                       AUDIO_CAPTURE_SAMPLE_RATE_HZ);
    }

cleanup:
    cleanup_start_ms = esp_log_timestamp();
    if (rx_handle) {
        if (i2s_enabled) {
            esp_err_t disable_err = i2s_channel_disable(rx_handle);
            if (disable_err != ESP_OK) {
                ESP_LOGW(TAG, "falha ao parar I2S RX: %s", esp_err_to_name(disable_err));
            }
        }
        esp_err_t del_err = i2s_del_channel(rx_handle);
        if (del_err != ESP_OK) {
            ESP_LOGW(TAG, "falha ao liberar I2S RX: %s", esp_err_to_name(del_err));
        }
    }
    cleanup_end_ms = esp_log_timestamp();

    close_start_ms = esp_log_timestamp();
    esp_err_t close_err = wav_writer_close(writer);
    close_end_ms = esp_log_timestamp();
    if (err == ESP_OK) {
        err = close_err;
    }

    free(mono_buffer);
    capture_fn_end_ms = esp_log_timestamp();

    ESP_LOGI(TAG,
             "timing captura: total=%lldms open=%lldms i2s_init=%lldms alloc=%lldms enable=%lldms preroll=%lldms read=%ums process=%ums write=%ums cleanup=%lldms close=%lldms blocks=%u max_read=%ums max_process=%ums max_write=%ums",
             (long long)(capture_fn_end_ms - capture_fn_start_ms),
             (long long)(open_end_ms - open_start_ms),
             (long long)(i2s_init_end_ms - i2s_init_start_ms),
             (long long)(alloc_end_ms - alloc_start_ms),
             (long long)(enable_end_ms - enable_start_ms),
             (long long)(preroll_end_ms - preroll_start_ms),
             (unsigned)read_ms,
             (unsigned)process_ms,
             (unsigned)write_ms,
             (long long)(cleanup_end_ms - cleanup_start_ms),
             (long long)(close_end_ms - close_start_ms),
             (unsigned)block_count,
             (unsigned)read_max_ms,
             (unsigned)process_max_ms,
             (unsigned)write_max_ms);

    ESP_LOGI(TAG,
             "captura finalizada: %u bytes PCM, %u bytes pre-roll descartados, ganho=%.2fx, rms=%.1f, active=%ums, threshold=%d, pico=%d, clipped=%s",
             (unsigned)total_written,
             (unsigned)total_discarded,
             (double)gain_q8 / 256.0,
             result ? result->rms : 0.0,
             result ? (unsigned)result->active_ms : 0,
             silence_threshold_rms,
             (int)peak_sample,
             clipped ? "sim" : "nao");
    return err;
}

esp_err_t audio_capture_record_pcm_to_buffer(int duration_seconds,
                                             int gain_q8,
                                             int silence_threshold_rms,
                                             int silence_hysteresis_rms,
                                             audio_capture_result_t *result,
                                             audio_capture_buffer_t *buffer)
{
    int64_t capture_fn_start_ms = esp_log_timestamp();
    int64_t alloc_start_ms = 0;
    int64_t alloc_end_ms = 0;
    int64_t i2s_init_start_ms = 0;
    int64_t i2s_init_end_ms = 0;
    int64_t enable_start_ms = 0;
    int64_t enable_end_ms = 0;
    int64_t preroll_start_ms = 0;
    int64_t preroll_end_ms = 0;
    int64_t cleanup_start_ms = 0;
    int64_t cleanup_end_ms = 0;
    int64_t capture_fn_end_ms = 0;
    uint32_t read_ms = 0;
    uint32_t process_ms = 0;
    uint32_t copy_ms = 0;
    uint32_t read_max_ms = 0;
    uint32_t process_max_ms = 0;
    uint32_t copy_max_ms = 0;
    uint32_t block_count = 0;

    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "buffer nulo");
    ESP_RETURN_ON_FALSE(duration_seconds >= AUDIO_CAPTURE_MIN_SECONDS &&
                            duration_seconds <= AUDIO_CAPTURE_MAX_SECONDS,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "duracao invalida");

    gain_q8 = normalize_gain_q8(gain_q8 > 0 ? gain_q8 : AUDIO_CAPTURE_DIGITAL_GAIN_Q8);
    silence_hysteresis_rms = normalize_silence_hysteresis_rms(silence_hysteresis_rms);

    memset(buffer, 0, sizeof(*buffer));
    if (result) {
        memset(result, 0, sizeof(*result));
        result->duration_seconds = duration_seconds;
        result->gain_q8 = gain_q8;
        result->silence_threshold_rms = silence_threshold_rms > 0 ? silence_threshold_rms : 0;
    }

    const size_t target_bytes = (size_t)AUDIO_CAPTURE_SAMPLE_RATE_HZ *
                                AUDIO_CAPTURE_CHANNELS *
                                (AUDIO_CAPTURE_BITS_PER_SAMPLE / 8) *
                                (size_t)duration_seconds;
    const size_t preroll_bytes = (size_t)AUDIO_CAPTURE_SAMPLE_RATE_HZ *
                                 AUDIO_CAPTURE_CHANNELS *
                                 (AUDIO_CAPTURE_BITS_PER_SAMPLE / 8) *
                                 AUDIO_CAPTURE_PREROLL_DISCARD_MS / 1000;
    const size_t sample_bytes = AUDIO_CAPTURE_BITS_PER_SAMPLE / 8;

    alloc_start_ms = esp_log_timestamp();
    uint8_t *pcm_data = heap_caps_malloc(target_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buffer = calloc(AUDIO_CAPTURE_BUFFER_FRAMES, sizeof(*mono_buffer));
    alloc_end_ms = esp_log_timestamp();
    if (!pcm_data || !mono_buffer) {
        heap_caps_free(pcm_data);
        free(mono_buffer);
        ESP_LOGE(TAG,
                 "sem memoria para captura em PSRAM: pcm=%u bytes",
                 (unsigned)target_bytes);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t err = ESP_OK;
    size_t total_written = 0;
    size_t total_discarded = 0;
    size_t total_samples = 0;
    size_t active_samples = 0;
    uint64_t total_squares = 0;
    int16_t peak_sample = 0;
    bool clipped = false;
    bool silence_gate_active = false;
    bool i2s_enabled = false;

    i2s_init_start_ms = esp_log_timestamp();
    err = init_i2s(&rx_handle);
    i2s_init_end_ms = esp_log_timestamp();
    if (err != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "capturando %d s para PSRAM (%u bytes PCM esperados)",
             duration_seconds,
             (unsigned)target_bytes);

    enable_start_ms = esp_log_timestamp();
    err = i2s_channel_enable(rx_handle);
    enable_end_ms = esp_log_timestamp();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar I2S RX: %s", esp_err_to_name(err));
        goto cleanup;
    }
    i2s_enabled = true;

    preroll_start_ms = esp_log_timestamp();
    while (total_discarded < preroll_bytes) {
        size_t bytes_read = 0;
        size_t bytes_to_read = AUDIO_CAPTURE_BUFFER_FRAMES * sample_bytes;
        size_t remaining = preroll_bytes - total_discarded;
        if (bytes_to_read > remaining) {
            bytes_to_read = remaining;
        }

        err = i2s_channel_read(rx_handle,
                               mono_buffer,
                               bytes_to_read,
                               &bytes_read,
                               AUDIO_CAPTURE_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao descartar pre-roll I2S: %s", esp_err_to_name(err));
            break;
        }
        if (bytes_read == 0) {
            err = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "pre-roll I2S sem dados");
            break;
        }
        total_discarded += bytes_read;
    }
    preroll_end_ms = esp_log_timestamp();

    while (err == ESP_OK && total_written < target_bytes) {
        size_t bytes_read = 0;
        size_t remaining_mono_samples = (target_bytes - total_written) / sizeof(mono_buffer[0]);
        size_t frames_to_read = AUDIO_CAPTURE_BUFFER_FRAMES;
        if (frames_to_read > remaining_mono_samples) {
            frames_to_read = remaining_mono_samples;
        }
        size_t bytes_to_read = frames_to_read * sample_bytes;

        int64_t read_start_ms = esp_log_timestamp();
        err = i2s_channel_read(rx_handle,
                               mono_buffer,
                               bytes_to_read,
                               &bytes_read,
                               AUDIO_CAPTURE_READ_TIMEOUT_MS);
        int64_t read_end_ms = esp_log_timestamp();
        uint32_t block_read_ms = (uint32_t)(read_end_ms - read_start_ms);
        read_ms += block_read_ms;
        if (block_read_ms > read_max_ms) {
            read_max_ms = block_read_ms;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao ler I2S: %s", esp_err_to_name(err));
            break;
        }
        if (bytes_read == 0) {
            err = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "leitura I2S sem dados");
            break;
        }

        size_t frames_read = bytes_read / sample_bytes;
        uint64_t block_squares = 0;
        int64_t process_start_ms = esp_log_timestamp();
        for (size_t i = 0; i < frames_read; ++i) {
            int16_t gained_sample = apply_gain_with_saturation(mono_buffer[i],
                                                               gain_q8,
                                                               &clipped);
            int64_t sample_for_square = gained_sample;
            int16_t abs_sample = abs_i16_for_peak(gained_sample);
            uint64_t square = (uint64_t)(sample_for_square * sample_for_square);
            block_squares += square;
            total_squares += square;
            if (abs_sample > peak_sample) {
                peak_sample = abs_sample;
            }
            mono_buffer[i] = gained_sample;
        }
        int64_t process_end_ms = esp_log_timestamp();
        uint32_t block_process_ms = (uint32_t)(process_end_ms - process_start_ms);
        process_ms += block_process_ms;
        if (block_process_ms > process_max_ms) {
            process_max_ms = block_process_ms;
        }

        total_samples += frames_read;
        if (frames_read > 0) {
            double block_rms = sqrt((double)block_squares / (double)frames_read);
            silence_gate_active = silence_gate_accepts_block(silence_gate_active,
                                                             block_rms,
                                                             silence_threshold_rms,
                                                             silence_hysteresis_rms);
            if (silence_gate_active) {
                active_samples += frames_read;
            }
        }

        size_t bytes_to_copy = frames_read * sizeof(mono_buffer[0]);
        int64_t copy_start_ms = esp_log_timestamp();
        memcpy(pcm_data + total_written, mono_buffer, bytes_to_copy);
        int64_t copy_end_ms = esp_log_timestamp();
        uint32_t block_copy_ms = (uint32_t)(copy_end_ms - copy_start_ms);
        copy_ms += block_copy_ms;
        if (block_copy_ms > copy_max_ms) {
            copy_max_ms = block_copy_ms;
        }
        block_count++;
        total_written += bytes_to_copy;
    }

    if (result) {
        result->bytes_written = total_written;
        result->peak_sample = peak_sample;
        result->clipped = clipped;
        result->rms = total_samples > 0 ?
            sqrt((double)total_squares / (double)total_samples) : 0.0;
        result->active_ms = (uint32_t)(((uint64_t)active_samples * 1000ULL) /
                                       AUDIO_CAPTURE_SAMPLE_RATE_HZ);
    }

cleanup:
    cleanup_start_ms = esp_log_timestamp();
    if (rx_handle) {
        if (i2s_enabled) {
            esp_err_t disable_err = i2s_channel_disable(rx_handle);
            if (disable_err != ESP_OK) {
                ESP_LOGW(TAG, "falha ao parar I2S RX: %s", esp_err_to_name(disable_err));
            }
        }
        esp_err_t del_err = i2s_del_channel(rx_handle);
        if (del_err != ESP_OK) {
            ESP_LOGW(TAG, "falha ao liberar I2S RX: %s", esp_err_to_name(del_err));
        }
    }
    cleanup_end_ms = esp_log_timestamp();

    free(mono_buffer);
    if (err == ESP_OK) {
        buffer->pcm_data = pcm_data;
        buffer->pcm_size = total_written;
        buffer->sample_rate_hz = AUDIO_CAPTURE_SAMPLE_RATE_HZ;
        buffer->channels = AUDIO_CAPTURE_CHANNELS;
        buffer->bits_per_sample = AUDIO_CAPTURE_BITS_PER_SAMPLE;
    } else {
        heap_caps_free(pcm_data);
    }
    capture_fn_end_ms = esp_log_timestamp();

    ESP_LOGI(TAG,
             "timing captura PSRAM: total=%lldms alloc=%lldms i2s_init=%lldms enable=%lldms preroll=%lldms read=%ums process=%ums copy=%ums cleanup=%lldms blocks=%u max_read=%ums max_process=%ums max_copy=%ums",
             (long long)(capture_fn_end_ms - capture_fn_start_ms),
             (long long)(alloc_end_ms - alloc_start_ms),
             (long long)(i2s_init_end_ms - i2s_init_start_ms),
             (long long)(enable_end_ms - enable_start_ms),
             (long long)(preroll_end_ms - preroll_start_ms),
             (unsigned)read_ms,
             (unsigned)process_ms,
             (unsigned)copy_ms,
             (long long)(cleanup_end_ms - cleanup_start_ms),
             (unsigned)block_count,
             (unsigned)read_max_ms,
             (unsigned)process_max_ms,
             (unsigned)copy_max_ms);
    ESP_LOGI(TAG,
             "captura PSRAM finalizada: %u bytes PCM, %u bytes pre-roll descartados, ganho=%.2fx, rms=%.1f, active=%ums, threshold=%d, pico=%d, clipped=%s",
             (unsigned)total_written,
             (unsigned)total_discarded,
             (double)gain_q8 / 256.0,
             result ? result->rms : 0.0,
             result ? (unsigned)result->active_ms : 0,
             silence_threshold_rms,
             (int)peak_sample,
             clipped ? "sim" : "nao");

    return err;
}

void audio_capture_buffer_free(audio_capture_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }

    heap_caps_free(buffer->pcm_data);
    memset(buffer, 0, sizeof(*buffer));
}
