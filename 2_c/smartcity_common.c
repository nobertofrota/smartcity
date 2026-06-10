#include "smartcity_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/time.h>
#endif

static bool buffer_reserve(SmartBuffer *buf, size_t extra) {
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

static bool buffer_append(SmartBuffer *buf, const void *data, size_t size) {
    if (!buffer_reserve(buf, size)) {
        return false;
    }
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    return true;
}

static bool buffer_append_byte(SmartBuffer *buf, uint8_t byte) {
    return buffer_append(buf, &byte, 1);
}

static bool buffer_append_varint(SmartBuffer *buf, uint64_t value) {
    while (value >= 0x80) {
        uint8_t byte = (uint8_t)((value & 0x7F) | 0x80);
        if (!buffer_append_byte(buf, byte)) {
            return false;
        }
        value >>= 7;
    }
    return buffer_append_byte(buf, (uint8_t)value);
}

static bool buffer_append_key(SmartBuffer *buf, uint32_t field_number, uint32_t wire_type) {
    uint64_t key = ((uint64_t)field_number << 3) | wire_type;
    return buffer_append_varint(buf, key);
}

static bool buffer_append_string_field(SmartBuffer *buf, uint32_t field_number, const char *value) {
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

static bool buffer_append_double_field(SmartBuffer *buf, uint32_t field_number, double value) {
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

static bool buffer_append_varint_field(SmartBuffer *buf, uint32_t field_number, uint64_t value) {
    if (!buffer_append_key(buf, field_number, 0)) {
        return false;
    }
    return buffer_append_varint(buf, value);
}

void smart_buffer_init(SmartBuffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void smart_buffer_free(SmartBuffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

bool smart_send_all(SOCKET sock, const uint8_t *data, size_t size) {
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

bool smart_recv_exact(SOCKET sock, uint8_t *out, size_t size) {
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

bool smart_send_msg(SOCKET sock, const uint8_t *payload, uint32_t payload_len) {
    uint32_t len_be = htonl(payload_len);
    return smart_send_all(sock, (const uint8_t *)&len_be, sizeof(len_be)) &&
           smart_send_all(sock, payload, payload_len);
}

bool smart_recv_msg(SOCKET sock, uint8_t **payload, uint32_t *payload_len) {
    uint32_t len_be = 0;
    if (!smart_recv_exact(sock, (uint8_t *)&len_be, sizeof(len_be))) {
        return false;
    }
    uint32_t len = ntohl(len_be);
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        return false;
    }
    if (!smart_recv_exact(sock, buf, len)) {
        free(buf);
        return false;
    }
    *payload = buf;
    *payload_len = len;
    return true;
}

char *smart_dup_cstring(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, len + 1);
    return out;
}

char *smart_dup_slice(const uint8_t *data, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

void smart_trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        --len;
    }
}

bool smart_read_line(const char *prompt, char *buf, size_t size) {
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (!fgets(buf, (int)size, stdin)) {
        return false;
    }
    smart_trim_newline(buf);
    return true;
}

bool smart_parse_int_line(const char *text, int *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0') {
        return false;
    }
    *out = (int)value;
    return true;
}

bool smart_parse_double_line(const char *text, double *out) {
    char *end = NULL;
    double value = strtod(text, &end);
    if (text[0] == '\0' || end == text || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

char *smart_get_local_ip(const char *remote_ip) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in addr;
    char buffer[INET_ADDRSTRLEN] = {0};
    int addr_len = sizeof(addr);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return smart_dup_cstring("127.0.0.1");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1);
    if (inet_pton(AF_INET, remote_ip, &addr.sin_addr) != 1) {
        closesocket(sock);
        return smart_dup_cstring("127.0.0.1");
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(sock, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, buffer, sizeof(buffer));
    } else {
        strcpy(buffer, "127.0.0.1");
    }

    closesocket(sock);
    return smart_dup_cstring(buffer);
}

int64_t smart_unix_time_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER uli;
    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;

    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (int64_t)((uli.QuadPart - EPOCH_DIFF_100NS) / 10000ULL);
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
#endif
}

double smart_now_seconds(void) {
    return (double)smart_unix_time_ms() / 1000.0;
}

void smart_iso_utc_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm_ptr = NULL;

    if (!buf || size == 0) {
        return;
    }

#ifdef _WIN32
    tm_ptr = gmtime(&now);
    if (tm_ptr) {
        tm_buf = *tm_ptr;
        tm_ptr = &tm_buf;
    }
#else
    if (gmtime_r(&now, &tm_buf)) {
        tm_ptr = &tm_buf;
    }
#endif

    if (!tm_ptr) {
        snprintf(buf, size, "1970-01-01T00:00:00Z");
        return;
    }

    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_ptr->tm_year + 1900,
             tm_ptr->tm_mon + 1,
             tm_ptr->tm_mday,
             tm_ptr->tm_hour,
             tm_ptr->tm_min,
             tm_ptr->tm_sec);
}

