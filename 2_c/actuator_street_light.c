#include "smartcity_common.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool active;
    double brightness;
    CRITICAL_SECTION lock;
} StreetState;

static StreetState g_state;

static void build_state_text(char *text, size_t size) {
    snprintf(text, size, "%s; brilho=%.0f%%",
             g_state.active ? "ligado" : "desligado",
             g_state.brightness);
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

        resp.sensor_id = smart_dup_cstring("poste-1");
        resp.sensor_type = STREET_LIGHT;
        resp.sensor_ip = local_ip ? local_ip : smart_dup_cstring("127.0.0.1");
        resp.control_tcp_port = 9203;
        EnterCriticalSection(&g_state.lock);
        resp.is_active = g_state.active;
        resp.device_kind = ACTUATOR;
        {
            char state_buf[128];
            build_state_text(state_buf, sizeof(state_buf));
            resp.state_text = smart_dup_cstring(state_buf);
        }
        LeaveCriticalSection(&g_state.lock);

        if (smart_encode_discovery_response(&resp, &payload)) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons((u_short)response_port);
            inet_pton(AF_INET, gw_ip ? gw_ip : "127.0.0.1", &to.sin_addr);
            sendto(sock, (const char *)payload.data, (int)payload.len, 0, (struct sockaddr *)&to, sizeof(to));
            smart_buffer_free(&payload);
            printf("[StreetLight] Discovery response sent to %s:%u state=%s\n", gw_ip ? gw_ip : "127.0.0.1", response_port, resp.state_text ? resp.state_text : "");
        } else {
            smart_buffer_free(&payload);
        }
        free(gw_ip);
        smart_free_discovery_request(&req);
        smart_free_discovery_response(&resp);
    }
}

static ControlResponse handle_command(const ControlCommand *cmd) {
    ControlResponse resp;
    memset(&resp, 0, sizeof(resp));
    EnterCriticalSection(&g_state.lock);
    if (cmd->command_type == ACTIVATE) {
        g_state.active = true;
        resp.status = OK;
        resp.message = smart_dup_cstring("Poste ligado");
    } else if (cmd->command_type == DEACTIVATE) {
        g_state.active = false;
        resp.status = OK;
        resp.message = smart_dup_cstring("Poste desligado");
    } else if (cmd->command_type == STREET_LIGHT_SET_BRIGHTNESS) {
        if (cmd->value < 0 || cmd->value > 100) {
            resp.status = ERROR;
            resp.message = smart_dup_cstring("Erro no comando: Brilho deve estar entre 0 e 100");
        } else {
            g_state.brightness = cmd->value;
            resp.status = OK;
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "Brilho alterado para %.0f%%", g_state.brightness);
                resp.message = smart_dup_cstring(buf);
            }
        }
    } else {
        resp.status = ERROR;
        resp.message = smart_dup_cstring("Erro no comando: Comando invalido para poste");
    }
    resp.sensor_id = smart_dup_cstring("poste-1");
    resp.is_active = g_state.active;
    {
        char state_buf[128];
        build_state_text(state_buf, sizeof(state_buf));
        resp.state_text = smart_dup_cstring(state_buf);
    }
    LeaveCriticalSection(&g_state.lock);
    return resp;
}

static DWORD WINAPI control_thread(LPVOID param) {
    (void)param;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    BOOL reuse = TRUE;

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9203);
    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, SOMAXCONN);
    printf("[StreetLight] control TCP on 0.0.0.0:9203\n");

    for (;;) {
        SOCKET conn = accept(server, NULL, NULL);
        if (conn == INVALID_SOCKET) {
            continue;
        }

        {
            uint8_t *raw = NULL;
            uint32_t raw_len = 0;
            ControlCommand cmd;
            ControlResponse resp;
            SmartBuffer payload;

            if (!smart_recv_msg(conn, &raw, &raw_len)) {
                closesocket(conn);
                continue;
            }
            memset(&cmd, 0, sizeof(cmd));
            if (!smart_parse_control_command(&cmd, raw, raw_len)) {
                free(raw);
                closesocket(conn);
                continue;
            }
            free(raw);
            printf("[StreetLight] TCP command received command=%d value=%.2f text=%s\n",
                   cmd.command_type, cmd.value, cmd.text_value ? cmd.text_value : "");
            resp = handle_command(&cmd);
            smart_free_control_command(&cmd);
            if (smart_encode_control_response(&resp, &payload)) {
                smart_send_msg(conn, payload.data, (uint32_t)payload.len);
                smart_buffer_free(&payload);
            } else {
                smart_buffer_free(&payload);
            }
            smart_free_control_response(&resp);
        }

        closesocket(conn);
    }
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.active = true;
    g_state.brightness = 70.0;
    InitializeCriticalSection(&g_state.lock);
    CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
    control_thread(NULL);
    DeleteCriticalSection(&g_state.lock);
    WSACleanup();
    return 0;
}
