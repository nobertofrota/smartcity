#include "smartcity_common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DiscoveryResponse info;
    double last_seen;
} SensorRecord;

typedef struct {
    SensorRecord *sensors;
    size_t sensor_count;
    SensorReading *readings;
    size_t reading_count;
    CRITICAL_SECTION lock;
} GatewayState;

#define CSV_ROOT_DIR "data\\csv"

static double now_seconds(void) {
    return smart_now_seconds();
}

static void iso_timestamp(char *buf, size_t size) {
    smart_iso_utc_timestamp(buf, size);
}

static void free_sensor_record(SensorRecord *record) {
    smart_free_discovery_response(&record->info);
    record->last_seen = 0.0;
}

static void ensure_csv_dirs(void) {
    CreateDirectoryA("data", NULL);
    CreateDirectoryA(CSV_ROOT_DIR, NULL);
}

static void safe_filename(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    size_t j = 0;
    if (!src || dst_size == 0) {
        if (dst_size > 0) {
            dst[0] = '\0';
        }
        return;
    }
    while (src[i] != '\0' && j + 1 < dst_size) {
        char c = src[i++];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';
    if (j == 0 && dst_size > 0) {
        strncpy(dst, "unknown", dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static char *csv_quote_string(const char *src) {
    size_t len = src ? strlen(src) : 0;
    size_t max_len = len * 2 + 3;
    char *out = (char *)malloc(max_len);
    size_t i = 0;
    size_t j = 0;

    if (!out) {
        return NULL;
    }

    out[j++] = '"';
    for (i = 0; i < len; ++i) {
        char c = src[i];
        if (c == '"') {
            out[j++] = '"';
        }
        out[j++] = c;
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static void persist_reading_csv(const SensorReading *reading) {
    char safe_id[128];
    char path[MAX_PATH];
    FILE *fp;
    DWORD attrs;
    bool write_header;
    char *sensor_id_q = NULL;
    char *metric_q = NULL;
    char *unit_q = NULL;
    char *alert_q = NULL;

    if (!reading || !reading->sensor_id) {
        return;
    }

    ensure_csv_dirs();
    safe_filename(reading->sensor_id, safe_id, sizeof(safe_id));
    snprintf(path, sizeof(path), "%s\\%s.csv", CSV_ROOT_DIR, safe_id);
    attrs = GetFileAttributesA(path);
    write_header = (attrs == INVALID_FILE_ATTRIBUTES) || (attrs & FILE_ATTRIBUTE_DIRECTORY);

    fp = fopen(path, "a");
    if (!fp) {
        return;
    }

    sensor_id_q = csv_quote_string(reading->sensor_id ? reading->sensor_id : "");
    metric_q = csv_quote_string(reading->metric ? reading->metric : "");
    unit_q = csv_quote_string(reading->unit ? reading->unit : "");
    alert_q = csv_quote_string(reading->alert_message ? reading->alert_message : "");

    if (!sensor_id_q || !metric_q || !unit_q || !alert_q) {
        free(sensor_id_q);
        free(metric_q);
        free(unit_q);
        free(alert_q);
        fclose(fp);
        return;
    }

    if (write_header) {
        fprintf(fp, "sensor_id,sensor_type,metric,value,unit,timestamp_unix_ms,alert,alert_message\n");
    }

    fprintf(fp, "%s,%d,%s,%.15g,%s,%" PRId64 ",%d,%s\n",
            sensor_id_q,
            (int)reading->sensor_type,
            metric_q,
            reading->value,
            unit_q,
            reading->timestamp_unix_ms,
            reading->alert ? 1 : 0,
            alert_q);
    free(sensor_id_q);
    free(metric_q);
    free(unit_q);
    free(alert_q);
    fclose(fp);
}

static SensorRecord *find_sensor(GatewayState *state, const char *sensor_id) {
    size_t i;
    if (!sensor_id) {
        return NULL;
    }
    for (i = 0; i < state->sensor_count; ++i) {
        if (state->sensors[i].info.sensor_id && strcmp(state->sensors[i].info.sensor_id, sensor_id) == 0) {
            return &state->sensors[i];
        }
    }
    return NULL;
}

static bool append_sensor_copy(GatewayState *state, const DiscoveryResponse *src) {
    SensorRecord *new_items = (SensorRecord *)realloc(state->sensors, (state->sensor_count + 1) * sizeof(SensorRecord));
    if (!new_items) {
        return false;
    }
    state->sensors = new_items;
    memset(&state->sensors[state->sensor_count], 0, sizeof(SensorRecord));
    state->sensors[state->sensor_count].info.sensor_id = smart_dup_cstring(src->sensor_id);
    state->sensors[state->sensor_count].info.sensor_type = src->sensor_type;
    state->sensors[state->sensor_count].info.sensor_ip = smart_dup_cstring(src->sensor_ip);
    state->sensors[state->sensor_count].info.control_tcp_port = src->control_tcp_port;
    state->sensors[state->sensor_count].info.is_active = src->is_active;
    state->sensors[state->sensor_count].info.frequency_seconds = src->frequency_seconds;
    state->sensors[state->sensor_count].info.threshold = src->threshold;
    state->sensors[state->sensor_count].info.device_kind = src->device_kind;
    state->sensors[state->sensor_count].info.state_text = smart_dup_cstring(src->state_text);
    if ((src->sensor_id && !state->sensors[state->sensor_count].info.sensor_id) ||
        (src->sensor_ip && !state->sensors[state->sensor_count].info.sensor_ip) ||
        (src->state_text && !state->sensors[state->sensor_count].info.state_text)) {
        free_sensor_record(&state->sensors[state->sensor_count]);
        return false;
    }
    state->sensors[state->sensor_count].last_seen = now_seconds();
    state->sensor_count += 1;
    return true;
}

static bool append_reading_copy(GatewayState *state, const SensorReading *src) {
    SensorReading *new_items = (SensorReading *)realloc(state->readings, (state->reading_count + 1) * sizeof(SensorReading));
    if (!new_items) {
        return false;
    }
    state->readings = new_items;
    memset(&state->readings[state->reading_count], 0, sizeof(SensorReading));
    state->readings[state->reading_count].sensor_id = smart_dup_cstring(src->sensor_id);
    state->readings[state->reading_count].sensor_type = src->sensor_type;
    state->readings[state->reading_count].value = src->value;
    state->readings[state->reading_count].unit = smart_dup_cstring(src->unit);
    state->readings[state->reading_count].timestamp_unix_ms = src->timestamp_unix_ms;
    state->readings[state->reading_count].alert = src->alert;
    state->readings[state->reading_count].alert_message = smart_dup_cstring(src->alert_message);
    state->readings[state->reading_count].metric = smart_dup_cstring(src->metric);
    if ((src->sensor_id && !state->readings[state->reading_count].sensor_id) ||
        (src->unit && !state->readings[state->reading_count].unit) ||
        (src->alert_message && !state->readings[state->reading_count].alert_message) ||
        (src->metric && !state->readings[state->reading_count].metric)) {
        smart_free_sensor_reading(&state->readings[state->reading_count]);
        return false;
    }
    state->reading_count += 1;
    return true;
}

static void update_sensor(GatewayState *state, const DiscoveryResponse *discovery) {
    EnterCriticalSection(&state->lock);
    SensorRecord *record = find_sensor(state, discovery->sensor_id);
    if (record) {
        free_sensor_record(record);
        memset(record, 0, sizeof(*record));
        record->info.sensor_id = smart_dup_cstring(discovery->sensor_id);
        record->info.sensor_type = discovery->sensor_type;
        record->info.sensor_ip = smart_dup_cstring(discovery->sensor_ip);
        record->info.control_tcp_port = discovery->control_tcp_port;
        record->info.is_active = discovery->is_active;
        record->info.frequency_seconds = discovery->frequency_seconds;
        record->info.threshold = discovery->threshold;
        record->info.device_kind = discovery->device_kind;
        record->info.state_text = smart_dup_cstring(discovery->state_text);
        record->last_seen = now_seconds();
    } else {
        append_sensor_copy(state, discovery);
    }
    LeaveCriticalSection(&state->lock);
}

static void update_last_seen_for_reading(GatewayState *state, const char *sensor_id) {
    size_t i;
    if (!sensor_id) {
        return;
    }
    for (i = 0; i < state->sensor_count; ++i) {
        if (state->sensors[i].info.sensor_id && strcmp(state->sensors[i].info.sensor_id, sensor_id) == 0) {
            state->sensors[i].last_seen = now_seconds();
            return;
        }
    }
}

static double compute_average_locked(GatewayState *state, const char *metric, bool *has_values) {
    double sum = 0.0;
    size_t count = 0;
    size_t i;
    for (i = 0; i < state->reading_count; ++i) {
        if (state->readings[i].metric && strcmp(state->readings[i].metric, metric) == 0) {
            sum += state->readings[i].value;
            count += 1;
        }
    }
    *has_values = count > 0;
    return count ? (sum / (double)count) : 0.0;
}

static const SensorReading *compute_max_locked(GatewayState *state) {
    size_t i;
    const SensorReading *best = NULL;
    for (i = 0; i < state->reading_count; ++i) {
        if (!best || state->readings[i].value > best->value) {
            best = &state->readings[i];
        }
    }
    return best;
}

static bool append_reading_to_response(ClientResponse *resp, const SensorReading *reading) {
    SensorReading *new_items = (SensorReading *)realloc(resp->readings, (resp->reading_count + 1) * sizeof(SensorReading));
    if (!new_items) {
        return false;
    }
    resp->readings = new_items;
    memset(&resp->readings[resp->reading_count], 0, sizeof(SensorReading));
    resp->readings[resp->reading_count].sensor_id = smart_dup_cstring(reading->sensor_id);
    resp->readings[resp->reading_count].sensor_type = reading->sensor_type;
    resp->readings[resp->reading_count].value = reading->value;
    resp->readings[resp->reading_count].unit = smart_dup_cstring(reading->unit);
    resp->readings[resp->reading_count].timestamp_unix_ms = reading->timestamp_unix_ms;
    resp->readings[resp->reading_count].alert = reading->alert;
    resp->readings[resp->reading_count].alert_message = smart_dup_cstring(reading->alert_message);
    resp->readings[resp->reading_count].metric = smart_dup_cstring(reading->metric);
    if ((reading->sensor_id && !resp->readings[resp->reading_count].sensor_id) ||
        (reading->unit && !resp->readings[resp->reading_count].unit) ||
        (reading->alert_message && !resp->readings[resp->reading_count].alert_message) ||
        (reading->metric && !resp->readings[resp->reading_count].metric)) {
        smart_free_sensor_reading(&resp->readings[resp->reading_count]);
        return false;
    }
    resp->reading_count += 1;
    return true;
}

static ClientResponse build_sensor_list_response(GatewayState *state) {
    ClientResponse resp;
    size_t i;
    double now = now_seconds();
    memset(&resp, 0, sizeof(resp));
    resp.status = OK;
    resp.message = smart_dup_cstring("Sensores listados");

    EnterCriticalSection(&state->lock);
    for (i = 0; i < state->sensor_count; ++i) {
        SensorRecord *record = &state->sensors[i];
        SensorInfo copy;
        double timeout = record->info.frequency_seconds * 3.0;
        bool recently_seen;
        memset(&copy, 0, sizeof(copy));
        recently_seen = (now - record->last_seen) <= (timeout > SMARTCITY_STALE_GRACE_SECONDS ? timeout : SMARTCITY_STALE_GRACE_SECONDS);
        copy.sensor_id = smart_dup_cstring(record->info.sensor_id);
        copy.sensor_type = record->info.sensor_type;
        copy.sensor_ip = smart_dup_cstring(record->info.sensor_ip);
        copy.control_tcp_port = record->info.control_tcp_port;
        copy.is_active = record->info.is_active && recently_seen;
        copy.frequency_seconds = record->info.frequency_seconds;
        copy.threshold = record->info.threshold;
        copy.device_kind = record->info.device_kind;
        copy.state_text = smart_dup_cstring(record->info.state_text);
        if ((record->info.sensor_id && !copy.sensor_id) ||
            (record->info.sensor_ip && !copy.sensor_ip) ||
            (record->info.state_text && !copy.state_text)) {
            smart_free_discovery_response((DiscoveryResponse *)&copy);
            smart_free_client_response(&resp);
            LeaveCriticalSection(&state->lock);
            return resp;
        }
        SensorInfo *new_items = (SensorInfo *)realloc(resp.sensors, (resp.sensor_count + 1) * sizeof(SensorInfo));
        if (!new_items) {
            smart_free_discovery_response((DiscoveryResponse *)&copy);
            smart_free_client_response(&resp);
            LeaveCriticalSection(&state->lock);
            return resp;
        }
        resp.sensors = new_items;
        resp.sensors[resp.sensor_count] = copy;
        resp.sensor_count += 1;
    }
    LeaveCriticalSection(&state->lock);
    return resp;
}

static ClientResponse build_history_response(GatewayState *state) {
    ClientResponse resp;
    size_t start;
    memset(&resp, 0, sizeof(resp));
    resp.status = OK;
    resp.message = smart_dup_cstring("Historico de leituras");
    EnterCriticalSection(&state->lock);
    start = state->reading_count > 200 ? state->reading_count - 200 : 0;
    for (; start < state->reading_count; ++start) {
        if (!append_reading_to_response(&resp, &state->readings[start])) {
            smart_free_client_response(&resp);
            LeaveCriticalSection(&state->lock);
            return resp;
        }
    }
    LeaveCriticalSection(&state->lock);
    return resp;
}

static ClientResponse build_metric_response(GatewayState *state, const char *metric, const char *ok_label, const char *empty_label) {
    ClientResponse resp;
    bool has_values = false;
    double avg;
    memset(&resp, 0, sizeof(resp));
    EnterCriticalSection(&state->lock);
    avg = compute_average_locked(state, metric, &has_values);
    LeaveCriticalSection(&state->lock);
    if (!has_values) {
        resp.status = ERROR;
        resp.message = smart_dup_cstring(empty_label);
    } else {
        resp.status = OK;
        resp.message = smart_dup_cstring(ok_label);
        resp.metric_value = avg;
    }
    return resp;
}

static ClientResponse build_max_response(GatewayState *state) {
    ClientResponse resp;
    const SensorReading *mx;
    memset(&resp, 0, sizeof(resp));
    EnterCriticalSection(&state->lock);
    mx = compute_max_locked(state);
    if (!mx) {
        LeaveCriticalSection(&state->lock);
        resp.status = ERROR;
        resp.message = smart_dup_cstring("Sem leituras");
        return resp;
    }
    resp.status = OK;
    resp.metric_value = mx->value;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Maior leitura: %s %.15g %s",
                 mx->sensor_id ? mx->sensor_id : "",
                 mx->value,
                 mx->unit ? mx->unit : "");
        resp.message = smart_dup_cstring(buf);
    }
    LeaveCriticalSection(&state->lock);
    return resp;
}

static ClientResponse send_control_command(GatewayState *state, const char *sensor_id, CommandType command_type, double value, const char *text_value) {
    ClientResponse result;
    SensorRecord *sensor;
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in addr;
    ControlCommand cmd;
    SmartBuffer payload;
    uint8_t *raw = NULL;
    uint32_t raw_len = 0;
    char *sensor_ip_copy = NULL;
    uint32_t control_port = 0;

    memset(&result, 0, sizeof(result));
    memset(&cmd, 0, sizeof(cmd));
    if (!sensor_id) {
        result.status = ERROR;
        result.message = smart_dup_cstring("Dispositivo nao encontrado");
        return result;
    }

    EnterCriticalSection(&state->lock);
    sensor = find_sensor(state, sensor_id);
    if (!sensor) {
        LeaveCriticalSection(&state->lock);
        result.status = ERROR;
        result.message = smart_dup_cstring("Dispositivo nao encontrado");
        return result;
    }
    if (sensor->info.device_kind != ACTUATOR || sensor->info.control_tcp_port == 0) {
        LeaveCriticalSection(&state->lock);
        result.status = ERROR;
        result.message = smart_dup_cstring("Fonte de dados nao controlavel");
        return result;
    }
    sensor_ip_copy = smart_dup_cstring(sensor->info.sensor_ip);
    control_port = sensor->info.control_tcp_port;
    cmd.command_type = command_type;
    cmd.sensor_id = (char *)sensor_id;
    cmd.value = value;
    cmd.text_value = (char *)text_value;
    LeaveCriticalSection(&state->lock);

    if (!smart_encode_control_command(&cmd, &payload)) {
        smart_buffer_free(&payload);
        free(sensor_ip_copy);
        result.status = ERROR;
        result.message = smart_dup_cstring("Falha ao montar comando");
        return result;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        smart_buffer_free(&payload);
        result.status = ERROR;
        result.message = smart_dup_cstring("Falha no socket");
        return result;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)control_port);
    if (!sensor_ip_copy || inet_pton(AF_INET, sensor_ip_copy, &addr.sin_addr) != 1 || connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        smart_buffer_free(&payload);
        free(sensor_ip_copy);
        result.status = ERROR;
        result.message = smart_dup_cstring("Falha no comando");
        return result;
    }

    if (!smart_send_msg(sock, payload.data, (uint32_t)payload.len) || !smart_recv_msg(sock, &raw, &raw_len)) {
        closesocket(sock);
        smart_buffer_free(&payload);
        free(sensor_ip_copy);
        result.status = ERROR;
        result.message = smart_dup_cstring("Sem resposta do sensor");
        return result;
    }
    smart_buffer_free(&payload);
    closesocket(sock);
    free(sensor_ip_copy);

    {
        ControlResponse ctrl_resp;
        memset(&ctrl_resp, 0, sizeof(ctrl_resp));
        if (!smart_parse_control_response(&ctrl_resp, raw, raw_len)) {
            free(raw);
            result.status = ERROR;
            result.message = smart_dup_cstring("Erro ao decodificar resposta");
            return result;
        }
        free(raw);

        EnterCriticalSection(&state->lock);
        sensor = find_sensor(state, sensor_id);
        if (sensor) {
            sensor->info.is_active = ctrl_resp.is_active;
            sensor->info.frequency_seconds = ctrl_resp.frequency_seconds;
            sensor->info.threshold = ctrl_resp.threshold;
            free(sensor->info.state_text);
            sensor->info.state_text = smart_dup_cstring(ctrl_resp.state_text);
            sensor->last_seen = now_seconds();
        }
        LeaveCriticalSection(&state->lock);

        result.status = ctrl_resp.status;
        result.message = smart_dup_cstring(ctrl_resp.message ? ctrl_resp.message : "");
        smart_free_control_response(&ctrl_resp);
    }

    return result;
}

static ClientResponse process_client_request(GatewayState *state, const ClientRequest *req) {
    if (req->request_type == LIST_SENSORS) return build_sensor_list_response(state);
    if (req->request_type == AVG_TEMPERATURE) return build_metric_response(state, "temperature", "Media de temperatura", "Sem leituras de temperatura");
    if (req->request_type == AVG_CO2) return build_metric_response(state, "co2", "Media de CO2", "Sem leituras de CO2");
    if (req->request_type == AVG_HUMIDITY) return build_metric_response(state, "humidity", "Media de umidade", "Sem leituras de umidade");
    if (req->request_type == MAX_READING) return build_max_response(state);
    if (req->request_type == ACTIVATE_SENSOR) return send_control_command(state, req->target_sensor_id, ACTIVATE, 0.0, "");
    if (req->request_type == DEACTIVATE_SENSOR) return send_control_command(state, req->target_sensor_id, DEACTIVATE, 0.0, "");
    if (req->request_type == CHANGE_FREQUENCY) return send_control_command(state, req->target_sensor_id, SET_FREQUENCY, req->value, "");
    if (req->request_type == CHANGE_THRESHOLD) return send_control_command(state, req->target_sensor_id, SET_THRESHOLD, req->value, "");
    if (req->request_type == READING_HISTORY) return build_history_response(state);
    if (req->request_type == SEND_CONTROL_COMMAND) return send_control_command(state, req->target_sensor_id, req->command_type, req->value, req->text_value);
    if (req->request_type == EXIT) {
        ClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = OK;
        resp.message = smart_dup_cstring("Conexao encerrada");
        return resp;
    }
    {
        ClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = ERROR;
        resp.message = smart_dup_cstring("Requisicao invalida");
        return resp;
    }
}

static DWORD WINAPI udp_server_thread(LPVOID param) {
    GatewayState *state = (GatewayState *)param;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    char buffer[65535];
    printf("[Gateway] UDP listening on %s:%d\n", SMARTCITY_GATEWAY_UDP_HOST, SMARTCITY_GATEWAY_UDP_PORT);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SMARTCITY_GATEWAY_UDP_PORT);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    while (1) {
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &from_len);
        if (received > 0) {
            SensorReading reading;
            memset(&reading, 0, sizeof(reading));
            if (smart_parse_sensor_reading(&reading, (const uint8_t *)buffer, (size_t)received)) {
                EnterCriticalSection(&state->lock);
                append_reading_copy(state, &reading);
                update_last_seen_for_reading(state, reading.sensor_id);
                LeaveCriticalSection(&state->lock);
                persist_reading_csv(&reading);
                printf("[Gateway] Reading %s %s=%.2f %s%s\n",
                       reading.sensor_id ? reading.sensor_id : "",
                       reading.metric ? reading.metric : "",
                       reading.value,
                       reading.unit ? reading.unit : "",
                       reading.alert ? " ALERT" : "");
            }
            smart_free_sensor_reading(&reading);
        }
    }
    return 0;
}

static DWORD WINAPI discovery_response_thread(LPVOID param) {
    GatewayState *state = (GatewayState *)param;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    char buffer[65535];
    printf("[Gateway] Discovery responses on 0.0.0.0:%d\n", SMARTCITY_DISCOVERY_RESPONSE_PORT);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SMARTCITY_DISCOVERY_RESPONSE_PORT);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    while (1) {
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &from_len);
        if (received > 0) {
            DiscoveryResponse resp;
            memset(&resp, 0, sizeof(resp));
            if (smart_parse_discovery_response(&resp, (const uint8_t *)buffer, (size_t)received)) {
                update_sensor(state, &resp);
                printf("[Gateway] Sensor discovered: %s (%d) at %s\n",
                       resp.sensor_id ? resp.sensor_id : "",
                       resp.sensor_type,
                       inet_ntoa(from.sin_addr));
            }
            smart_free_discovery_response(&resp);
        }
    }
    return 0;
}