void smart_free_discovery_response(DiscoveryResponse *resp) {
    free(resp->sensor_id);
    free(resp->sensor_ip);
    free(resp->state_text);
    memset(resp, 0, sizeof(*resp));
}

void smart_free_discovery_request(DiscoveryRequest *req) {
    free(req->gateway_id);
    free(req->request_timestamp);
    memset(req, 0, sizeof(*req));
}

void smart_free_sensor_reading(SensorReading *reading) {
    free(reading->sensor_id);
    free(reading->unit);
    free(reading->alert_message);
    free(reading->metric);
    memset(reading, 0, sizeof(*reading));
}

void smart_free_client_response(ClientResponse *resp) {
    size_t i;
    free(resp->message);
    for (i = 0; i < resp->sensor_count; ++i) {
        smart_free_discovery_response((DiscoveryResponse *)&resp->sensors[i]);
    }
    for (i = 0; i < resp->reading_count; ++i) {
        smart_free_sensor_reading(&resp->readings[i]);
    }
    free(resp->sensors);
    free(resp->readings);
    memset(resp, 0, sizeof(*resp));
}

void smart_free_control_response(ControlResponse *resp) {
    free(resp->message);
    free(resp->sensor_id);
    free(resp->state_text);
    memset(resp, 0, sizeof(*resp));
}

void smart_free_client_request(ClientRequest *req) {
    free(req->target_sensor_id);
    free(req->text_value);
    memset(req, 0, sizeof(*req));
}

