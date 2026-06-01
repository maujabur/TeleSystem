# Manual de branding da pagina inicial

## Objetivo

Este documento orienta alteracoes visuais na pagina inicial do produto,
servida em `/` a partir de HTML embutido no firmware, com os arquivos fonte em:

[firmware_assets/web/index.html](../firmware_assets/web/index.html)

A pagina inicial e a interface que o usuario final deve acessar. Ela deve
continuar simples: mostrar estado basico de Wi-Fi, permitir escolher uma rede,
informar a senha e conectar o dispositivo.

As paginas tecnicas (`/status`, `/settings` e `/logs`) nao fazem parte deste
manual. Elas podem ter outra identidade visual e nao devem aparecer como botao
ou link visivel na pagina `/`.

## O Que Pode Mudar

Quem for aplicar o branding do cliente pode alterar:

- nome exibido no `h1`;
- cores em `:root`;
- familia de fonte;
- espacamentos, bordas, sombras e proporcoes;
- textos visiveis, desde que mantenham o mesmo sentido operacional;
- layout visual dos blocos;
- inclusao de logotipo, imagem de fundo ou elementos de marca;
- estilo dos botoes, inputs, cards e mensagens.

Ao adicionar imagens ou fontes locais, coloque os arquivos em `firmware_assets/web/` ou
em uma subpasta dentro de `firmware_assets/web/`. Depois disso, e necessario gerar uma
nova compilacao para embutir os assets atualizados no firmware.

## O Que Deve Permanecer

Nao remova nem renomeie estes elementos sem tambem alterar o JavaScript da
propria pagina:

| ID | Funcao |
|---|---|
| `statusText` | mostra estado geral da conexao |
| `provisioningBadge` | mostra o SSID de provisionamento |
| `wifiPanel` | container do formulario Wi-Fi |
| `networkSelect` | lista redes encontradas |
| `refreshNetworksButton` | atualiza a lista de redes |
| `ssid` | campo SSID enviado para o firmware |
| `password` | campo senha enviado para o firmware |
| `wifiBanner` | mostra sucesso/erro de Wi-Fi |
| `currentSsid` | mostra a rede atual |
| `currentIp` | mostra o IP atual |

Tambem mantenha estas funcoes JavaScript, ou atualize todos os pontos onde elas
sao chamadas:

- `loadNetworks()`;
- `saveWifi()`;
- `refreshStatus()`;
- `updateSelectedNetwork()`;
- `showBanner()`;
- `hideBanner()`.

## Funcionamento Da Pagina

A pagina usa HTML, CSS e JavaScript inline. Nao existe framework frontend.

Fluxo normal:

1. Ao carregar, chama `loadNetworks()` para buscar redes proximas.
2. Chama `refreshStatus()` para obter estado do dispositivo.
3. Atualiza `refreshStatus()` a cada 3 segundos.
4. Quando o usuario escolhe uma rede no select, copia o SSID para o campo
   `ssid`.
5. Quando o usuario clica em `Conectar`, chama `saveWifi()`.
6. `saveWifi()` envia SSID e senha ao firmware.
7. A UI mostra a resposta em `wifiBanner`.

## Endpoints Usados

### `GET /api/status`

Usado por `refreshStatus()`.

Campos usados pela pagina:

- `provisioning_ssid`;
- `ssid`;
- `ip`;
- `wifi_ready`;
- `state`;
- `last_error`.

Esse endpoint retorna mais campos tecnicos, mas a pagina `/` nao deve exibir
ACR, firmware, logs, BT_NEXT ou metricas de captura.

### `GET /api/wifi/networks`

Usado por `loadNetworks()`.

Campos usados:

- `networks[].ssid`;
- `networks[].rssi`;
- `networks[].auth_required`.

A lista mostra uma descricao simples de sinal:

- `Excelente`;
- `Bom`;
- `Regular`;
- `Fraco`.

### `POST /api/wifi`

Usado por `saveWifi()`.

Payload enviado:

```json
{
  "ssid": "nome-da-rede",
  "password": "senha-da-rede"
}
```

O firmware retorna texto simples. Esse texto e exibido em `wifiBanner`.

## Comportamentos Importantes

- A pagina preserva a ultima lista valida de redes se uma nova busca falhar.
- Apos enviar credenciais, a pagina espera o estado sair de `sta_connecting`
  antes de atualizar a lista de redes novamente.
- Se `last_error` vier preenchido em `/api/status`, ele aparece como erro no
  banner.
- Se `wifi_ready` estiver verdadeiro, a UI mostra sucesso e atualiza o SSID
  atual.
- O campo `password` nunca e preenchido pelo firmware.

## Regras Para Branding

Ao customizar a aparencia para o cliente:

- mantenha a pagina focada em Wi-Fi/provisionamento;
- nao adicione botoes para `/status`, `/settings`, `/logs` ou rotas tecnicas;
- nao exiba versao de firmware na pagina `/`;
- nao exiba ACR, IA/humano, BT_NEXT, logs ou metricas de audio;
- mantenha os campos de SSID e senha claros;
- mantenha feedback de sucesso e erro visivel;
- garanta que os textos caibam em telas pequenas;
- preserve o comportamento em celular, especialmente durante captive portal.

## Rotas Tecnicas Fora Do Escopo

Estas rotas existem para manutencao, mas nao devem ser anunciadas na home:

- `/status`;
- `/settings`;
- `/logs`;
- `/device-config`, se estiver habilitada no firmware.

Elas podem ser acessadas por URL conhecida durante desenvolvimento ou
manutencao, mas a experiencia do usuario final deve permanecer centrada em `/`.

## Checklist Antes De Entregar

- A pagina `/` carrega sem erro.
- A lista de redes aparece ou mostra erro amigavel.
- Selecionar uma rede preenche o campo SSID.
- O botao `Conectar` envia `POST /api/wifi`.
- Mensagens de erro e sucesso aparecem em `wifiBanner`.
- SSID atual e IP atual aparecem nos campos inferiores.
- Nao ha link ou botao visivel para paginas tecnicas.
- Nao ha informacao de ACR, firmware, logs, audio ou BT_NEXT.
- O layout funciona em celular e desktop.
- Depois de alterar `firmware_assets/web/index.html`, o build regenerou os assets
  embutidos.
