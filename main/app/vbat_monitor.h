#ifndef VBAT_MONITOR_H
#define VBAT_MONITOR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    VBAT_MONITOR_MOMENT_BOOT = 0,
    VBAT_MONITOR_MOMENT_BEFORE_WIFI_PS_EXIT,
    VBAT_MONITOR_MOMENT_AFTER_ACR_TX,
    VBAT_MONITOR_MOMENT_AFTER_AUDIO_CAPTURE,
    VBAT_MONITOR_MOMENT_MAX_INTERVAL,
} vbat_monitor_moment_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool calibrated;
    bool shutdown_enabled;
    bool shutdown_countdown_active;
    bool maintenance_mode;
    int gpio;
    int raw_avg;
    int gpio_mv;
    int vbat_mv;
    int shutdown_threshold_mv;
    int shutdown_debounce_ms;
    int64_t last_measurement_ms;
    int64_t shutdown_below_threshold_since_ms;
    vbat_monitor_moment_t last_moment;
    uint32_t measurement_count;
} vbat_monitor_status_t;

esp_err_t vbat_monitor_init(void);
void vbat_monitor_maybe_measure(vbat_monitor_moment_t moment);
TickType_t vbat_monitor_next_wait_timeout(TickType_t base_timeout);
esp_err_t vbat_monitor_get_status(vbat_monitor_status_t *out);
const char *vbat_monitor_moment_name(vbat_monitor_moment_t moment);

/**
 * @brief Verifica se o sistema esta em modo de manutencao (bateria desconectada).
 *
 * Retorna true quando a ultima leitura de vbat_mv for menor ou igual a
 * CONFIG_VBAT_MAINTENANCE_MODE_MAX_MV, indicando que a bateria esta
 * desconectada. Neste estado o desligamento automatico e inibido.
 *
 * @return true se em modo manutencao, false caso contrario.
 */
bool vbat_monitor_is_maintenance_mode(void);

/**
 * @brief Verifica se a ultima leitura esta abaixo do limiar de desligamento.
 *
 * Nao aplica debounce — indica apenas se a leitura mais recente ficou
 * abaixo de CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV. Usar somente no boot,
 * onde ha apenas um ponto de dados disponivel.
 *
 * @return true se vbat_mv < threshold, false caso contrario ou se
 *         VBAT_SHUTDOWN_ENABLED estiver desabilitado.
 */
bool vbat_monitor_is_below_threshold(void);

/**
 * @brief Verifica se o sistema deve entrar em desligamento por bateria baixa.
 *
 * Retorna true quando a tensao esteve continuamente abaixo do limiar por
 * pelo menos CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS. Usar no loop de operacao.
 *
 * @return true se desligamento deve ser iniciado, false caso contrario.
 */
bool vbat_monitor_check_shutdown(void);

#endif