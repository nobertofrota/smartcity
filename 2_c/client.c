#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "Ws2_32.lib")

#define GATEWAY_TCP_IP "127.0.0.1"
#define GATEWAY_TCP_PORT 9000

#define RESPONSE_STATUS_OK 1
#define RESPONSE_STATUS_ERROR 2

#define REQUEST_LIST_SENSORS 1
#define REQUEST_AVG_TEMPERATURE 2
#define REQUEST_AVG_CO2 3
#define REQUEST_MAX_READING 4
#define REQUEST_READING_HISTORY 11
#define REQUEST_EXIT 9
#define REQUEST_AVG_HUMIDITY 10
#define REQUEST_SEND_CONTROL_COMMAND 12

#define COMMAND_ACTIVATE 1
#define COMMAND_DEACTIVATE 2
#define COMMAND_TRAFFIC_LIGHT_SET_COLOR 8
#define COMMAND_STREET_LIGHT_SET_BRIGHTNESS 10

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char *sensor_id;
    int sensor_type;
    char *sensor_ip;
    uint32_t control_tcp_port;
    bool is_active;
    double frequency_seconds;
    double threshold;
    int device_kind;
    char *state_text;
} SensorInfo;

typedef struct {
    char *sensor_id;
    int sensor_type;
    double value;
    char *unit;
    int64_t timestamp_unix_ms;
    bool alert;
    char *alert_message;
    char *metric;
} SensorReading;

typedef struct {
    int status;
    char *message;
    SensorInfo *sensors;
    size_t sensor_count;
    double metric_value;
    SensorReading *readings;
    size_t reading_count;
} ClientResponse;

typedef struct {
    int request_type;
    char *target_sensor_id;
    double value;
    int command_type;
    char *text_value;
} ClientRequest;

static void buffer_init(Buffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void buffer_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static bool buffer_reserve(Buffer *buf, size_t extra) {
    if (buf->len + extra <= buf->cap) {
        return true;
    }

    size_t new_cap = buf->cap ? buf->cap : 128;
    while (new_cap < buf->len + extra) {
        new_cap *= 2;
    }

    uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
    if (!new_data) {
        return false;
    }

    buf->data = new_data;
    buf->cap = new_cap;
    return true;
}

static bool buffer_append(Buffer *buf, const void *data, size_t size) {
    if (!buffer_reserve(buf, size)) {
        return false;
    }
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    return true;
}

static bool buffer_append_byte(Buffer *buf, uint8_t byte) {
    return buffer_append(buf, &byte, 1);
}

static bool buffer_append_u32_be(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)((value >> 24) & 0xFF);
    out[1] = (uint8_t)((value >> 16) & 0xFF);
    out[2] = (uint8_t)((value >> 8) & 0xFF);
    out[3] = (uint8_t)(value & 0xFF);
    return true;
}

static bool buffer_append_varint(Buffer *buf, uint64_t value) {
    while (value >= 0x80) {
        uint8_t byte = (uint8_t)((value & 0x7F) | 0x80);
        if (!buffer_append_byte(buf, byte)) {
            return false;
        }
        value >>= 7;
    }
    return buffer_append_byte(buf, (uint8_t)value);
}

static bool buffer_append_key(Buffer *buf, uint32_t field_number, uint32_t wire_type) {
    uint64_t key = ((uint64_t)field_number << 3) | wire_type;
    return buffer_append_varint(buf, key);
}

static bool buffer_append_string_field(Buffer *buf, uint32_t field_number, const char *value) {
    if (!value || value[0] == '\0') {
        return true;
    }

    size_t len = strlen(value);
    if (!buffer_append_key(buf, field_number, 2)) {
        return false;
    }
    if (!buffer_append_varint(buf, (uint64_t)len)) {
        return false;
    }
    return buffer_append(buf, value, len);
}

static bool buffer_append_double_field(Buffer *buf, uint32_t field_number, double value) {
    if (!buffer_append_key(buf, field_number, 1)) {
        return false;
    }
    union {
        double d;
        uint8_t b[8];
    } u;
    u.d = value;
    return buffer_append(buf, u.b, 8);
}

static bool buffer_append_varint_field(Buffer *buf, uint32_t field_number, uint64_t value) {
    if (!buffer_append_key(buf, field_number, 0)) {
        return false;
    }
    return buffer_append_varint(buf, value);
}

