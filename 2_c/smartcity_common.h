#ifndef SMARTCITY_COMMON_H
#define SMARTCITY_COMMON_H

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#ifdef ERROR
#undef ERROR
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef int SOCKET;
typedef unsigned short WORD;
typedef unsigned short u_short;
typedef unsigned long DWORD;
typedef void *LPVOID;
typedef void *HANDLE;
typedef int BOOL;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define WINAPI
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define MAKEWORD(a, b) ((WORD)(((a) & 0xff) | (((b) & 0xff) << 8)))

typedef struct {
    int unused;
} WSADATA;

static inline int WSAStartup(WORD version, WSADATA *data) {
    (void)version;
    (void)data;
    return 0;
}

static inline int WSACleanup(void) {
    return 0;
}

static inline int closesocket(SOCKET sock) {
    return close(sock);
}

static inline void Sleep(unsigned long ms) {
    usleep(ms * 1000);
}

typedef pthread_mutex_t CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION *lock) {
    pthread_mutex_init(lock, NULL);
}

static inline void DeleteCriticalSection(CRITICAL_SECTION *lock) {
    pthread_mutex_destroy(lock);
}

static inline void EnterCriticalSection(CRITICAL_SECTION *lock) {
    pthread_mutex_lock(lock);
}

static inline void LeaveCriticalSection(CRITICAL_SECTION *lock) {
    pthread_mutex_unlock(lock);
}

typedef DWORD (*SmartThreadProc)(LPVOID);

typedef struct {
    SmartThreadProc proc;
    LPVOID param;
} SmartThreadStart;

static inline void *smart_thread_entry(void *arg) {
    SmartThreadStart *start = (SmartThreadStart *)arg;
    SmartThreadProc proc = start->proc;
    LPVOID param = start->param;
    free(start);
    (void)proc(param);
    return NULL;
}

static inline HANDLE smart_create_thread(SmartThreadProc proc, LPVOID param) {
    pthread_t thread;
    SmartThreadStart *start = (SmartThreadStart *)malloc(sizeof(*start));
    if (!start) {
        return NULL;
    }
    start->proc = proc;
    start->param = param;
    if (pthread_create(&thread, NULL, smart_thread_entry, start) != 0) {
        free(start);
        return NULL;
    }
    pthread_detach(thread);
    return NULL;
}

#define CreateThread(security, stack, start, param, flags, id) smart_create_thread((SmartThreadProc)(start), (param))
#endif

#define SMARTCITY_GATEWAY_ID "gateway-1"
#define SMARTCITY_GATEWAY_TCP_HOST "0.0.0.0"
#define SMARTCITY_GATEWAY_TCP_PORT 9000
#define SMARTCITY_GATEWAY_UDP_HOST "0.0.0.0"
#define SMARTCITY_GATEWAY_UDP_PORT 9001
#define SMARTCITY_MULTICAST_GROUP "224.1.1.1"
#define SMARTCITY_MULTICAST_PORT 10000
#define SMARTCITY_DISCOVERY_RESPONSE_PORT 10001

#define SMARTCITY_STALE_GRACE_SECONDS 10.0

typedef enum {
    DEVICE_KIND_UNSPECIFIED = 0,
    SENSOR = 1,
    ACTUATOR = 2,
} DeviceKind;

typedef enum {
    DEVICE_TYPE_UNSPECIFIED = 0,
    TEMPERATURE_SENSOR = 1,
    AIR_QUALITY_SENSOR = 2,
    CAMERA = 3,
    TRAFFIC_LIGHT = 4,
    STREET_LIGHT = 5,
    NOISE_SENSOR = 6,
} DeviceType;

typedef enum {
    COMMAND_TYPE_UNSPECIFIED = 0,
    ACTIVATE = 1,
    DEACTIVATE = 2,
    SET_FREQUENCY = 3,
    SET_THRESHOLD = 4,
    CAMERA_START_RECORDING = 5,
    CAMERA_STOP_RECORDING = 6,
    CAMERA_SET_DIRECTION = 7,
    TRAFFIC_LIGHT_SET_COLOR = 8,
    TRAFFIC_LIGHT_SET_MODE = 9,
    STREET_LIGHT_SET_BRIGHTNESS = 10,
} CommandType;

typedef enum {
    REQUEST_TYPE_UNSPECIFIED = 0,
    LIST_SENSORS = 1,
    AVG_TEMPERATURE = 2,
    AVG_CO2 = 3,
    MAX_READING = 4,
    ACTIVATE_SENSOR = 5,
    DEACTIVATE_SENSOR = 6,
    CHANGE_FREQUENCY = 7,
    CHANGE_THRESHOLD = 8,
    EXIT = 9,
    AVG_HUMIDITY = 10,
    READING_HISTORY = 11,
    SEND_CONTROL_COMMAND = 12,
} ClientRequestType;