static DWORD WINAPI discovery_loop_thread(LPVOID param) {
    (void)param;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    DiscoveryRequest req;
    SmartBuffer payload;
    DWORD ttl = 2;
    printf("[Gateway] Discovery multicast on %s:%d\n", SMARTCITY_MULTICAST_GROUP, SMARTCITY_MULTICAST_PORT);
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, SMARTCITY_MULTICAST_GROUP, &addr.sin_addr);
    addr.sin_port = htons(SMARTCITY_MULTICAST_PORT);

    for (;;) {
        char timestamp[64];
        memset(&req, 0, sizeof(req));
        iso_timestamp(timestamp, sizeof(timestamp));
        req.gateway_id = SMARTCITY_GATEWAY_ID;
        req.request_timestamp = timestamp;
        req.gateway_udp_port = SMARTCITY_GATEWAY_UDP_PORT;
        req.discovery_response_port = SMARTCITY_DISCOVERY_RESPONSE_PORT;
        if (smart_encode_discovery_request(&req, &payload)) {
            sendto(sock, (const char *)payload.data, (int)payload.len, 0, (struct sockaddr *)&addr, sizeof(addr));
            smart_buffer_free(&payload);
        } else {
            smart_buffer_free(&payload);
        }
        Sleep(5000);
    }
    return 0;
}

