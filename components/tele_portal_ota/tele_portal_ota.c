#include "cJSON.h"
#include "esp_http_server.h"

#include "http_helpers.h"
#include "tele_portal_core.h"
#include "tele_portal_ota.h"

#define TELE_PORTAL_OTA_DEFAULT_RESTART_DELAY_MS 1200

static tele_portal_ota_config_t s_config;

static const char *OTA_PAGE_HTML =
"<!doctype html>"
"<html lang='pt-BR'>"
"<head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>OTA</title>"
"<style>"
":root{color-scheme:light;--bg:#eef4f7;--panel:#fff;--text:#172026;--muted:#5d6b75;--line:#c8d7df;--primary:#0d5b78;--primary-dark:#08465c;--soft:#f4f8fa;--ok-bg:#e6f6ee;--ok-text:#1e6b43;--error-bg:#fdecea;--error-text:#8b2d2d;}"
"*{box-sizing:border-box}body{min-height:100vh;margin:0;background:linear-gradient(180deg,#eef4f7 0%,#f7fbfd 100%);color:var(--text);font-family:Arial,Helvetica,sans-serif}.shell{width:min(760px,calc(100% - 32px));margin:0 auto;padding:24px 0}header{margin-bottom:18px}h1{margin:0 0 8px;font-size:30px}.status{margin:0;color:var(--muted);font-size:14px;line-height:1.45}.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:22px;box-shadow:0 12px 34px rgba(15,37,51,.08)}label{display:block;margin:14px 0 6px;font-weight:700}input{width:100%;min-height:44px;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:#fbfdfe;color:var(--text);font:inherit}button{width:100%;min-height:42px;margin-top:12px;padding:0 16px;border:0;border-radius:8px;background:var(--primary);color:#fff;font:inherit;font-weight:700}button:hover{background:var(--primary-dark)}.secondary{border:1px solid var(--line);background:var(--soft);color:var(--primary-dark)}.secondary:hover{background:#e8f1f5}.section-title{margin:26px 0 8px;padding-top:18px;border-top:1px solid var(--line);font-size:18px}.hint{margin:0 0 10px;color:var(--muted);font-size:13px;line-height:1.45}.banner,.mini-status{margin-top:14px;padding:12px 14px;border-radius:8px;font-size:14px;white-space:pre-line}.banner{display:none}.banner.show{display:block}.ok{background:var(--ok-bg);color:var(--ok-text)}.error{background:var(--error-bg);color:var(--error-text)}.mini-status{border:1px solid var(--line);background:var(--soft);color:var(--muted)}nav{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:18px;padding-bottom:12px;border-bottom:1px solid var(--line)}nav a{color:var(--primary-dark);font-weight:700;text-decoration:none}@media (max-width:640px){.shell{width:min(100% - 24px,760px);padding:16px 0}}"
"</style>"
"</head>"
"<body>"
"<main class='shell'>"
"<nav><a href='/'>Inicio</a><a href='/status'>Status</a><a href='/networks'>Redes</a><a href='/ota'>OTA</a><a href='/settings'>Ajustes</a><a href='/logs'>Logs</a></nav>"
"<header><h1>Atualizacao OTA</h1><p class='status'>Upload direto do .bin para o dispositivo, sem URL remota.</p></header>"
"<section class='panel'>"
"<p class='hint'>Selecione um firmware .bin gerado pelo build atual e envie diretamente para a particao OTA.</p>"
"<label for='ota_bin'>Arquivo de firmware (.bin)</label><input id='ota_bin' type='file' accept='.bin,application/octet-stream'>"
"<button type='button' onclick='uploadOtaBin()'>Enviar .bin e atualizar</button>"
"<button class='secondary' type='button' onclick='refreshOtaStatus()'>Atualizar status</button>"
"<div id='ota_status' class='mini-status'>Status OTA: --</div>"
"<div id='banner' class='banner'></div>"
"</section>"
"</main>"
"<script>"
"function showBanner(kind,text){const el=document.getElementById('banner');el.className='banner show '+kind;el.textContent=text;}"
"function renderOtaStatus(status){const lines=['Estado: '+(status.state||'--'),'Versao atual: '+(status.current_version||'--'),'Particao atual: '+(status.running_partition||'--'),'Proxima particao OTA: '+(status.next_update_partition||'--')];if(status.last_error){lines.push('Erro: '+status.last_error);}if(status.restart_pending){lines.push('Reboot pendente apos gravacao bem-sucedida.');}document.getElementById('ota_status').textContent=lines.join('\\n');}"
"async function refreshOtaStatus(){try{const resp=await fetch('/api/ota/status',{cache:'no-store'});const status=await resp.json();renderOtaStatus(status);}catch(err){document.getElementById('ota_status').textContent='Falha ao carregar status OTA.';}}"
"async function uploadOtaBin(){const input=document.getElementById('ota_bin');const file=input.files&&input.files[0]?input.files[0]:null;if(!file){showBanner('error','Selecione um arquivo .bin.');return;}if(!file.name.toLowerCase().endsWith('.bin')){showBanner('error','Arquivo invalido: use um .bin.');return;}showBanner('ok','Upload em andamento...');const resp=await fetch('/api/ota/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});const text=await resp.text();showBanner(resp.ok?'ok':'error',text);await refreshOtaStatus();}"
"refreshOtaStatus();"
"</script>"
"</body>"
"</html>";

