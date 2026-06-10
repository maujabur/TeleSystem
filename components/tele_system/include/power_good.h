#ifndef POWER_GOOD_H
#define POWER_GOOD_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Inicializa o sinal POWER_GOOD.
 *
 * Configura a GPIO como saida e a deixa no nivel inativo (perifericos
 * desligados). Deve ser chamado antes de power_good_set().
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t power_good_init(void);

/**
 * @brief Liga ou desliga os perifericos via POWER_GOOD.
 *
 * @param on true  = nivel ativo (perifericos ligados)
 *           false = nivel inativo (perifericos desligados)
 */
void power_good_set(bool on);

#endif /* POWER_GOOD_H */
