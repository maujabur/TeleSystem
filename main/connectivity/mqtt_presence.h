#ifndef MQTT_PRESENCE_H
#define MQTT_PRESENCE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_presence_start(void);
esp_err_t mqtt_presence_suspend_for_acr(void);
esp_err_t mqtt_presence_resume_after_acr(void);

#ifdef __cplusplus
}
#endif

#endif
