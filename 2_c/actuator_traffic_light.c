#include "smartcity_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool active;
    char color[32];
    char mode[32];
    CRITICAL_SECTION lock;
} TrafficState;

static TrafficState g_state;

static void build_state_text(char *text, size_t size) {
    snprintf(text, size, "%s; cor=%s; modo=%s",
             g_state.active ? "ligado" : "desligado",
             g_state.color,
             g_state.mode);
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

        resp.sensor_id = smart_dup_cstring("semaforo-1");
        resp.sensor_type = TRAFFIC_LIGHT;
        resp.sensor_ip = local_ip ? local_ip : smart_dup_cstring("127.0.0.1");
        resp.control_tcp_port = 9202;
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
            printf("[TrafficLight] Discovery response sent to %s:%u state=%s\n", gw_ip ? gw_ip : "127.0.0.1", response_port, resp.state_text ? resp.state_text : "");
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
        resp.message = smart_dup_cstring("Semaforo ligado");
    } else if (cmd->command_type == DEACTIVATE) {
        g_state.active = false;
        resp.status = OK;
        resp.message = smart_dup_cstring("Semaforo desligado");
    } else if (cmd->command_type == TRAFFIC_LIGHT_SET_COLOR) {
        const char *color = cmd->text_value ? cmd->text_value : "";
        if (strcmp(color, "verde") != 0 && strcmp(color, "amarelo") != 0 && strcmp(color, "vermelho") != 0) {
            resp.status = ERROR;
            resp.message = smart_dup_cstring("Erro no comando: Cor deve ser verde, amarelo ou vermelho");
        } else {
            strncpy(g_state.color, color, sizeof(g_state.color) - 1);
            g_state.color[sizeof(g_state.color) - 1] = '\0';
            strcpy(g_state.mode, "manual");
            resp.status = OK;
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "Cor alterada para %s", g_state.color);
                resp.message = smart_dup_cstring(buf);
            }
        }
    } else if (cmd->command_type == TRAFFIC_LIGHT_SET_MODE) {
        const char *mode = cmd->text_value ? cmd->text_value : "";
        if (strcmp(mode, "automatico") != 0 && strcmp(mode, "manual") != 0) {
            resp.status = ERROR;
            resp.message = smart_dup_cstring("Erro no comando: Modo deve ser automatico ou manual");
        } else {
            strncpy(g_state.mode, mode, sizeof(g_state.mode) - 1);
            g_state.mode[sizeof(g_state.mode) - 1] = '\0';
            resp.status = OK;
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "Modo alterado para %s", g_state.mode);
                resp.message = smart_dup_cstring(buf);
            }
        }
    } else {
        resp.status = ERROR;
        resp.message = smart_dup_cstring("Erro no comando: Comando invalido para semaforo");
    }
    resp.sensor_id = smart_dup_cstring("semaforo-1");
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
    addr.sin_port = htons(9202);
    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, SOMAXCONN);
    printf("[TrafficLight] control TCP on 0.0.0.0:9202\n");

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
            printf("[TrafficLight] TCP command received command=%d value=%.2f text=%s\n",
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
    strcpy(g_state.color, "vermelho");
    strcpy(g_state.mode, "automatico");
    g_state.active = true;
    InitializeCriticalSection(&g_state.lock);
    CreateThread(NULL, 0, discovery_thread, NULL, 0, NULL);
    control_thread(NULL);
    DeleteCriticalSection(&g_state.lock);
    WSACleanup();
    return 0;
}
