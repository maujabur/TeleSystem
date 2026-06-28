# tele_indicator

Guia pratico para usar o sistema de indicacao visual implementado com
`tele_signal`, `status_led`, `tele_indicator` e `app_indicators`.

Este sistema separa tres responsabilidades:

- `tele_signal`: descreve o que deve ser mostrado, sem conhecer hardware ou
  regra de negocio.
- `status_led`: controla o LED fisico e executa efeitos simples.
- `tele_indicator`: decide qual evento deve aparecer agora, com prioridade,
  desempate por recencia e duracao.
- `app_indicators`: tabela do produto. Ela registra outputs, fontes e eventos
  semanticos como `wifi.connecting`, `battery.low` e `system.error`.

O fluxo final fica assim:

```text
modulo de dominio
    -> tele_indicator_raise("wifi", "wifi.connecting")
    -> tele_indicator escolhe o evento efetivo
    -> callback do output
    -> status_led_apply_effect()
    -> LED fisico
```

## Quando Usar

Use `tele_indicator` quando o codigo quer expressar uma intencao semantica:

```c
tele_indicator_raise("wifi", "wifi.connecting");
tele_indicator_raise("battery", "battery.low");
tele_indicator_clear_source("wifi");
```

Use `status_led_apply_effect()` diretamente apenas para teste de driver ou
diagnostico de hardware:

```c
tele_signal_effect_t effect = {
    .id = TELE_SIGNAL_EFFECT_SOLID,
    .color_a = {.red = 255, .green = 0, .blue = 0},
    .brightness = 80,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};

ESP_ERROR_CHECK(status_led_apply_effect(&effect));
```

## Conceitos

### Efeito

Um efeito e uma descricao generica de sinal fisico:

```c
typedef struct {
    char id[TELE_SIGNAL_ID_SIZE];
    tele_signal_color_t color_a;
    tele_signal_color_t color_b;
    uint32_t time_a_ms;
    uint32_t time_b_ms;
    uint8_t brightness;
    uint32_t target_mask;
    uint32_t flags;
} tele_signal_effect_t;
```

IDs de efeito suportados nesta etapa:

- `TELE_SIGNAL_EFFECT_OFF`
- `TELE_SIGNAL_EFFECT_SOLID`
- `TELE_SIGNAL_EFFECT_BLINK`
- `TELE_SIGNAL_EFFECT_ALTERNATE`
- `TELE_SIGNAL_EFFECT_BREATH`
- `TELE_SIGNAL_EFFECT_HEARTBEAT`
- `TELE_SIGNAL_EFFECT_PULSE`

`duration_ms` nao pertence ao efeito. A duracao pertence ao evento registrado
no `tele_indicator`.

### Output

Um output e um destino fisico ou logico: `status_led`, `front_leds`, `buzzer`,
`display`, etc.

Cada output declara quais efeitos suporta e fornece um callback:

```c
static esp_err_t status_led_output_apply(const tele_signal_effect_t *effect,
                                         void *ctx)
{
    (void)ctx;
    return status_led_apply_effect(effect);
}

static const tele_indicator_output_t s_status_led_output = {
    .id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
    .apply = status_led_output_apply,
};
```

O `status_led` exporta sua propria lista de capacidades:

```c
tele_indicator_output_t output = s_status_led_output;
output.supported_effect_ids = status_led_supported_effect_ids();
output.supported_effect_count = status_led_supported_effect_count();

ESP_ERROR_CHECK(tele_indicator_register_output(&output));
```

Se um evento usar um efeito que o output nao suporta,
`tele_indicator_register_event()` retorna `ESP_ERR_NOT_SUPPORTED`.

### Fonte

Uma fonte representa quem esta pedindo a indicacao:

```c
static const tele_indicator_source_t s_sources[] = {
    {.id = "system", .default_priority = 100},
    {.id = "wifi", .default_priority = 20},
    {.id = "product", .default_priority = 40},
    {.id = "battery", .default_priority = 120},
};
```

O implementation atual mantem um slot ativo por fonte. Se `system.error` for
levantado pela fonte `system`, ele substitui o evento anterior dessa mesma
fonte.

### Evento

Um evento liga uma semantica do produto a um efeito fisico:

```c
static const tele_indicator_event_t s_events[] = {
    {
        .id = "wifi.connecting",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 20,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .time_a_ms = 250,
            .time_b_ms = 750,
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connected",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 15,
        .duration_ms = 1500,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_SOLID,
            .color_a = {.red = 0, .green = 255, .blue = 0},
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
};
```

Eventos com `duration_ms = 0` ficam ativos ate serem limpos.
Eventos com `duration_ms > 0` sao limpos automaticamente.

## Politica De Selecao

O `tele_indicator` escolhe o evento efetivo assim:

1. considera apenas eventos ativos;
2. escolhe maior `priority`;
3. em empate, escolhe o evento mais recente;
4. se nao houver evento ativo, aplica `off` no output default.

Exemplo:

```c
tele_indicator_raise("wifi", "wifi.connecting");  // prioridade 20
tele_indicator_raise("battery", "battery.low");   // prioridade 180, vence
tele_indicator_clear_source("battery");           // volta para WiFi
```

