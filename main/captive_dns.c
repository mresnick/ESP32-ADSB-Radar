#include "captive_dns.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";

#define DNS_PORT 53
#define AP_IP_A 192
#define AP_IP_B 168
#define AP_IP_C 4
#define AP_IP_D 1

// Rewrites a received DNS query packet in-place into a response that answers
// every question with a single A record pointing at the SoftAP's own IP.
// Not a general resolver: only handles a single question, ignores QTYPE, and
// assumes the query fits the buffer with room for one more 16-byte answer.
static int build_response(uint8_t *packet, int query_len, int buf_size)
{
    if (query_len < 12 || query_len + 16 > buf_size) {
        return -1;
    }

    // Flags: QR=1 (response), opcode preserved, AA=1, RA=0, RCODE=0.
    packet[2] = 0x84;
    packet[3] = 0x00;

    // ANCOUNT = 1
    packet[6] = 0x00;
    packet[7] = 0x01;

    uint8_t *answer = packet + query_len;
    answer[0] = 0xC0;
    answer[1] = 0x0C; // name = pointer to question name at offset 12
    answer[2] = 0x00;
    answer[3] = 0x01; // TYPE = A
    answer[4] = 0x00;
    answer[5] = 0x01; // CLASS = IN
    answer[6] = 0x00;
    answer[7] = 0x00;
    answer[8] = 0x00;
    answer[9] = 0x3C; // TTL = 60s
    answer[10] = 0x00;
    answer[11] = 0x04; // RDLENGTH = 4
    answer[12] = AP_IP_A;
    answer[13] = AP_IP_B;
    answer[14] = AP_IP_C;
    answer[15] = AP_IP_D;

    return query_len + 16;
}

static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive DNS responder listening on UDP:%d", DNS_PORT);

    uint8_t packet[512];
    while (1) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        int recv_len = recvfrom(sock, packet, sizeof(packet), 0,
                                 (struct sockaddr *)&src_addr, &src_len);
        if (recv_len <= 0) {
            continue;
        }

        int resp_len = build_response(packet, recv_len, sizeof(packet));
        if (resp_len < 0) {
            continue;
        }

        sendto(sock, packet, resp_len, 0, (struct sockaddr *)&src_addr, src_len);
    }
}

void captive_dns_start(void)
{
    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 5, NULL);
}
