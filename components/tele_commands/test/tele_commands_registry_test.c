#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tele_commands.h"

static const tele_command_arg_t config_set_args[] = {
    {
        .id = "id",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
    },
    {
        .id = "value",
        .type = TELE_COMMAND_ARG_ANY,
        .required = true,
    },
};

static const tele_command_arg_t reboot_args[] = {
    {
        .id = "delay_ms",
        .type = TELE_COMMAND_ARG_U32,
        .required = false,
        .min.u32 = 100,
        .max.u32 = 10000,
    },
};

static const tele_command_t commands[] = {
    {
        .name = "ping",
        .label = "Ping",
        .description = "Solicita uma resposta imediata do equipamento.",
        .group = "system",
        .flags = TELE_COMMAND_FLAG_MQTT,
    },
    {
        .name = "config/set",
        .label = "Salvar configuracao",
        .description = "Atualiza um campo configuravel.",
        .group = "config",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_MUTATING,
        .args = config_set_args,
        .arg_count = 2,
    },
    {
        .name = "apply_and_reboot",
        .label = "Aplicar e reiniciar",
        .description = "Agenda um reboot curto depois do ACK.",
        .group = "system",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_MUTATING | TELE_COMMAND_FLAG_REBOOT_REQUIRED,
        .args = reboot_args,
        .arg_count = 1,
    },
};

int main(void)
{
    cJSON *root = NULL;
    char *text = NULL;

    assert(tele_commands_register(commands, 3) == ESP_OK);
    assert(tele_commands_find("config/set") == &commands[1]);
    assert(tele_commands_find("missing") == NULL);
    assert(tele_commands_register(commands, 1) == ESP_ERR_INVALID_STATE);

    root = cJSON_CreateObject();
    assert(root != NULL);
    assert(tele_commands_add_manifest_to_json(root, TELE_COMMAND_FLAG_MQTT) == ESP_OK);
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);
    assert(strstr(text, "\"registry_revision\":1") != NULL);
    assert(strstr(text, "\"commands\"") != NULL);
    assert(strstr(text, "\"name\":\"ping\"") != NULL);
    assert(strstr(text, "\"label\":\"Ping\"") != NULL);
    assert(strstr(text, "\"description\":\"Solicita uma resposta imediata do equipamento.\"") != NULL);
    assert(strstr(text, "\"group\":\"system\"") != NULL);
    assert(strstr(text, "\"name\":\"config/set\"") != NULL);
    assert(strstr(text, "\"group\":\"config\"") != NULL);
    assert(strstr(text, "\"mutating\":true") != NULL);
    assert(strstr(text, "\"name\":\"apply_and_reboot\"") != NULL);
    assert(strstr(text, "\"reboot_required\":true") != NULL);
    assert(strstr(text, "\"id\":\"delay_ms\"") != NULL);
    assert(strstr(text, "\"type\":\"u32\"") != NULL);
    assert(strstr(text, "\"min\":100") != NULL);
    assert(strstr(text, "\"max\":10000") != NULL);
    assert(strstr(text, "\"id\":\"value\"") != NULL);
    assert(strstr(text, "\"type\":\"any\"") != NULL);
    cJSON_free(text);
    cJSON_Delete(root);

    return 0;
}
