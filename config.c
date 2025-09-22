/* For json config */
#include <json-glib/json-glib.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <curl/curl.h>
#include "config.h"
#include "nvds_process.h"

extern int get_service_address(char *address);

static inline char* safe_get_string(JsonObject *obj, const char *member) {
    const char *str = json_object_get_string_member(obj, member);
    return str ? g_strdup(str) : NULL;
}

int load_config(const char *file_name, WebRTCConfig *config, CurlIinfoType *curl_info)
{
    JsonParser *parser;
    GError *error;
    JsonNode *root;
    JsonObject *object, *child;

    parser = json_parser_new();
    error = NULL;
    json_parser_load_from_file(parser, file_name, &error);
    if (error)
    {
        glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
        g_error_free(error);
        g_object_unref(parser);
        return FALSE;
    }

    JsonReader *reader = json_reader_new(json_parser_get_root(parser));
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_object_unref(reader);  // Fix memory leak
        g_object_unref(parser);
        return FALSE;
    }

    object = json_node_get_object(root);
    if (json_object_has_member(object, "camera_id"))
    {
        const char *value = json_object_get_string_member(object, "camera_id");
        if (value && strlen(value) > 0 && strlen(value) < 256) {  // Input validation
            glog_trace("parse member %s : %s\n", "camera_id", value);
            config->camera_id = strdup(value);
        } else {
            glog_trace("Invalid camera_id value\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "tty"))
    {
        child = json_object_get_object_member(object, "tty");
        const char *value = json_object_get_string_member(child, "name");
        int value2 = json_object_get_int_member(child, "baudrate");
        if (value && strlen(value) > 0 && strlen(value) < 256) {  // Input validation
            glog_trace("parse member %s : %s, %d\n", "tty", value, value2);
            config->tty_name = strdup(value);
            config->tty_buadrate = value2;
        } else {
            glog_trace("Invalid tty name\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "max_stream_cnt"))
    {
        int value = json_object_get_int_member(object, "max_stream_cnt");
        glog_trace("parse member %s : %d\n", "max_stream_cnt", value);
        config->max_stream_cnt = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "stream_base_port"))
    {
        int value = json_object_get_int_member(object, "stream_base_port");
        glog_trace("parse member %s : %d\n", "stream_base_port", value);
        config->stream_base_port = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "device_cnt"))
    {
        int value = json_object_get_int_member(object, "device_cnt");
        glog_trace("parse member %s : %d\n", "device_cnt", value);
        config->device_cnt = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "comm_socket_port"))
    {
        int value = json_object_get_int_member(object, "comm_socket_port");
        glog_trace("parse member %s : %d\n", "device_cnt", value);
        config->comm_socket_port = value;
    }
    else
    {
        return FALSE;
    }

    // Array bounds protection
    if (config->device_cnt > 10 || config->device_cnt < 0) {
        glog_trace("Invalid device_cnt: %d\n", config->device_cnt);
        g_object_unref(reader);
        g_object_unref(parser);
        return FALSE;
    }
    
    for (int i = 0; i < config->device_cnt; i++)
    {
        char video_name[8] = "videox";  // Increased buffer size
        if (i > 9) break;  // Safety check
        video_name[5] = '0' + i;
        if (json_object_has_member(object, video_name))
        {
            child = json_object_get_object_member(object, video_name);
            
            config->flip_method[i] = json_object_get_int_member(child, "flip_method");
            config->bitrate_high[i] = json_object_get_int_member(child, "bitrate_high");
            config->bitrate_low[i] = json_object_get_int_member(child, "bitrate_low");
            config->model_config[i] = safe_get_string(child, "model_config");

            glog_trace("parse member %s : %d, %d, %d, %s\n",
                       video_name, config->flip_method[i],
                       config->bitrate_high[i], config->bitrate_low[i],
                       config->model_config[i] ? config->model_config[i] : "NULL");
        }
        else
        {
            return FALSE;
        }
    }

    if (json_object_has_member(object, "server_ip"))
    {
        const char *value = json_object_get_string_member(object, "server_ip");
        if (value && strlen(value) > 0 && strlen(value) < 256) {
            glog_trace("parse member %s : %s\n", "server_ip", value);
            config->server_ip = strdup(value);
        } else {
            glog_trace("Invalid server_ip\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "snapshot_path"))
    {
        const char *value = json_object_get_string_member(object, "snapshot_path");
        if (value && strlen(value) > 0 && strlen(value) < 512) {
            glog_trace("parse member %s : %s\n", "snapshot_path", value);
            config->snapshot_path = strdup(value);
        } else {
            glog_trace("Invalid snapshot_path\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "status_timer_interval"))
    {
        int value = json_object_get_int_member(object, "status_timer_interval");
        glog_trace("parse member %s : %d\n", "status_timer_interval", value);
        config->status_timer_interval = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "device_setting_path"))
    {
        const char *value = json_object_get_string_member(object, "device_setting_path");
        if (value && strlen(value) > 0 && strlen(value) < 512) {
            glog_trace("parse member %s : %s\n", "device_setting_path", value);
            config->device_setting_path = strdup(value);
        } else {
            glog_trace("Invalid device_setting_path\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    // event server connection info
    if (json_object_has_member(object, "event_user_id"))
    {
        const char *value = json_object_get_string_member(object, "event_user_id");
        if (value && strlen(value) < sizeof(curl_info->phone)) {
            glog_trace("parse member %s : %s\n", "event_user_id", value);
            strncpy(curl_info->phone, value, sizeof(curl_info->phone) - 1);
            curl_info->phone[sizeof(curl_info->phone) - 1] = '\0';
        } else {
            glog_trace("Invalid or too long event_user_id\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "event_user_pw"))
    {
        const char *value = json_object_get_string_member(object, "event_user_pw");
        if (value && strlen(value) < sizeof(curl_info->password)) {
            // Security: Don't log password in plaintext
            glog_trace("parse member %s : [MASKED]\n", "event_user_pw");
            strncpy(curl_info->password, value, sizeof(curl_info->password) - 1);
            curl_info->password[sizeof(curl_info->password) - 1] = '\0';
        } else {
            glog_trace("Invalid or too long event_user_pw\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "event_server_ip"))
    {
        const char *value = json_object_get_string_member(object, "event_server_ip");
        if (value && strlen(value) < sizeof(curl_info->server_ip)) {
            glog_trace("parse member %s : %s\n", "event_server_ip", value);
            strncpy(curl_info->server_ip, value, sizeof(curl_info->server_ip) - 1);
            curl_info->server_ip[sizeof(curl_info->server_ip) - 1] = '\0';
            curl_info->port = 0;
        } else {
            glog_trace("Invalid or too long event_server_ip\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    // Safe string copy with bounds checking
    if (config->snapshot_path && strlen(config->snapshot_path) < sizeof(curl_info->snapshot_path)) {
        strncpy(curl_info->snapshot_path, config->snapshot_path, sizeof(curl_info->snapshot_path) - 1);
        curl_info->snapshot_path[sizeof(curl_info->snapshot_path) - 1] = '\0';
    } else {
        glog_trace("Invalid or too long snapshot_path\n");
        curl_info->snapshot_path[0] = '\0';  // Set empty string as fallback
    }

    // record infomation
    if (json_object_has_member(object, "record_path"))
    {
        const char *value = json_object_get_string_member(object, "record_path");
        if (value && strlen(value) > 0 && strlen(value) < 512) {
            glog_trace("parse member %s : %s\n", "record_path", value);
            config->record_path = strdup(value);
        } else {
            glog_trace("Invalid record_path\n");
            g_object_unref(reader);
            g_object_unref(parser);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "record_duration"))
    {
        int value = json_object_get_int_member(object, "record_duration");
        glog_trace("parse member %s : %d\n", "record_duration", value);
        config->record_duration = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "event_buf_time"))
    {
        int value = json_object_get_int_member(object, "event_buf_time");
        glog_trace("parse member %s : %d\n", "event_buf_time", value);
        config->event_buf_time = value;
    }
    else
    {
        return FALSE;
    }

    if (json_object_has_member(object, "record_enc_index"))
    {
        int value = json_object_get_int_member(object, "record_enc_index");
        glog_trace("parse member %s : %d\n", "record_enc_index", value);
        config->record_enc_index = value;
    }
    else
    {
        config->record_enc_index = 0;
    }

    if (json_object_has_member(object, "event_record_enc_index"))
    {
        int value = json_object_get_int_member(object, "event_record_enc_index");
        glog_trace("parse member %s : %d\n", "event_record_enc_index", value);
        config->event_record_enc_index = value;
    }
    else
    {
        config->event_record_enc_index = 0;
    }

    if (json_object_has_member(object, "http_service_port"))
    {
        glog_trace("http_service_port\n");
        const char *value = json_object_get_string_member(object, "http_service_port");
        config->http_service_port = atoi(value);
        glog_trace("parse member %s : %d\n", "http_service_port", config->http_service_port);
    }
    else
    {
        config->http_service_port = 0;
    }

    g_object_unref(reader);
    g_object_unref(parser);

    return TRUE;
}

void free_config(WebRTCConfig *config)
{
    free(config->camera_id);
    free(config->tty_name);
    for (int i = 0; i < 2; i++)
    {
        free(config->model_config[i]);
    }
    free(config->server_ip);
    free(config->snapshot_path);
    free(config->device_setting_path);
    free(config->http_service_ip);
}

// Callback function to handle the response with buffer overflow protection
size_t write_callback(void *ptr, size_t size, size_t nmemb, char *data)
{
    size_t total_size = size * nmemb;
    size_t current_len = strlen(data);
    const size_t MAX_RESPONSE_SIZE = 99;  // Reserve 1 byte for null terminator
    
    // Buffer overflow protection
    if (current_len + total_size >= MAX_RESPONSE_SIZE) {
        size_t available_space = MAX_RESPONSE_SIZE - current_len;
        if (available_space > 0) {
            strncat(data, (char*)ptr, available_space);
            data[MAX_RESPONSE_SIZE] = '\0';
        }
        return total_size;  // Return original size to avoid curl errors
    }
    
    strncat(data, (char*)ptr, total_size);
    return total_size;
}

char* get_global_ip_robust() {
    // 여러 외부 IP 확인 서비스를 배열로 관리
    const char* ip_services[] = {
        "http://ifconfig.me/ip",
        "http://api.ipify.org",
        "http://icanhazip.com",
        NULL
    };
    
    CURL *curl;
    char *ip = (char *)malloc(100);
    if (!ip) return NULL;

    // curl 전역 초기화 (한 번만 수행)
    static int curl_initialized = 0;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = 1;
    }

    for (int i = 0; ip_services[i] != NULL; i++) {
        for (int retry = 0; retry < 3; retry++) { // 각 서비스마다 3번 재시도
            ip[0] = '\0';
            curl = curl_easy_init();
            if (curl) {
                curl_easy_setopt(curl, CURLOPT_URL, ip_services[i]);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, ip);
                // DNS 캐시 타임아웃 설정
                curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
                // Follow redirects
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
                
                glog_trace("Attempting to get external IP from %s (attempt %d)...\n", ip_services[i], retry + 1);
                CURLcode res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);
                
                if (res == CURLE_OK && strlen(ip) > 0) {
                    // Remove any trailing whitespace/newline from IP
                    char *newline = strchr(ip, '\n');
                    if (newline) *newline = '\0';
                    newline = strchr(ip, '\r');
                    if (newline) *newline = '\0';
                    
                    // Validate IP format (basic check)
                    if (strlen(ip) >= 7 && strlen(ip) <= 15) {
                        glog_trace("Successfully got external IP: %s\n", ip);
                        return ip; // 성공 시 즉시 반환
                    } else {
                        glog_trace("Invalid IP format received: %s\n", ip);
                    }
                } else {
                    glog_trace("Failed attempt %d from %s (error: %s)\n", 
                              retry + 1, ip_services[i], 
                              res != CURLE_OK ? curl_easy_strerror(res) : "empty response");
                }
                
                if (retry < 2) { // 마지막 시도가 아니면 대기
                    g_usleep(500000); // 0.5초 대기 후 재시도
                }
            }
        }
    }
    
    // 모든 시도가 실패한 경우
    free(ip);
    glog_error("Failed to get external IP from all services.\n");
    return NULL;
}

void get_local_ip(char *ip_str)
{
    struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;
    char *ip_address = NULL;

    // Get the list of network interfaces
    if (getifaddrs(&ifap) == -1)
    {
        perror("getifaddrs");
        return;
    }

    // Iterate over the interfaces
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next)
    {
        // Check if the interface is an IPv4 address (with NULL check)
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)
        {
            sa = (struct sockaddr_in *)ifa->ifa_addr;
            ip_address = inet_ntoa(sa->sin_addr);
            // We check if the interface is not a loopback interface (lo)
            if (strcmp(ifa->ifa_name, "lo") != 0)
            { // Exclude loopback
                printf("Local IP address of interface %s: %s\n", ifa->ifa_name, ip_address);
                // Safe string copy with bounds checking
                strncpy(ip_str, ip_address, 99);  // Assume ip_str is at least 100 bytes
                ip_str[99] = '\0';
                break; // Print the first non-loopback IP address
            }
        }
    }
    // Free the memory allocated by getifaddrs
    freeifaddrs(ifap);
}

void write_lines_to_file(const char *filename, char *line1, char *line2)
{
    FILE *file = fopen(filename, "w"); // Open the file in write mode ("w")

    if (file == NULL)
    {
        perror("Failed to open file");
        return;
    }

    // Write each line to the file
    fprintf(file, "%s\n", line1); // Write line with newline
    fprintf(file, "%s\n", line2); // Write line with newline

    fclose(file); // Close the file
    printf("Lines written to file successfully.\n");
}

char *get_global_ip_with_timeout()
{
    CURL *curl;
    char *ip = (char *)malloc(100);
    ip[0] = '\0';
    
    curl = curl_easy_init();
    if (curl) {
        // 타임아웃 설정 추가
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        
        // HTTP 사용 (HTTPS보다 빠름)
        curl_easy_setopt(curl, CURLOPT_URL, "http://ifconfig.me/ip");
        
        // 나머지는 기존 코드와 동일
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, ip);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            free(ip);
            ip = NULL;
        }
        
        curl_easy_cleanup(curl);
    }
    
    return ip;
}

static gpointer update_external_ip_thread(gpointer data)
{
    WebRTCConfig *config = (WebRTCConfig *)data;
    char *global_ip = get_global_ip_robust(); // 새로운 함수 호출
    
    if (global_ip) {
        char *new_ip = g_strdup_printf("%s:%d", global_ip, config->http_service_port);
        
        // 스레드 안전하게 IP 주소 교체 (race condition 방지)
        gchar *old_ip = g_atomic_pointer_get(&config->http_service_ip);
        g_atomic_pointer_set(&config->http_service_ip, new_ip);
        g_free(old_ip);
        
        glog_trace("Updated http_service_ip to external (async): %s\n", new_ip);
        
        char local_ip[100];
        get_local_ip(local_ip);
        write_lines_to_file("local_ip.log", local_ip, new_ip);
        
        free(global_ip);
    } else {
        glog_info("Failed to get external IP in background thread. Keeping local IP.\n");
    }
    
    return NULL;
}

void update_http_service_ip(WebRTCConfig *config, gboolean is_async)
{
    char local_ip[100];
    get_local_ip(local_ip);
    
    // 이전 IP 메모리 해제
    if (config->http_service_ip) {
        free(config->http_service_ip);
    }
    // 우선 로컬 IP로 설정
    config->http_service_ip = g_strdup_printf("%s:%d", local_ip, config->http_service_port);
    glog_trace("Initial http_service_ip set to local: %s", config->http_service_ip);

    if (is_async) {
        // 비동기 모드: 백그라운드에서 외부 IP 업데이트
        g_thread_new("external-ip-update-async", update_external_ip_thread, config);
    } else {
        // 동기 모드: 외부 IP를 가져올 때까지 기다림
        char *global_ip = get_global_ip_robust();
        if (global_ip) {
            free(config->http_service_ip); // 로컬 IP 설정 해제
            config->http_service_ip = g_strdup_printf("%s:%d", global_ip, config->http_service_port);
            glog_trace("Updated http_service_ip to external (sync): %s", config->http_service_ip);
            write_lines_to_file("local_ip.log", local_ip, config->http_service_ip);
            free(global_ip);
        } else {
            glog_error("Failed to get external IP synchronously. Using local IP as fallback.");
        }
    }
}

/*
int main(int argc, char *argv[])
{
  WebRTCConfig config ={};
  load_config("config.json", &config);

  glog_trace("camera id  %d \n", config.camera_id);
  glog_trace("tty name  %s \n", config.tty_name);
  for(int i = 0 ; i < 2 ; i++){
    glog_trace("video config %d : %s, %d\n", i , config.video_src[i], config.video_bitrate[i]);
  }

  free_config(&config);
}
*/