static DWORD WINAPI client_thread(LPVOID param) {
    SOCKET conn = (SOCKET)(uintptr_t)param;
    GatewayState *state = NULL;
    ClientRequest req;
    uint8_t *raw = NULL;
    uint32_t raw_len = 0;
    extern GatewayState *g_gateway_state;
    state = g_gateway_state;

    for (;;) {
        ClientResponse resp;
        SmartBuffer payload;
        bool exit_requested;
        memset(&req, 0, sizeof(req));
        if (!smart_recv_msg(conn, &raw, &raw_len)) {
            break;
        }
        if (!smart_parse_client_request(&req, raw, raw_len)) {
            free(raw);
            break;
        }
        free(raw);
        raw = NULL;
        exit_requested = req.request_type == EXIT;

        resp = process_client_request(state, &req);
        smart_free_client_request(&req);

        if (!smart_encode_client_response(&resp, &payload)) {
            smart_free_client_response(&resp);
            break;
        }
        smart_send_msg(conn, payload.data, (uint32_t)payload.len);
        smart_buffer_free(&payload);
        smart_free_client_response(&resp);

        if (exit_requested) {
            break;
        }
    }

    closesocket(conn);
    return 0;
}

GatewayState *g_gateway_state = NULL;

static void free_gateway_state(GatewayState *state) {
    size_t i;
    for (i = 0; i < state->sensor_count; ++i) {
        free_sensor_record(&state->sensors[i]);
    }
    for (i = 0; i < state->reading_count; ++i) {
        smart_free_sensor_reading(&state->readings[i]);
    }
    free(state->sensors);
    free(state->readings);
    DeleteCriticalSection(&state->lock);
}

