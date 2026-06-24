#ifndef TELE_CA_UPDATER_H
#define TELE_CA_UPDATER_H

#include <stdbool.h>

#include "esp_err.h"
#include "tele_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *manifest_url;
    const char *channel;
    bool restart_on_update;
} tele_ca_updater_config_t;

esp_err_t tele_ca_updater_check(const tele_ca_updater_config_t *config,
                                tele_manifest_artifact_t *out_artifact);

esp_err_t tele_ca_updater_apply(const tele_ca_updater_config_t *config,
                                tele_manifest_run_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