static bool read_exact(SOCKET sock, uint8_t *out, size_t size) {
    size_t received = 0;
    while (received < size) {
        int chunk = recv(sock, (char *)out + received, (int)(size - received), 0);
        if (chunk <= 0) {
            return false;
        }
        received += (size_t)chunk;
    }
    return true;
}

static bool send_all(SOCKET sock, const uint8_t *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        int chunk = send(sock, (const char *)data + sent, (int)(size - sent), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += (size_t)chunk;
    }
    return true;
}

static bool send_msg(SOCKET sock, const uint8_t *payload, uint32_t payload_len) {
    uint8_t header[4];
    buffer_append_u32_be(header, payload_len);
    if (!send_all(sock, header, sizeof(header))) {
        return false;
    }
    return send_all(sock, payload, payload_len);
}

static uint64_t read_varint(const uint8_t *data, size_t len, size_t *pos, bool *ok) {
    uint64_t result = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        uint8_t byte = data[(*pos)++];
        result |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) {
            *ok = true;
            return result;
        }
        shift += 7;
    }
    *ok = false;
    return 0;
}

static bool read_key(const uint8_t *data, size_t len, size_t *pos, uint32_t *field_number, uint32_t *wire_type) {
    bool ok = false;
    uint64_t key = read_varint(data, len, pos, &ok);
    if (!ok) {
        return false;
    }
    *field_number = (uint32_t)(key >> 3);
    *wire_type = (uint32_t)(key & 0x07);
    return true;
}

static bool read_length_delimited(const uint8_t *data, size_t len, size_t *pos, const uint8_t **out_ptr, size_t *out_len) {
    bool ok = false;
    uint64_t field_len = read_varint(data, len, pos, &ok);
    if (!ok || field_len > SIZE_MAX || *pos + (size_t)field_len > len) {
        return false;
    }
    *out_ptr = data + *pos;
    *out_len = (size_t)field_len;
    *pos += (size_t)field_len;
    return true;
}

static bool read_fixed64_double(const uint8_t *data, size_t len, size_t *pos, double *out) {
    if (*pos + 8 > len) {
        return false;
    }
    union {
        double d;
        uint8_t b[8];
    } u;
    memcpy(u.b, data + *pos, 8);
    *pos += 8;
    *out = u.d;
    return true;
}

static bool skip_field(const uint8_t *data, size_t len, size_t *pos, uint32_t wire_type) {
    switch (wire_type) {
        case 0: {
            bool ok = false;
            (void)read_varint(data, len, pos, &ok);
            return ok;
        }
        case 1:
            if (*pos + 8 > len) {
                return false;
            }
            *pos += 8;
            return true;
        case 2: {
            const uint8_t *ptr = NULL;
            size_t n = 0;
            return read_length_delimited(data, len, pos, &ptr, &n);
        }
        case 5:
            if (*pos + 4 > len) {
                return false;
            }
            *pos += 4;
            return true;
        default:
            return false;
    }
}