int main(void) {
    WSADATA wsa;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in addr;
    GatewayState state;
    HANDLE threads[3];

    memset(&state, 0, sizeof(state));
    InitializeCriticalSection(&state.lock);
    g_gateway_state = &state;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Falha ao inicializar Winsock\n");
        return 1;
    }

    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        printf("Falha ao criar socket TCP\n");
        WSACleanup();
        free_gateway_state(&state);
        return 1;
    }

    {
        BOOL reuse = TRUE;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SMARTCITY_GATEWAY_TCP_PORT);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR || listen(server, SOMAXCONN) == SOCKET_ERROR) {
        printf("Falha ao iniciar TCP do gateway\n");
        closesocket(server);
        WSACleanup();
        free_gateway_state(&state);
        return 1;
    }
    printf("[Gateway] TCP listening on %s:%d\n", SMARTCITY_GATEWAY_TCP_HOST, SMARTCITY_GATEWAY_TCP_PORT);

    threads[0] = CreateThread(NULL, 0, udp_server_thread, &state, 0, NULL);
    threads[1] = CreateThread(NULL, 0, discovery_response_thread, &state, 0, NULL);
    threads[2] = CreateThread(NULL, 0, discovery_loop_thread, &state, 0, NULL);

    for (;;) {
        SOCKET conn = accept(server, NULL, NULL);
        if (conn != INVALID_SOCKET) {
            printf("[Gateway] Client connected\n");
            CreateThread(NULL, 0, client_thread, (LPVOID)(uintptr_t)conn, 0, NULL);
        }
    }

    closesocket(server);
    WSACleanup();
    free_gateway_state(&state);
    return 0;
}
