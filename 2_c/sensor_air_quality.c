#include "smartcity_common.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char gateway_ip[64];
    uint32_t gateway_udp_port;
    CRITICAL_SECTION lock;
} AirState;

static AirState g_state;

static double round2(double value) {
    return ((double)((int)(value * 100.0 + (value >= 0 ? 0.5 : -0.5)))) / 100.0;
}

static DWORD WINAPI discovery_thread(LPVOID param) {
    (void)param;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in bind_addr;
    struct ip_mreq mreq;
    char buffer[65535];
    BOOL reuse = TRUE;

    memset(&bind_addr, 0, sizeof(bind_addr));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(SMARTCITY_MULTICAST_PORT);
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    mreq.imr_multiaddr.s_addr = inet_addr(SMARTCITY_MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));

    for (;;) {
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &from_len);
        if (received <= 0) {
            continue;
        }

        DiscoveryRequest req;
        DiscoveryResponse resp;
        SmartBuffer payload;
        char *gw_ip;
        char *local_ip;
        uint32_t response_port;

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        if (!smart_parse_discovery_request(&req, (const uint8_t *)buffer, (size_t)received)) {
            smart_free_discovery_request(&req);
            continue;
        }

        gw_ip = smart_dup_cstring(inet_ntoa(from.sin_addr));
        response_port = req.discovery_response_port ? req.discovery_response_port : SMARTCITY_DISCOVERY_RESPONSE_PORT;
        local_ip = smart_get_local_ip(gw_ip ? gw_ip : "127.0.0.1");

        EnterCriticalSection(&g_state.lock);
        if (gw_ip) {
            strncpy(g_state.gateway_ip, gw_ip, sizeof(g_state.gateway_ip) - 1);
            g_state.gateway_ip[sizeof(g_state.gateway_ip) - 1] = '\0';
        }
        g_state.gateway_udp_port = req.gateway_udp_port ? req.gateway_udp_port : SMARTCITY_GATEWAY_UDP_PORT;
        LeaveCriticalSection(&g_state.lock);

        resp.sensor_id = smart_dup_cstring("air-1");
        resp.sensor_type = AIR_QUALITY_SENSOR;
        resp.sensor_ip = local_ip ? local_ip : smart_dup_cstring("127.0.0.1");
        resp.control_tcp_port = 0;
        resp.is_active = true;
        resp.frequency_seconds = 2.0;
        resp.threshold = 900.0;
        resp.device_kind = SENSOR;
        resp.state_text = smart_dup_cstring("CO2 e umidade via UDP");

        if (smart_encode_discovery_response(&resp, &payload)) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons((u_short)response_port);
            inet_pton(AF_INET, gw_ip ? gw_ip : "127.0.0.1", &to.sin_addr);
            sendto(sock, (const char *)payload.data, (int)payload.len, 0, (struct sockaddr *)&to, sizeof(to));
            smart_buffer_free(&payload);
            printf("[Air] Discovery response sent to %s:%u\n", gw_ip ? gw_ip : "127.0.0.1", response_port);
        } else {
            smart_buffer_free(&payload);
        }
        free(gw_ip);
        smart_free_discovery_request(&req);
    }
}

static SensorReading make_reading(const char *metric, double value, const char *unit, bool alert, const char *alert_message) {
    SensorReading reading;
    memset(&reading, 0, sizeof(reading));
    reading.sensor_id = "air-1";
    reading.sensor_type = AIR_QUALITY_SENSOR;
    reading.value = value;
    reading.unit = (char *)unit;
    reading.timestamp_unix_ms = smart_unix_time_ms();
    reading.alert = alert;
    reading.alert_message = (char *)(alert_message ? alert_message : "");
    reading.metric = (char *)metric;
    return reading;
}

static void send_readings_loop(void) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    srand((unsigned)time(NULL));

    for (;;) {
        char gw_ip[64];
        uint32_t gw_port;

        EnterCriticalSection(&g_state.lock);
        strncpy(gw_ip, g_state.gateway_ip, sizeof(gw_ip) - 1);
        gw_ip[sizeof(gw_ip) - 1] = '\0';
        gw_port = g_state.gateway_udp_port;
        LeaveCriticalSection(&g_state.lock);

        if (gw_ip[0] == '\0' || gw_port == 0) {
            Sleep(1000);
            continue;
        }

        {
            double co2 = round2(400.0 + ((double)rand() / (double)RAND_MAX) * 800.0);
            double humidity = round2(35.0 + ((double)rand() / (double)RAND_MAX) * 50.0);
            bool alert = co2 > 900.0;
            char alert_msg[128];
            SensorReading readings[2];
            int i;

            snprintf(alert_msg, sizeof(alert_msg), "CO2 acima do limiar (900.00)");
            readings[0] = make_reading("co2", co2, "ppm", alert, alert ? alert_msg : "");
            readings[1] = make_reading("humidity", humidity, "%", false, "");

            for (i = 0; i < 2; ++i) {
                SmartBuffer payload;
                struct sockaddr_in to;
                if (smart_encode_sensor_reading(&readings[i], &payload)) {
                    memset(&to, 0, sizeof(to));
                    to.sin_family = AF_INET;
                    to.sin_port = htons((u_short)gw_port);
                    inet_pton(AF_INET, gw_ip, &to.sin_addr);
                    sendto(sock, (const char *)payload.data, (int)payload.len, 0, (struct sockaddr *)&to, sizeof(to));
                    smart_buffer_free(&payload);
                } else {
                    smart_buffer_free(&payload);
                }
            }

            printf("[Air] CO2=%.2f ppm humidity=%.2f%%%s\n", co2, humidity, alert ? " ALERTA" : "");
        }

        Sleep(2000);
    }
}

int main(void) {
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }

    memset(&g_state, 0, sizeof(g_state));
    InitializeCriticalSection(&g_state.lock);
    CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
    send_readings_loop();

    DeleteCriticalSection(&g_state.lock);
    WSACleanup();
    return 0;
}