static char *dup_slice(const uint8_t *data, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

static void free_sensor_info(SensorInfo *info) {
    free(info->sensor_id);
    free(info->sensor_ip);
    free(info->state_text);
    memset(info, 0, sizeof(*info));
}

static void free_sensor_reading(SensorReading *reading) {
    free(reading->sensor_id);
    free(reading->unit);
    free(reading->alert_message);
    free(reading->metric);
    memset(reading, 0, sizeof(*reading));
}

static void free_client_response(ClientResponse *resp) {
    size_t i;
    free(resp->message);
    for (i = 0; i < resp->sensor_count; ++i) {
        free_sensor_info(&resp->sensors[i]);
    }
    for (i = 0; i < resp->reading_count; ++i) {
        free_sensor_reading(&resp->readings[i]);
    }
    free(resp->sensors);
    free(resp->readings);
    memset(resp, 0, sizeof(*resp));
}

static bool append_sensor_info(ClientResponse *resp, const SensorInfo *info) {
    SensorInfo *new_items = (SensorInfo *)realloc(resp->sensors, (resp->sensor_count + 1) * sizeof(SensorInfo));
    if (!new_items) {
        return false;
    }
    resp->sensors = new_items;
    resp->sensors[resp->sensor_count] = *info;
    resp->sensor_count += 1;
    return true;
}

static bool append_sensor_reading(ClientResponse *resp, const SensorReading *reading) {
    SensorReading *new_items = (SensorReading *)realloc(resp->readings, (resp->reading_count + 1) * sizeof(SensorReading));
    if (!new_items) {
        return false;
    }
    resp->readings = new_items;
    resp->readings[resp->reading_count] = *reading;
    resp->reading_count += 1;
    return true;
}

static bool parse_sensor_info(SensorInfo *info, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(info, 0, sizeof(*info));

    while (pos < len) {
        uint32_t field_number = 0;
        uint32_t wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            return false;
        }

        switch (field_number) {
            case 1: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                info->sensor_id = dup_slice(ptr, n);
                if (!info->sensor_id) {
                    return false;
                }
                break;
            }
            case 2: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                info->sensor_type = (int)read_varint(data, len, &pos, &ok);
                if (!ok) {
                    return false;
                }
                break;
            }
            case 3: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                info->sensor_ip = dup_slice(ptr, n);
                if (!info->sensor_ip) {
                    return false;
                }
                break;
            }
            case 4: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                info->control_tcp_port = (uint32_t)read_varint(data, len, &pos, &ok);
                if (!ok) {
                    return false;
                }
                break;
            }
            case 5: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                info->is_active = read_varint(data, len, &pos, &ok) != 0;
                if (!ok) {
                    return false;
                }
                break;
            }
            case 6:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &info->frequency_seconds)) {
                    return false;
                }
                break;
            case 7:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &info->threshold)) {
                    return false;
                }
                break;
            case 8: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                info->device_kind = (int)read_varint(data, len, &pos, &ok);
                if (!ok) {
                    return false;
                }
                break;
            }
            case 9: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                info->state_text = dup_slice(ptr, n);
                if (!info->state_text) {
                    return false;
                }
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) {
                    return false;
                }
                break;
        }
    }

    return true;
}

static bool parse_sensor_reading(SensorReading *reading, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(reading, 0, sizeof(*reading));

    while (pos < len) {
        uint32_t field_number = 0;
        uint32_t wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            return false;
        }

        switch (field_number) {
            case 1: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                reading->sensor_id = dup_slice(ptr, n);
                if (!reading->sensor_id) {
                    return false;
                }
                break;
            }
            case 2: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                reading->sensor_type = (int)read_varint(data, len, &pos, &ok);
                if (!ok) {
                    return false;
                }
                break;
            }
            case 3:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &reading->value)) {
                    return false;
                }
                break;
            case 4: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                reading->unit = dup_slice(ptr, n);
                if (!reading->unit) {
                    return false;
                }
                break;
            }
            case 5: {
                bool ok = false;
                uint64_t v;
                if (wire_type != 0) {
                    return false;
                }
                v = read_varint(data, len, &pos, &ok);
                if (!ok) {
                    return false;
                }
                reading->timestamp_unix_ms = (int64_t)v;
                break;
            }
            case 6: {
                bool ok = false;
                if (wire_type != 0) {
                    return false;
                }
                reading->alert = read_varint(data, len, &pos, &ok) != 0;
                if (!ok) {
                    return false;
                }
                break;
            }
            case 7: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                reading->alert_message = dup_slice(ptr, n);
                if (!reading->alert_message) {
                    return false;
                }
                break;
            }
            case 8: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    return false;
                }
                reading->metric = dup_slice(ptr, n);
                if (!reading->metric) {
                    return false;
                }
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) {
                    return false;
                }
                break;
        }
    }

    return true;
}

static bool parse_client_response(ClientResponse *resp, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(resp, 0, sizeof(*resp));

    while (pos < len) {
        uint32_t field_number = 0;
        uint32_t wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            free_client_response(resp);
            return false;
        }

        switch (field_number) {
            case 1: {
                bool ok = false;
                if (wire_type != 0) {
                    free_client_response(resp);
                    return false;
                }
                resp->status = (int)read_varint(data, len, &pos, &ok);
                if (!ok) {
                    free_client_response(resp);
                    return false;
                }
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    free_client_response(resp);
                    return false;
                }
                resp->message = dup_slice(ptr, n);
                if (!resp->message) {
                    free_client_response(resp);
                    return false;
                }
                break;
            }
            case 3: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                SensorInfo info;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    free_client_response(resp);
                    return false;
                }
                if (!parse_sensor_info(&info, ptr, n) || !append_sensor_info(resp, &info)) {
                    free_sensor_info(&info);
                    free_client_response(resp);
                    return false;
                }
                break;
            }
            case 4:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &resp->metric_value)) {
                    free_client_response(resp);
                    return false;
                }
                break;
            case 5: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                SensorReading reading;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) {
                    free_client_response(resp);
                    return false;
                }
                if (!parse_sensor_reading(&reading, ptr, n) || !append_sensor_reading(resp, &reading)) {
                    free_sensor_reading(&reading);
                    free_client_response(resp);
                    return false;
                }
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) {
                    free_client_response(resp);
                    return false;
                }
                break;
        }
    }

    return true;
}

