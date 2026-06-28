#include <assert.h>
#include <string.h>

#include "cJSON.h"
#include "tele_commands.h"
#include "tele_config.h"
#include "tele_core_commands.h"
#include "tele_status.h"

static int state_calls;
static int technical_calls;
static int config_changed_calls;
static uint32_t restart_delay_ms;

esp_err_t tele_config_add_manifest_to_json(cJSON *root, uint32_t required_channel_flags)
{
    cJSON_AddNumberToObject(root, "config_channels", (double)required_channel_flags);
    return ESP_OK;
}

const tele_config_field_t *tele_config_find_field(const char *id)
{
    (void)id;
    return NULL;
}

esp_err_t tele_config_update_value(const char *id,
                                   const tele_config_value_t *value,
                                   tele_config_update_result_t *out_result)
{
    (void)id;
    (void)value;
    if (out_result) {
        out_result->stored = true;
    }
    return ESP_OK;
}

esp_err_t tele_config_reset_value(const char *id, tele_config_update_result_t *out_result)
{
    (void)id;
    if (out_result) {
        out_result->stored = true;
    }
    return ESP_OK;
}

esp_err_t tele_status_add_fields_to_json(cJSON *root,
                                         uint32_t required_channel_flags,
                                         uint32_t required_flags)
{
    cJSON_AddNumberToObject(root, "status_channels", (double)required_channel_flags);
    cJSON_AddNumberToObject(root, "status_flags", (double)required_flags);
    return ESP_OK;
}

esp_err_t tele_status_add_manifest_to_json(cJSON *root, uint32_t required_channel_flags)
{
    cJSON_AddNumberToObject(root, "status_manifest_channels", (double)required_channel_flags);
    return ESP_OK;
}

static cJSON *build_state(void *ctx)
{
    (void)ctx;
    state_calls++;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    cJSON_AddStringToObject(root, "source", "state_cb");
    return root;
}

static cJSON *build_technical_status(void *ctx)
{
    (void)ctx;
    technical_calls++;
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    cJSON_AddStringToObject(root, "source", "technical_cb");
    return root;
}

static void config_changed(void *ctx)
{
    (void)ctx;
    config_changed_calls++;
}

static void restart(uint32_t delay_ms, void *ctx)
{
    (void)ctx;
    restart_delay_ms = delay_ms;
}

int main(void)
{
    const tele_core_commands_config_t config = {
        .build_state = build_state,
        .build_technical_status = build_technical_status,
        .config_changed = config_changed,
        .restart = restart,
    };

    assert(tele_core_commands_register(&config) == ESP_OK);
    assert(tele_commands_find("ping") != NULL);
    assert(tele_commands_find("get_state") != NULL);
    assert(tele_commands_find("get_technical_status") != NULL);
    assert(tele_commands_find("config/get") != NULL);
    assert(tele_commands_find("commands/get") != NULL);
    assert(tele_commands_find("config/set") != NULL);
    assert(tele_commands_find("config/reset") != NULL);
    assert(tele_commands_find("apply_and_reboot") != NULL);

    tele_command_response_t response = {0};
    const tele_command_request_t state_request = {
        .cmd_id = "state-1",
        .name = "get_state",
        .required_channel_flags = 0,
    };
    assert(tele_commands_execute(&state_request, &response) == ESP_OK);
    assert(response.ok);
    assert(state_calls == 1);
    tele_commands_response_cleanup(&response);

    const tele_command_request_t technical_request = {
        .cmd_id = "technical-1",
        .name = "get_technical_status",
        .required_channel_flags = 0,
    };
    assert(tele_commands_execute(&technical_request, &response) == ESP_OK);
    assert(response.ok);
    assert(technical_calls == 1);
    tele_commands_response_cleanup(&response);

    cJSON *args = cJSON_CreateObject();
    assert(args != NULL);
    cJSON_AddNumberToObject(args, "delay_ms", 1500);
    const tele_command_request_t reboot_request = {
        .cmd_id = "reboot-1",
        .name = "apply_and_reboot",
        .args = args,
        .required_channel_flags = 0,
    };
    assert(tele_commands_execute(&reboot_request, &response) == ESP_OK);
    assert(response.ok);
    assert(restart_delay_ms == 1500);
    tele_commands_response_cleanup(&response);
    cJSON_Delete(args);

    return 0;
}