## Inicializacao Em Um Projeto Novo

### 1. Copie os componentes

Inclua no projeto:

```text
components/
  tele_signal/
  tele_indicator/
  status_led/
```

Para projetos sem LED WS28xx, mantenha `tele_signal` e `tele_indicator` e
troque `status_led` por outro output fisico.

### 2. Configure CMake dos componentes

`tele_signal/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "tele_signal.c"
    INCLUDE_DIRS "include"
)
```

`tele_indicator/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "tele_indicator.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer tele_signal
)
```

`status_led/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt tele_signal
)
```

### 3. Crie `main/app_indicators.h`

```c
#ifndef APP_INDICATORS_H
#define APP_INDICATORS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_indicators_start(void);

#ifdef __cplusplus
}
#endif

#endif
```

### 4. Crie `main/app_indicators.c`

Exemplo minimo com boot, Wi-Fi conectando, Wi-Fi conectado e erro:

```c
#include "app_indicators.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "status_led.h"
#include "tele_indicator.h"

static const char *TAG = "app-indicators";
static bool s_started;

static esp_err_t status_led_output_apply(const tele_signal_effect_t *effect,
                                         void *ctx)
{
    (void)ctx;
    return status_led_apply_effect(effect);
}

static const tele_indicator_output_t s_status_led_output = {
    .id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
    .apply = status_led_output_apply,
};

static const tele_indicator_source_t s_sources[] = {
    {.id = "system", .default_priority = 100},
    {.id = "wifi", .default_priority = 20},
};

static const tele_indicator_event_t s_events[] = {
    {
        .id = "system.boot",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 10,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BREATH,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .time_a_ms = 1200,
            .brightness = 64,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connecting",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 20,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .time_a_ms = 250,
            .time_b_ms = 750,
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connected",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 15,
        .duration_ms = 1500,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_SOLID,
            .color_a = {.red = 0, .green = 255, .blue = 0},
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "system.error",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 250,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 255, .green = 0, .blue = 0},
            .time_a_ms = 120,
            .time_b_ms = 120,
            .brightness = 120,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
};

esp_err_t app_indicators_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(status_led_start(), TAG, "status_led_start failed");
    ESP_RETURN_ON_ERROR(tele_indicator_init(), TAG, "tele_indicator_init failed");

    tele_indicator_output_t status_led_output = s_status_led_output;
    status_led_output.supported_effect_ids = status_led_supported_effect_ids();
    status_led_output.supported_effect_count = status_led_supported_effect_count();

    ESP_RETURN_ON_ERROR(tele_indicator_register_output(&status_led_output),
                        TAG,
                        "register status_led output failed");

    for (size_t i = 0; i < sizeof(s_sources) / sizeof(s_sources[0]); ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_source(&s_sources[i]),
                            TAG,
                            "register source failed");
    }

    for (size_t i = 0; i < sizeof(s_events) / sizeof(s_events[0]); ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_event(&s_events[i]),
                            TAG,
                            "register event failed");
    }

    ESP_RETURN_ON_ERROR(tele_indicator_raise("system", "system.boot"),
                        TAG,
                        "raise boot indicator failed");

    s_started = true;
    return ESP_OK;
}
```

### 5. Registre no `main/CMakeLists.txt`

```cmake
set(MAIN_SRCS
    "main.c"
    "app_indicators.c"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES status_led tele_indicator tele_signal
)
```

### 6. Inicialize no boot

```c
#include "app_indicators.h"

void app_main(void)
{
    ESP_ERROR_CHECK(app_indicators_start());

    // Inicialize Wi-Fi, sensores, MQTT e regras de produto depois disso.
}
```

## Uso Em Modulos De Dominio

Um modulo de conectividade nao precisa conhecer cor, brilho ou tempo de blink.
Ele apenas levanta eventos:

```c
static void sync_wifi_indicator(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_CONNECTING:
        (void)tele_indicator_raise("wifi", "wifi.connecting");
        break;

    case WIFI_STATE_CONNECTED:
        (void)tele_indicator_raise("wifi", "wifi.connected");
        break;

    case WIFI_STATE_IDLE:
    default:
        (void)tele_indicator_clear_source("wifi");
        break;
    }
}
```

Um modulo de bateria pode ter prioridade maior:

```c
void battery_on_low_voltage(void)
{
    (void)tele_indicator_raise("battery", "battery.low");
}

void battery_on_voltage_recovered(void)
{
    (void)tele_indicator_clear_source("battery");
}
```

Um erro global pode ganhar de tudo:

```c
void fatal_error_indicator_on(void)
{
    (void)tele_indicator_raise("system", "system.error");
}

void fatal_error_indicator_off(void)
{
    (void)tele_indicator_clear_event("system.error");
}
```

## Eventos Temporarios

Eventos temporarios sao bons para feedback de transicao:

```c
{
    .id = "product.result_ok",
    .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
    .priority = 80,
    .duration_ms = 1200,
    .effect = {
        .id = TELE_SIGNAL_EFFECT_SOLID,
        .color_a = {.red = 0, .green = 255, .blue = 0},
        .brightness = 100,
        .target_mask = TELE_SIGNAL_TARGET_ALL,
    },
}
```