static bool encode_client_request(const ClientRequest *req, Buffer *out) {
    buffer_init(out);

    if (!buffer_append_varint_field(out, 1, (uint64_t)req->request_type)) {
        return false;
    }
    if (req->target_sensor_id && !buffer_append_string_field(out, 2, req->target_sensor_id)) {
        return false;
    }
    if (req->value != 0.0 && !buffer_append_double_field(out, 3, req->value)) {
        return false;
    }
    if (req->command_type != 0 && !buffer_append_varint_field(out, 4, (uint64_t)req->command_type)) {
        return false;
    }
    if (req->text_value && !buffer_append_string_field(out, 5, req->text_value)) {
        return false;
    }

    return true;
}

static void request_init(ClientRequest *req) {
    memset(req, 0, sizeof(*req));
}

static void request_free(ClientRequest *req) {
    free(req->target_sensor_id);
    free(req->text_value);
    memset(req, 0, sizeof(*req));
}

static char *dup_cstring(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, len + 1);
    return out;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        --len;
    }
}

static bool read_line(const char *prompt, char *buf, size_t size) {
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (!fgets(buf, (int)size, stdin)) {
        return false;
    }
    trim_newline(buf);
    return true;
}

static bool parse_int_line(const char *text, int *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0') {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_double_line(const char *text, double *out) {
    char *end = NULL;
    double value = strtod(text, &end);
    if (text[0] == '\0' || end == text || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static void show_menu(void) {
    printf("\n=== Cliente Analitico ===\n");
    printf("1 - Listar dispositivos conectados\n");
    printf("2 - Consultar media de temperatura\n");
    printf("3 - Consultar media de CO2\n");
    printf("4 - Consultar media de umidade\n");
    printf("5 - Consultar maior leitura registrada\n");
    printf("6 - Consultar historico de leituras\n");
    printf("7 - Ligar atuador\n");
    printf("8 - Desligar atuador\n");
    printf("9 - Alterar cor do semaforo\n");
    printf("10 - Alterar brilho do poste\n");
    printf("11 - Sair\n");
}

static const char *status_text(int status) {
    return status == RESPONSE_STATUS_OK ? "OK" : "ERROR";
}

static void print_response(const ClientResponse *resp) {
    printf("\n[%s] %s\n", status_text(resp->status), resp->message ? resp->message : "");

    if (resp->sensor_count > 0) {
        size_t i;
        printf("Dispositivos:\n");
        for (i = 0; i < resp->sensor_count; ++i) {
            const SensorInfo *s = &resp->sensors[i];
            printf("- id=%s type=%d ip=%s control_port=%u active=%d kind=%d state=%s\n",
                   s->sensor_id ? s->sensor_id : "",
                   s->sensor_type,
                   s->sensor_ip ? s->sensor_ip : "",
                   s->control_tcp_port,
                   s->is_active ? 1 : 0,
                   s->device_kind,
                   s->state_text ? s->state_text : "");
        }
    }

    if (resp->metric_value != 0.0) {
        printf("Valor: %.2f\n", resp->metric_value);
    }

    if (resp->reading_count > 0) {
        size_t i;
        printf("Historico:\n");
        for (i = 0; i < resp->reading_count; ++i) {
            const SensorReading *r = &resp->readings[i];
            printf("- id=%s metric=%s value=%.2f unit=%s ts=%" PRId64 " alert=%d msg=%s\n",
                   r->sensor_id ? r->sensor_id : "",
                   r->metric ? r->metric : "",
                   r->value,
                   r->unit ? r->unit : "",
                   r->timestamp_unix_ms,
                   r->alert ? 1 : 0,
                   r->alert_message ? r->alert_message : "");
        }
    }
}

static bool build_request(int option, ClientRequest *req) {
    char input[256];
    request_init(req);

    switch (option) {
        case 1:
            req->request_type = REQUEST_LIST_SENSORS;
            break;
        case 2:
            req->request_type = REQUEST_AVG_TEMPERATURE;
            break;
        case 3:
            req->request_type = REQUEST_AVG_CO2;
            break;
        case 4:
            req->request_type = REQUEST_AVG_HUMIDITY;
            break;
        case 5:
            req->request_type = REQUEST_MAX_READING;
            break;
        case 6:
            req->request_type = REQUEST_READING_HISTORY;
            break;
        case 7:
        case 8:
        case 9:
        case 10:
            req->request_type = REQUEST_SEND_CONTROL_COMMAND;
            if (!read_line("Dispositivo ID: ", input, sizeof(input))) {
                return false;
            }
            req->target_sensor_id = dup_cstring(input);
            if (!req->target_sensor_id) {
                return false;
            }
            break;
        case 11:
            req->request_type = REQUEST_EXIT;
            break;
        default:
            return false;
    }

    if (option == 7) {
        req->command_type = COMMAND_ACTIVATE;
    } else if (option == 8) {
        req->command_type = COMMAND_DEACTIVATE;
    } else if (option == 9) {
        req->command_type = COMMAND_TRAFFIC_LIGHT_SET_COLOR;
        if (!read_line("Cor (verde/amarelo/vermelho): ", input, sizeof(input))) {
            return false;
        }
        req->text_value = dup_cstring(input);
        if (!req->text_value) {
            return false;
        }
    } else if (option == 10) {
        req->command_type = COMMAND_STREET_LIGHT_SET_BRIGHTNESS;
        if (!read_line("Brilho (0-100): ", input, sizeof(input))) {
            return false;
        }
        if (!parse_double_line(input, &req->value)) {
            return false;
        }
    }

    return true;
}

static bool recv_msg(SOCKET sock, uint8_t **payload, uint32_t *payload_len) {
    uint8_t header[4];
    uint32_t len = 0;
    if (!read_exact(sock, header, sizeof(header))) {
        return false;
    }
    len = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | (uint32_t)header[3];
    *payload = (uint8_t *)malloc(len);
    if (!*payload) {
        return false;
    }
    if (!read_exact(sock, *payload, len)) {
        free(*payload);
        *payload = NULL;
        return false;
    }
    *payload_len = len;
    return true;
}

int main(void) {
    WSADATA wsa;
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in addr;
    bool connected = false;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Falha ao inicializar Winsock\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("Falha ao criar socket\n");
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GATEWAY_TCP_PORT);
    addr.sin_addr.s_addr = inet_addr(GATEWAY_TCP_IP);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("Falha ao conectar no gateway\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    connected = true;

    {
        DWORD timeout_ms = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    }

    while (connected) {
        char line[64];
        int option = 0;
        ClientRequest req;
        Buffer encoded;
        uint8_t *raw = NULL;
        uint32_t raw_len = 0;
        ClientResponse resp;

        show_menu();
        if (!read_line("Escolha: ", line, sizeof(line))) {
            break;
        }
        if (!parse_int_line(line, &option)) {
            printf("Opcao invalida\n");
            continue;
        }

        if (!build_request(option, &req)) {
            if (option == 7 || option == 8 || option == 9 || option == 10) {
                printf("Entrada invalida\n");
            } else {
                printf("Opcao invalida\n");
            }
            request_free(&req);
            continue;
        }

        if (!encode_client_request(&req, &encoded)) {
            printf("Erro ao codificar requisicao\n");
            request_free(&req);
            break;
        }

        if (!send_msg(sock, encoded.data, (uint32_t)encoded.len)) {
            printf("Erro no cliente: falha ao enviar requisicao\n");
            buffer_free(&encoded);
            request_free(&req);
            break;
        }

        buffer_free(&encoded);
        request_free(&req);

        if (!recv_msg(sock, &raw, &raw_len)) {
            printf("Conexao encerrada pelo gateway\n");
            break;
        }

        if (!parse_client_response(&resp, raw, raw_len)) {
            printf("Erro ao decodificar resposta\n");
            free(raw);
            break;
        }

        print_response(&resp);

        free_client_response(&resp);
        free(raw);

        if (option == 11) {
            break;
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
