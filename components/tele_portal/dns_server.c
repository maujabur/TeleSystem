#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_log.h"

#include "dns_server.h"

#define DNS_PORT 53
#define DNS_MAX_PACKET_SIZE 512
#define DNS_TASK_STACK_SIZE 4096

static const char *TAG = "dns-server";

static TaskHandle_t s_dns_task;
static int s_dns_socket = -1;
static volatile bool s_dns_running;
static esp_ip4_addr_t s_reply_ip;

static bool parse_dns_question(const uint8_t *request,
                               size_t request_len,
                               uint16_t *question_count,
                               size_t *question_end,
                               uint16_t *question_type,
                               char *question_name,
                               size_t question_name_size)
{
    size_t offset = 12;
    size_t name_offset = 0;

    if (!request || request_len < 17 || !question_count || !question_end || !question_type) {
        return false;
    }

    *question_count = (uint16_t)((request[4] << 8) | request[5]);
    if (*question_count == 0) {
        return false;
    }

    while (offset < request_len && request[offset] != 0) {
        uint8_t label_len = request[offset];

        if ((label_len & 0xC0) != 0 || offset + 1 + label_len > request_len) {
            return false;
        }

        if (question_name && question_name_size > 0) {
            if (name_offset != 0 && name_offset < question_name_size - 1) {
                question_name[name_offset++] = '.';
            }

            for (size_t i = 0; i < label_len && name_offset < question_name_size - 1; ++i) {
                question_name[name_offset++] = (char)request[offset + 1 + i];
            }
        }

        offset += (size_t)label_len + 1;
    }

    if (offset + 5 > request_len) {
        return false;
    }

    if (question_name && question_name_size > 0) {
        question_name[name_offset] = '\0';
    }

    offset++;
    *question_type = (uint16_t)((request[offset] << 8) | request[offset + 1]);
    *question_end = offset + 4;
    return true;
}

static size_t build_dns_response(const uint8_t *request,
                                 size_t request_len,
                                 uint8_t *response,
                                 size_t response_size,
                                 uint16_t question_count,
                                 uint16_t question_type,
                                 size_t question_end,
                                 bool *answered)
{
    size_t answer_offset = 0;

    if (!request || !response || !answered || request_len < 12 || response_size < question_end + 16) {
        return 0;
    }

    if (question_count == 0 || question_end > request_len) {
        return 0;
    }

    memcpy(response, request, question_end);
    response[2] = 0x81;
    response[3] = 0x80;
    response[4] = (uint8_t)(question_count >> 8);
    response[5] = (uint8_t)(question_count & 0xFF);
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    if (question_type != 0x0001) {
        response[6] = 0x00;
        response[7] = 0x00;
        *answered = false;
        return question_end;
    }

    response[6] = 0x00;
    response[7] = 0x01;

    answer_offset = question_end;
    response[answer_offset++] = 0xC0;
    response[answer_offset++] = 0x0C;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x01;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x01;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x3C;
    response[answer_offset++] = 0x00;
    response[answer_offset++] = 0x04;
    response[answer_offset++] = esp_ip4_addr1(&s_reply_ip);
    response[answer_offset++] = esp_ip4_addr2(&s_reply_ip);
    response[answer_offset++] = esp_ip4_addr3(&s_reply_ip);
    response[answer_offset++] = esp_ip4_addr4(&s_reply_ip);

    *answered = true;
    return answer_offset;
}

static void dns_server_task(void *arg)
{
    struct sockaddr_in server_addr = {0};
    struct sockaddr_in client_addr = {0};
    socklen_t client_len = sizeof(client_addr);
    uint8_t request[DNS_MAX_PACKET_SIZE];
    uint8_t response[DNS_MAX_PACKET_SIZE];

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Nao foi possivel criar socket DNS");
        s_dns_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Nao foi possivel abrir porta DNS");
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Servidor DNS iniciado");

    while (s_dns_running) {
        fd_set read_fds;
        struct timeval timeout = {
            .tv_sec = 1,
            .tv_usec = 0,
        };
        char question_name[128] = {0};
        char client_ip[16] = {0};
        uint16_t question_count = 0;
        uint16_t question_type = 0;
        size_t question_end = 0;
        bool answered = false;

        FD_ZERO(&read_fds);
        FD_SET(s_dns_socket, &read_fds);

        int ready = select(s_dns_socket + 1, &read_fds, NULL, NULL, &timeout);
        if (ready <= 0 || !FD_ISSET(s_dns_socket, &read_fds)) {
            continue;
        }

        int received = recvfrom(s_dns_socket,
                                request,
                                sizeof(request),
                                0,
                                (struct sockaddr *)&client_addr,
                                &client_len);
        if (received <= 0) {
            continue;
        }

        client_len = sizeof(client_addr);
        inet_ntoa_r(client_addr.sin_addr.s_addr, client_ip, sizeof(client_ip));

        if (!parse_dns_question(request,
                                (size_t)received,
                                &question_count,
                                &question_end,
                                &question_type,
                                question_name,
                                sizeof(question_name))) {
            uint16_t flags = (uint16_t)((request[2] << 8) | request[3]);
            uint16_t arcount = (uint16_t)((request[10] << 8) | request[11]);
            ESP_LOGW(TAG,
                     "Consulta DNS invalida de %s (len=%d flags=0x%04x qd=%u ar=%u)",
                     client_ip,
                     received,
                     flags,
                     (unsigned)((request[4] << 8) | request[5]),
                     (unsigned)arcount);
            continue;
        }

        size_t response_len = build_dns_response(request,
                                                 (size_t)received,
                                                 response,
                                                 sizeof(response),
                                                 question_count,
                                                 question_type,
                                                 question_end,
                                                 &answered);
        if (response_len == 0) {
            ESP_LOGW(TAG,
                     "Falha ao montar resposta DNS para %s (tipo=0x%04x, nome=%s)",
                     client_ip,
                     question_type,
                     question_name[0] ? question_name : "<vazio>");
            continue;
        }

        ESP_LOGI(TAG,
             "DNS %s perguntou por %s (tipo=0x%04x) -> %u.%u.%u.%u",
             client_ip,
             question_name[0] ? question_name : "<vazio>",
             question_type,
             answered ? (unsigned)esp_ip4_addr1(&s_reply_ip) : 0,
             answered ? (unsigned)esp_ip4_addr2(&s_reply_ip) : 0,
             answered ? (unsigned)esp_ip4_addr3(&s_reply_ip) : 0,
             answered ? (unsigned)esp_ip4_addr4(&s_reply_ip) : 0);

        sendto(s_dns_socket,
               response,
               response_len,
               0,
               (struct sockaddr *)&client_addr,
               client_len);
    }

    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }

    ESP_LOGI(TAG, "Servidor DNS parado");
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(esp_ip4_addr_t ip)
{
    if (s_dns_task) {
        s_reply_ip = ip;
        s_dns_running = true;
        return ESP_OK;
    }

    s_reply_ip = ip;
    s_dns_running = true;

    BaseType_t task_ok = xTaskCreate(dns_server_task,
                                     "dns_server",
                                     DNS_TASK_STACK_SIZE,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_dns_task);
    if (task_ok != pdPASS) {
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t dns_server_stop(void)
{
    if (!s_dns_task) {
        return ESP_OK;
    }

    s_dns_running = false;
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, 0);
    }

    return ESP_OK;
}