static bool callbacks_ready(void)
{
    return s_config.begin &&
           s_config.write &&
           s_config.finalize &&
           s_config.abort &&
           s_config.status;
}

static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, OTA_PAGE_HTML);
}

static esp_err_t api_ota_status_get_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_config.status) {
        err = s_config.status(json, s_config.ctx);
    }
    if (err != ESP_OK) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Status OTA indisponivel");
    }

    err = http_helpers_send_json(req, json, 200);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_ota_upload_post_handler(httpd_req_t *req)
{
    uint8_t buffer[1024];
    int remaining = req->content_len;

    if (remaining <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    if (!callbacks_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "OTA nao configurado");
    }

    esp_err_t err = s_config.begin(s_config.ctx);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "OTA ja esta em andamento");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Falha interna ao processar solicitacao");
    }

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        int received = httpd_req_recv(req, (char *)buffer, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            s_config.abort(s_config.ctx);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "Falha durante upload do firmware");
        }

        err = s_config.write(buffer, (size_t)received, s_config.ctx);
        if (err != ESP_OK) {
            s_config.abort(s_config.ctx);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "Falha ao gravar particao OTA");
        }

        remaining -= received;
    }

    err = s_config.finalize(s_config.ctx);
    if (err != ESP_OK) {
        s_config.abort(s_config.ctx);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Falha ao finalizar imagem OTA");
    }

    tele_portal_core_request_restart(s_config.restart_delay_ms > 0 ?
                                     s_config.restart_delay_ms :
                                     TELE_PORTAL_OTA_DEFAULT_RESTART_DELAY_MS);

    return httpd_resp_sendstr(req, "Upload concluido. Reiniciando para ativar o novo firmware.");
}

static esp_err_t register_ota_routes(httpd_handle_t server)
{
    httpd_uri_t page = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_page_get_handler,
    };
    httpd_uri_t api_status = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = api_ota_status_get_handler,
    };
    httpd_uri_t api_upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = api_ota_upload_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &page);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_status);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_upload);
    }

    return err;
}

esp_err_t tele_portal_ota_init(const tele_portal_ota_config_t *config)
{
    if (!config || !config->begin || !config->write || !config->finalize ||
        !config->abort || !config->status) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    return ESP_OK;
}

esp_err_t tele_portal_ota_register_routes(void)
{
    return tele_portal_core_register_routes(register_ota_routes);
}
