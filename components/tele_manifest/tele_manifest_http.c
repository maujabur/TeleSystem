#include "tele_manifest.h"

esp_err_t tele_manifest_http_get_text(const char *url,
                                      char *out_text,
                                      size_t out_size,
                                      int *out_status)
{
    (void)url;
    (void)out_text;
    (void)out_size;
    if (out_status) {
        *out_status = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