void smart_free_control_command(ControlCommand *cmd) {
    free(cmd->sensor_id);
    free(cmd->text_value);
    memset(cmd, 0, sizeof(*cmd));
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

static bool append_sensor_info(SensorInfo **dst_array, size_t *count, const SensorInfo *src) {
    SensorInfo *new_items = (SensorInfo *)realloc(*dst_array, (*count + 1) * sizeof(SensorInfo));
    if (!new_items) {
        return false;
    }
    *dst_array = new_items;
    (*dst_array)[*count] = *src;
    *count += 1;
    return true;
}

static bool append_reading(SensorReading **dst_array, size_t *count, const SensorReading *src) {
    SensorReading *new_items = (SensorReading *)realloc(*dst_array, (*count + 1) * sizeof(SensorReading));
    if (!new_items) {
        return false;
    }
    *dst_array = new_items;
    (*dst_array)[*count] = *src;
    *count += 1;
    return true;
}

static bool encode_sensor_info(const SensorInfo *info, SmartBuffer *out) {
    if (!buffer_append_string_field(out, 1, info->sensor_id)) return false;
    if (info->sensor_type != 0 && !buffer_append_varint_field(out, 2, (uint64_t)info->sensor_type)) return false;
    if (!buffer_append_string_field(out, 3, info->sensor_ip)) return false;
    if (info->control_tcp_port != 0 && !buffer_append_varint_field(out, 4, info->control_tcp_port)) return false;
    if (info->is_active && !buffer_append_varint_field(out, 5, 1)) return false;
    if (info->frequency_seconds != 0.0 && !buffer_append_double_field(out, 6, info->frequency_seconds)) return false;
    if (info->threshold != 0.0 && !buffer_append_double_field(out, 7, info->threshold)) return false;
    if (info->device_kind != 0 && !buffer_append_varint_field(out, 8, (uint64_t)info->device_kind)) return false;
    if (!buffer_append_string_field(out, 9, info->state_text)) return false;
    return true;
}

static bool parse_sensor_info(SensorInfo *info, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(info, 0, sizeof(*info));
    while (pos < len) {
        uint32_t field_number = 0;
        uint32_t wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            smart_free_discovery_response((DiscoveryResponse *)info);
            return false;
        }
        switch (field_number) {
            case 1: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                info->sensor_id = smart_dup_slice(ptr, n);
                if (!info->sensor_id) return false;
                break;
            }
            case 2: {
                bool ok = false;
                if (wire_type != 0) return false;
                info->sensor_type = (DeviceType)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 3: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                info->sensor_ip = smart_dup_slice(ptr, n);
                if (!info->sensor_ip) return false;
                break;
            }
            case 4: {
                bool ok = false;
                if (wire_type != 0) return false;
                info->control_tcp_port = (uint32_t)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 5: {
                bool ok = false;
                if (wire_type != 0) return false;
                info->is_active = read_varint(data, len, &pos, &ok) != 0;
                if (!ok) return false;
                break;
            }
            case 6:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &info->frequency_seconds)) return false;
                break;
            case 7:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &info->threshold)) return false;
                break;
            case 8: {
                bool ok = false;
                if (wire_type != 0) return false;
                info->device_kind = (DeviceKind)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 9: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                info->state_text = smart_dup_slice(ptr, n);
                if (!info->state_text) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

static bool parse_sensor_reading_internal(SensorReading *reading, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(reading, 0, sizeof(*reading));
    while (pos < len) {
        uint32_t field_number = 0;
        uint32_t wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            smart_free_sensor_reading(reading);
            return false;
        }
        switch (field_number) {
            case 1: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                reading->sensor_id = smart_dup_slice(ptr, n);
                if (!reading->sensor_id) return false;
                break;
            }
            case 2: {
                bool ok = false;
                if (wire_type != 0) return false;
                reading->sensor_type = (DeviceType)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 3:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &reading->value)) return false;
                break;
            case 4: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                reading->unit = smart_dup_slice(ptr, n);
                if (!reading->unit) return false;
                break;
            }
            case 5: {
                bool ok = false;
                if (wire_type != 0) return false;
                reading->timestamp_unix_ms = (int64_t)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 6: {
                bool ok = false;
                if (wire_type != 0) return false;
                reading->alert = read_varint(data, len, &pos, &ok) != 0;
                if (!ok) return false;
                break;
            }
            case 7: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                reading->alert_message = smart_dup_slice(ptr, n);
                if (!reading->alert_message) return false;
                break;
            }
            case 8: {
                const uint8_t *ptr = NULL;
                size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                reading->metric = smart_dup_slice(ptr, n);
                if (!reading->metric) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

static bool encode_sensor_reading_internal(const SensorReading *reading, SmartBuffer *out) {
    if (!buffer_append_string_field(out, 1, reading->sensor_id)) return false;
    if (reading->sensor_type != 0 && !buffer_append_varint_field(out, 2, (uint64_t)reading->sensor_type)) return false;
    if (!buffer_append_double_field(out, 3, reading->value)) return false;
    if (!buffer_append_string_field(out, 4, reading->unit)) return false;
    if (reading->timestamp_unix_ms != 0 && !buffer_append_varint_field(out, 5, (uint64_t)reading->timestamp_unix_ms)) return false;
    if (reading->alert && !buffer_append_varint_field(out, 6, 1)) return false;
    if (!buffer_append_string_field(out, 7, reading->alert_message)) return false;
    if (!buffer_append_string_field(out, 8, reading->metric)) return false;
    return true;
}

static bool encode_control_response_internal(const ControlResponse *resp, SmartBuffer *out) {
    if (resp->status != 0 && !buffer_append_varint_field(out, 1, (uint64_t)resp->status)) return false;
    if (!buffer_append_string_field(out, 2, resp->message)) return false;
    if (!buffer_append_string_field(out, 3, resp->sensor_id)) return false;
    if (resp->is_active && !buffer_append_varint_field(out, 4, 1)) return false;
    if (resp->frequency_seconds != 0.0 && !buffer_append_double_field(out, 5, resp->frequency_seconds)) return false;
    if (resp->threshold != 0.0 && !buffer_append_double_field(out, 6, resp->threshold)) return false;
    if (!buffer_append_string_field(out, 7, resp->state_text)) return false;
    return true;
}

bool smart_encode_discovery_request(const DiscoveryRequest *req, SmartBuffer *out) {
    smart_buffer_init(out);
    if (!buffer_append_string_field(out, 1, req->gateway_id)) return false;
    if (!buffer_append_string_field(out, 2, req->request_timestamp)) return false;
    if (req->gateway_udp_port != 0 && !buffer_append_varint_field(out, 3, req->gateway_udp_port)) return false;
    if (req->discovery_response_port != 0 && !buffer_append_varint_field(out, 4, req->discovery_response_port)) return false;
    return true;
}

bool smart_encode_discovery_response(const DiscoveryResponse *req, SmartBuffer *out) {
    const SensorInfo *info = (const SensorInfo *)req;
    smart_buffer_init(out);
    return encode_sensor_info(info, out);
}

bool smart_encode_sensor_reading(const SensorReading *reading, SmartBuffer *out) {
    smart_buffer_init(out);
    return encode_sensor_reading_internal(reading, out);
}

bool smart_encode_client_request(const ClientRequest *req, SmartBuffer *out) {
    smart_buffer_init(out);
    if (req->request_type != 0 && !buffer_append_varint_field(out, 1, (uint64_t)req->request_type)) return false;
    if (!buffer_append_string_field(out, 2, req->target_sensor_id)) return false;
    if (req->value != 0.0 && !buffer_append_double_field(out, 3, req->value)) return false;
    if (req->command_type != 0 && !buffer_append_varint_field(out, 4, (uint64_t)req->command_type)) return false;
    if (!buffer_append_string_field(out, 5, req->text_value)) return false;
    return true;
}

bool smart_encode_client_response(const ClientResponse *resp, SmartBuffer *out) {
    size_t i;
    smart_buffer_init(out);
    if (resp->status != 0 && !buffer_append_varint_field(out, 1, (uint64_t)resp->status)) return false;
    if (!buffer_append_string_field(out, 2, resp->message)) return false;
    for (i = 0; i < resp->sensor_count; ++i) {
        SmartBuffer nested;
        smart_buffer_init(&nested);
        if (!encode_sensor_info((const SensorInfo *)&resp->sensors[i], &nested)) {
            smart_buffer_free(&nested);
            return false;
        }
        if (!buffer_append_key(out, 3, 2) ||
            !buffer_append_varint(out, (uint64_t)nested.len) ||
            !buffer_append(out, nested.data, nested.len)) {
            smart_buffer_free(&nested);
            return false;
        }
        smart_buffer_free(&nested);
    }
    if (resp->metric_value != 0.0 && !buffer_append_double_field(out, 4, resp->metric_value)) return false;
    for (i = 0; i < resp->reading_count; ++i) {
        SmartBuffer nested;
        smart_buffer_init(&nested);
        if (!encode_sensor_reading_internal(&resp->readings[i], &nested)) {
            smart_buffer_free(&nested);
            return false;
        }
        if (!buffer_append_key(out, 5, 2) ||
            !buffer_append_varint(out, (uint64_t)nested.len) ||
            !buffer_append(out, nested.data, nested.len)) {
            smart_buffer_free(&nested);
            return false;
        }
        smart_buffer_free(&nested);
    }
    return true;
}

bool smart_encode_control_command(const ControlCommand *cmd, SmartBuffer *out) {
    smart_buffer_init(out);
    if (cmd->command_type != 0 && !buffer_append_varint_field(out, 1, (uint64_t)cmd->command_type)) return false;
    if (!buffer_append_string_field(out, 2, cmd->sensor_id)) return false;
    if (cmd->value != 0.0 && !buffer_append_double_field(out, 3, cmd->value)) return false;
    if (!buffer_append_string_field(out, 4, cmd->text_value)) return false;
    return true;
}

bool smart_encode_control_response(const ControlResponse *resp, SmartBuffer *out) {
    smart_buffer_init(out);
    return encode_control_response_internal(resp, out);
}

bool smart_parse_discovery_request(DiscoveryRequest *req, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(req, 0, sizeof(*req));
    while (pos < len) {
        uint32_t field_number = 0, wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) return false;
        switch (field_number) {
            case 1: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                req->gateway_id = smart_dup_slice(ptr, n);
                if (!req->gateway_id) return false;
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                req->request_timestamp = smart_dup_slice(ptr, n);
                if (!req->request_timestamp) return false;
                break;
            }
            case 3: {
                bool ok = false;
                if (wire_type != 0) return false;
                req->gateway_udp_port = (uint32_t)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 4: {
                bool ok = false;
                if (wire_type != 0) return false;
                req->discovery_response_port = (uint32_t)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

bool smart_parse_discovery_response(DiscoveryResponse *resp, const uint8_t *data, size_t len) {
    return parse_sensor_info((SensorInfo *)resp, data, len);
}

bool smart_parse_sensor_reading(SensorReading *reading, const uint8_t *data, size_t len) {
    return parse_sensor_reading_internal(reading, data, len);
}

bool smart_parse_client_request(ClientRequest *req, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(req, 0, sizeof(*req));
    while (pos < len) {
        uint32_t field_number = 0, wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) return false;
        switch (field_number) {
            case 1: {
                bool ok = false;
                if (wire_type != 0) return false;
                req->request_type = (ClientRequestType)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                req->target_sensor_id = smart_dup_slice(ptr, n);
                if (!req->target_sensor_id) return false;
                break;
            }
            case 3: {
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &req->value)) return false;
                break;
            }
            case 4: {
                bool ok = false;
                if (wire_type != 0) return false;
                req->command_type = (CommandType)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 5: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                req->text_value = smart_dup_slice(ptr, n);
                if (!req->text_value) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

bool smart_parse_client_response(ClientResponse *resp, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(resp, 0, sizeof(*resp));
    while (pos < len) {
        uint32_t field_number = 0, wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            smart_free_client_response(resp);
            return false;
        }
        switch (field_number) {
            case 1: {
                bool ok = false;
                if (wire_type != 0) return false;
                resp->status = (ResponseStatus)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                resp->message = smart_dup_slice(ptr, n);
                if (!resp->message) return false;
                break;
            }
            case 3: {
                const uint8_t *ptr = NULL; size_t n = 0;
                SensorInfo info;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                if (!parse_sensor_info(&info, ptr, n)) return false;
                if (!append_sensor_info(&resp->sensors, &resp->sensor_count, &info)) {
                    smart_free_discovery_response((DiscoveryResponse *)&info);
                    return false;
                }
                break;
            }
            case 4: {
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &resp->metric_value)) return false;
                break;
            }
            case 5: {
                const uint8_t *ptr = NULL; size_t n = 0;
                SensorReading reading;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                if (!parse_sensor_reading_internal(&reading, ptr, n)) return false;
                if (!append_reading(&resp->readings, &resp->reading_count, &reading)) {
                    smart_free_sensor_reading(&reading);
                    return false;
                }
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

bool smart_parse_control_command(ControlCommand *cmd, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(cmd, 0, sizeof(*cmd));
    while (pos < len) {
        uint32_t field_number = 0, wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) return false;
        switch (field_number) {
            case 1: {
                bool ok = false;
                if (wire_type != 0) return false;
                cmd->command_type = (CommandType)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                cmd->sensor_id = smart_dup_slice(ptr, n);
                if (!cmd->sensor_id) return false;
                break;
            }
            case 3: {
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &cmd->value)) return false;
                break;
            }
            case 4: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                cmd->text_value = smart_dup_slice(ptr, n);
                if (!cmd->text_value) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}

bool smart_parse_control_response(ControlResponse *resp, const uint8_t *data, size_t len) {
    size_t pos = 0;
    memset(resp, 0, sizeof(*resp));
    while (pos < len) {
        uint32_t field_number = 0, wire_type = 0;
        if (!read_key(data, len, &pos, &field_number, &wire_type)) {
            smart_free_control_response(resp);
            return false;
        }
        switch (field_number) {
            case 1: {
                bool ok = false;
                if (wire_type != 0) return false;
                resp->status = (ResponseStatus)read_varint(data, len, &pos, &ok);
                if (!ok) return false;
                break;
            }
            case 2: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                resp->message = smart_dup_slice(ptr, n);
                if (!resp->message) return false;
                break;
            }
            case 3: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                resp->sensor_id = smart_dup_slice(ptr, n);
                if (!resp->sensor_id) return false;
                break;
            }
            case 4: {
                bool ok = false;
                if (wire_type != 0) return false;
                resp->is_active = read_varint(data, len, &pos, &ok) != 0;
                if (!ok) return false;
                break;
            }
            case 5:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &resp->frequency_seconds)) return false;
                break;
            case 6:
                if (wire_type != 1 || !read_fixed64_double(data, len, &pos, &resp->threshold)) return false;
                break;
            case 7: {
                const uint8_t *ptr = NULL; size_t n = 0;
                if (wire_type != 2 || !read_length_delimited(data, len, &pos, &ptr, &n)) return false;
                resp->state_text = smart_dup_slice(ptr, n);
                if (!resp->state_text) return false;
                break;
            }
            default:
                if (!skip_field(data, len, &pos, wire_type)) return false;
                break;
        }
    }
    return true;
}