Uso:

```c
tele_indicator_raise("product", "product.result_ok");
```

Depois de 1200 ms, o evento e removido automaticamente. Se havia outro evento
ativo de menor prioridade, ele volta a ser mostrado.

## Consultar Estado Efetivo

Para debug, teste ou telemetria local:

```c
tele_indicator_effective_t effective = {0};

if (tele_indicator_get_effective(&effective) == ESP_OK && effective.active) {
    ESP_LOGI(TAG,
             "indicator source=%s event=%s output=%s effect=%s priority=%u sequence=%lu",
             effective.source_id,
             effective.event_id,
             effective.output_id,
             effective.effect.id,
             effective.priority,
             (unsigned long)effective.sequence);
}
```

## Criando Outro Output

Exemplo conceitual de output para buzzer. Ele aceita apenas `off`, `pulse` e
`heartbeat`:

```c
static esp_err_t buzzer_apply(const tele_signal_effect_t *effect, void *ctx)
{
    buzzer_driver_t *driver = (buzzer_driver_t *)ctx;

    if (strcmp(effect->id, TELE_SIGNAL_EFFECT_OFF) == 0) {
        return buzzer_stop(driver);
    }

    if (strcmp(effect->id, TELE_SIGNAL_EFFECT_PULSE) == 0) {
        return buzzer_pulse(driver, effect->time_a_ms);
    }

    if (strcmp(effect->id, TELE_SIGNAL_EFFECT_HEARTBEAT) == 0) {
        return buzzer_heartbeat(driver, effect->time_a_ms);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static const char *const s_buzzer_effects[] = {
    TELE_SIGNAL_EFFECT_OFF,
    TELE_SIGNAL_EFFECT_PULSE,
    TELE_SIGNAL_EFFECT_HEARTBEAT,
};

static tele_indicator_output_t make_buzzer_output(buzzer_driver_t *driver)
{
    return (tele_indicator_output_t) {
        .id = "buzzer",
        .supported_effect_ids = s_buzzer_effects,
        .supported_effect_count = sizeof(s_buzzer_effects) / sizeof(s_buzzer_effects[0]),
        .apply = buzzer_apply,
        .ctx = driver,
    };
}
```

Evento usando esse output:

```c
{
    .id = "system.error_sound",
    .output_id = "buzzer",
    .priority = 240,
    .duration_ms = 0,
    .effect = {
        .id = TELE_SIGNAL_EFFECT_HEARTBEAT,
        .time_a_ms = 1000,
        .target_mask = TELE_SIGNAL_TARGET_ALL,
    },
}
```

## Boas Praticas

- Defina eventos em uma tabela central (`app_indicators.c`).
- Evite espalhar cores e tempos por modulos de dominio.
- Use nomes estaveis de evento: `wifi.connecting`, `battery.low`,
  `product.result_ok`.
- Deixe eventos persistentes com `duration_ms = 0`.
- Use eventos temporarios para feedback momentaneo.
- Reserve prioridades altas para seguranca, erro e bateria.
- Nao chame callbacks de output diretamente a partir de modulos de dominio.
- Teste eventos temporarios com retorno ao estado anterior.

## Troubleshooting

### `tele_indicator_register_event()` retorna `ESP_ERR_NOT_FOUND`

O `output_id` do evento nao foi registrado ainda. Registre o output antes dos
eventos.

### `tele_indicator_register_event()` retorna `ESP_ERR_NOT_SUPPORTED`

O efeito do evento nao esta em `supported_effect_ids` do output.

### Nada aparece no LED

Verifique:

- `app_indicators_start()` foi chamado;
- `status_led_start()` retornou `ESP_OK`;
- o evento foi registrado;
- a fonte foi registrada;
- o evento tem `output_id` correto;
- `CONFIG_STATUS_LED_ENABLED` esta habilitado quando quiser testar o driver
  real, nao o stub.

### Um evento nao volta depois de outro ser limpo

Lembre que existe um slot ativo por fonte. Se dois eventos usam a mesma fonte,
o mais recente substitui o anterior dessa fonte.

## Testes Host

Os testes atuais podem ser compilados fora do ESP-IDF:

```bash
gcc -Icomponents/tele_signal/include \
    components/tele_signal/test/tele_signal_effect_id_test.c \
    components/tele_signal/tele_signal.c \
    -o /tmp/tele_signal_effect_id_test

/tmp/tele_signal_effect_id_test
```

```bash
gcc -DTELE_INDICATOR_HOST_TEST \
    -Icomponents/tele_indicator/include \
    -Icomponents/tele_signal/include \
    components/tele_indicator/test/tele_indicator_registry_test.c \
    components/tele_indicator/tele_indicator.c \
    components/tele_signal/tele_signal.c \
    -o /tmp/tele_indicator_registry_test

/tmp/tele_indicator_registry_test
```

O teste de `tele_indicator` cobre registro de output, source, event,
prioridade, desempate por recencia, evento temporario e protecao contra timer
antigo limpando evento novo.