typedef enum {
    RESPONSE_STATUS_UNSPECIFIED = 0,
    OK = 1,
    ERROR = 2,
} ResponseStatus;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} SmartBuffer;

typedef struct {
    char *gateway_id;
    char *request_timestamp;
    uint32_t gateway_udp_port;
    uint32_t discovery_response_port;
} DiscoveryRequest;

typedef struct {
    char *sensor_id;
    DeviceType sensor_type;
    char *sensor_ip;
    uint32_t control_tcp_port;
    bool is_active;
    double frequency_seconds;
    double threshold;
    DeviceKind device_kind;
    char *state_text;
} DiscoveryResponse;

typedef struct {
    char *sensor_id;
    DeviceType sensor_type;
    double value;
    char *unit;
    int64_t timestamp_unix_ms;
    bool alert;
    char *alert_message;
    char *metric;
} SensorReading;

typedef struct {
    ClientRequestType request_type;
    char *target_sensor_id;
    double value;
    CommandType command_type;
    char *text_value;
} ClientRequest;

typedef struct {
    char *sensor_id;
    DeviceType sensor_type;
    char *sensor_ip;
    uint32_t control_tcp_port;
    bool is_active;
    double frequency_seconds;
    double threshold;
    DeviceKind device_kind;
    char *state_text;
} SensorInfo;

typedef struct {
    ResponseStatus status;
    char *message;
    SensorInfo *sensors;
    size_t sensor_count;
    double metric_value;
    SensorReading *readings;
    size_t reading_count;
} ClientResponse;

typedef struct {
    CommandType command_type;
    char *sensor_id;
    double value;
    char *text_value;
} ControlCommand;

typedef struct {
    ResponseStatus status;
    char *message;
    char *sensor_id;
    bool is_active;
    double frequency_seconds;
    double threshold;
    char *state_text;
} ControlResponse;

void smart_buffer_init(SmartBuffer *buf);
void smart_buffer_free(SmartBuffer *buf);
bool smart_send_all(SOCKET sock, const uint8_t *data, size_t size);
bool smart_recv_exact(SOCKET sock, uint8_t *out, size_t size);
bool smart_send_msg(SOCKET sock, const uint8_t *payload, uint32_t payload_len);
bool smart_recv_msg(SOCKET sock, uint8_t **payload, uint32_t *payload_len);
char *smart_dup_cstring(const char *src);
char *smart_dup_slice(const uint8_t *data, size_t len);
void smart_trim_newline(char *s);
bool smart_read_line(const char *prompt, char *buf, size_t size);
bool smart_parse_int_line(const char *text, int *out);
bool smart_parse_double_line(const char *text, double *out);
char *smart_get_local_ip(const char *remote_ip);
int64_t smart_unix_time_ms(void);

void smart_free_discovery_response(DiscoveryResponse *resp);
void smart_free_discovery_request(DiscoveryRequest *req);
void smart_free_sensor_reading(SensorReading *reading);
void smart_free_client_response(ClientResponse *resp);
void smart_free_control_response(ControlResponse *resp);

bool smart_encode_discovery_request(const DiscoveryRequest *req, SmartBuffer *out);
bool smart_encode_discovery_response(const DiscoveryResponse *req, SmartBuffer *out);
bool smart_encode_sensor_reading(const SensorReading *reading, SmartBuffer *out);
bool smart_encode_client_request(const ClientRequest *req, SmartBuffer *out);
bool smart_encode_client_response(const ClientResponse *resp, SmartBuffer *out);
bool smart_encode_control_command(const ControlCommand *cmd, SmartBuffer *out);
bool smart_encode_control_response(const ControlResponse *resp, SmartBuffer *out);

bool smart_parse_discovery_request(DiscoveryRequest *req, const uint8_t *data, size_t len);
bool smart_parse_discovery_response(DiscoveryResponse *resp, const uint8_t *data, size_t len);
bool smart_parse_sensor_reading(SensorReading *reading, const uint8_t *data, size_t len);
bool smart_parse_client_request(ClientRequest *req, const uint8_t *data, size_t len);
bool smart_parse_client_response(ClientResponse *resp, const uint8_t *data, size_t len);
bool smart_parse_control_command(ControlCommand *cmd, const uint8_t *data, size_t len);
bool smart_parse_control_response(ControlResponse *resp, const uint8_t *data, size_t len);

void smart_free_client_request(ClientRequest *req);
void smart_free_control_command(ControlCommand *cmd);
double smart_now_seconds(void);
void smart_iso_utc_timestamp(char *buf, size_t size);

#endif
