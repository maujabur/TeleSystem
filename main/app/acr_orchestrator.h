#ifndef ACR_ORCHESTRATOR_H
#define ACR_ORCHESTRATOR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t acr_orchestrator_force_cycle(void);
void acr_orchestrator_run(void);

#ifdef __cplusplus
}
#endif

#endif
