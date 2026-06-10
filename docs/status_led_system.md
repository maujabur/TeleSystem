# Sistema de LED de status

## Objetivo

`status_led` controla um LED WS28xx, por padrao no IO48, usando RMT. O modulo
apresenta estados recebidos de outros subsistemas e nao decide regras de Wi-Fi,
portal ou produto.

## Arquivos

- `main/connectivity/status_led.c`
- `main/connectivity/status_led.h`
- `main/connectivity/status_led_stub.c`
- `main/connectivity/connectivity_controller.c`
- `main/Kconfig.projbuild`

## API publica

```c
typedef enum {
    STATUS_LED_STATE_BOOT = 0,
    STATUS_LED_STATE_WIFI_CONNECTING,
    STATUS_LED_STATE_WIFI_PROVISIONING,
    STATUS_LED_STATE_WIFI_CONNECTED,
    STATUS_LED_STATE_PRODUCT_TRANSMITTING,
    STATUS_LED_STATE_PRODUCT_WAITING,
    STATUS_LED_STATE_PRODUCT_RESULT_OK,
    STATUS_LED_STATE_PRODUCT_RESULT_ALERT,
    STATUS_LED_STATE_OUTPUT_ACTIVE,
    STATUS_LED_STATE_ERROR,
    STATUS_LED_STATE_LOW_BATTERY,
} status_led_state_t;

esp_err_t status_led_start(void);
status_led_state_t status_led_get_state(void);
esp_err_t status_led_set_state(status_led_state_t state);
esp_err_t status_led_set_capture_overlay(bool enabled);
```

## Estados padrao

| Estado | Cor padrao | Padrao |
|---|---:|---|
| Boot | `0x202020` | pisca lento |
| Wi-Fi conectando | `0x0040FF` | pisca rapido |
| Provisionamento AP | `0xFF9000` | pisca medio |
| Wi-Fi conectado | `0x00B050` | fixo |
| Produto transmitindo | `0x8000FF` | pisca rapido |
| Produto aguardando | `0x00C8FF` | pulso lento |
| Resultado OK | `0x00FF00` | fixo |
| Resultado alerta | `0xFF00A0` | fixo |
| Saida ativa | `0xFFFFFF` | fixo |
| Erro | `0xFF0000` | pisca rapido |
| Bateria baixa | `0xFF2000` | pisca lento |

## Configuracao

As opcoes ficam em `Status LED Config`:

- GPIO;
- quantidade de LEDs;
- brilho;
- ordem fisica dos canais (`RGB`, `RBG`, `GRB`, `GBR`, `BRG`, `BGR`);
- cores por estado;
- tempos de piscada;
- overlay de atividade.

O formato das cores e sempre RGB logico:

```text
0xRRGGBB
```

## Fluxo atual

`wifi_manager` publica eventos, `connectivity_controller` interpreta o estado de
conectividade e chama `status_led_set_state`. Modulos de produto futuros podem
usar os estados `PRODUCT_*` e `OUTPUT_ACTIVE` sem alterar o driver do LED.
