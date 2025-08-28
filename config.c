#include <json-glib/json-glib.h>
#include <string.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <curl/curl.h>
#include "config.h"
#include "nvds_process.h"

// Helper function for safe string extraction from JSON
static inline char* safe_get_string(JsonObject *obj, const char *member) {
    const char *str = json_object_get_string_member(obj, member);
    return str ? g_strdup(str) : NULL;
}

// Helper function for required string fields
static gboolean parse_required_string(JsonObject *obj, const char *key, char **dest) {
    if (!json_object_has_member(obj, key)) {
        glog_trace("Missing required field: %s\n", key);
        return FALSE;
    }
    
    const char *value = json_object_get_string_member(obj, key);
    if (!value) {
        glog_trace("Invalid value for field: %s\n", key);
        return FALSE;
    }
    
    *dest = g_strdup(value);
    glog_trace("Parsed %s: %s\n", key, value);
    return TRUE;
}

// Helper function for required integer fields
static gboolean parse_required_int(JsonObject *obj, const char *key, int *dest) {
    if (!json_object_has_member(obj, key)) {
        glog_trace("Missing required field: %s\n", key);
        return FALSE;
    }
    
    *dest = json_object_get_int_member(obj, key);
    glog_trace("Parsed %s: %d\n", key, *dest);
    return TRUE;
}

// Helper function for optional integer fields
static void parse_optional_int(JsonObject *obj, const char *key, int *dest, int default_value) {
    if (json_object_has_member(obj, key)) {
        *dest = json_object_get_int_member(obj, key);
        glog_trace("Parsed %s: %d\n", key, *dest);
    } else {
        *dest = default_value;
        glog_trace("Using default for %s: %d\n", key, default_value);
    }
}

// Parse video device configuration
static gboolean parse_video_devices(JsonObject *object, WebRTCConfig *config) {
    for (int i = 0; i < config->device_cnt && i < MAX_DEVICES; i++) {
        char video_name[16];
        snprintf(video_name, sizeof(video_name), "video%d", i);
        
        if (!json_object_has_member(object, video_name)) {
            glog_trace("Missing video device config: %s\n", video_name);
            return FALSE;
        }
        
        JsonObject *video_obj = json_object_get_object_member(object, video_name);
        if (!video_obj) {
            glog_trace("Invalid video device config: %s\n", video_name);
            return FALSE;
        }
        
        config->flip_method[i] = json_object_get_int_member(video_obj, "flip_method");
        config->bitrate_high[i] = json_object_get_int_member(video_obj, "bitrate_high");
        config->bitrate_low[i] = json_object_get_int_member(video_obj, "bitrate_low");
        config->model_config[i] = safe_get_string(video_obj, "model_config");
        
        glog_trace("Video%d config - flip: %d, bitrate_high: %d, bitrate_low: %d, model: %s\n",
                   i, config->flip_method[i], config->bitrate_high[i], 
                   config->bitrate_low[i], config->model_config[i] ? config->model_config[i] : "NULL");
    }
    return TRUE;
}

// Parse curl/event server configuration
static gboolean parse_curl_config(JsonObject *object, CurlIinfoType *curl_info, const char *snapshot_path) {
    const char *user_id, *password, *server_ip;
    
    // Get string values from JSON
    if (!json_object_has_member(object, "event_user_id") ||
        !json_object_has_member(object, "event_user_pw") ||
        !json_object_has_member(object, "event_server_ip")) {
        glog_trace("Missing required curl configuration fields\n");
        return FALSE;
    }
    
    user_id = json_object_get_string_member(object, "event_user_id");
    password = json_object_get_string_member(object, "event_user_pw");
    server_ip = json_object_get_string_member(object, "event_server_ip");
    
    if (!user_id || !password || !server_ip) {
        glog_trace("Invalid curl configuration values\n");
        return FALSE;
    }
    
    // Copy to fixed-size arrays safely
    g_strlcpy(curl_info->phone, user_id, sizeof(curl_info->phone));
    g_strlcpy(curl_info->password, password, sizeof(curl_info->password));
    g_strlcpy(curl_info->server_ip, server_ip, sizeof(curl_info->server_ip));
    g_strlcpy(curl_info->snapshot_path, snapshot_path, sizeof(curl_info->snapshot_path));
    
    curl_info->port = 0;
    
    glog_trace("Curl config - user: %s, server: %s\n", user_id, server_ip);
    
    return TRUE;
}

gboolean load_config(const char *file_name, WebRTCConfig *config, CurlIinfoType *curl_info) {
    JsonParser *parser = NULL;
    JsonReader *reader = NULL;
    gboolean result = FALSE;
    GError *error = NULL;
    
    // Initialize parser
    parser = json_parser_new();
    if (!json_parser_load_from_file(parser, file_name, &error)) {
        glog_trace("Unable to parse file '%s': %s\n", file_name, error->message);
        g_error_free(error);
        goto cleanup;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        glog_trace("JSON root is not an object\n");
        goto cleanup;
    }
    
    JsonObject *object = json_node_get_object(root);
    reader = json_reader_new(root);
    // Parse required basic configuration
    if (!parse_required_string(object, "camera_id", &config->camera_id) ||
        !parse_required_int(object, "max_stream_cnt", &config->max_stream_cnt) ||
        !parse_required_int(object, "stream_base_port", &config->stream_base_port) ||
        !parse_required_int(object, "device_cnt", &config->device_cnt) ||
        !parse_required_int(object, "comm_socket_port", &config->comm_socket_port)) {
        goto cleanup;
    }
    
    // Parse TTY configuration
    if (!json_object_has_member(object, "tty")) {
        glog_trace("Missing required field: tty\n");
        goto cleanup;
    }
    
    JsonObject *tty_obj = json_object_get_object_member(object, "tty");
    if (!tty_obj) {
        glog_trace("Invalid tty configuration\n");
        goto cleanup;
    }
    
    if (!parse_required_string(tty_obj, "name", &config->tty_name) ||
        !parse_required_int(tty_obj, "baudrate", &config->tty_baudrate)) {
        goto cleanup;
    }

    // Parse video device configurations
    if (!parse_video_devices(object, config)) {
        goto cleanup;
    }

    // Parse server and path configuration
    if (!parse_required_string(object, "server_ip", &config->server_ip) ||
        !parse_required_string(object, "snapshot_path", &config->snapshot_path) ||
        !parse_required_int(object, "status_timer_interval", &config->status_timer_interval) ||
        !parse_required_string(object, "device_setting_path", &config->device_setting_path)) {
        goto cleanup;
    }

    // Parse curl/event server configuration
    if (!parse_curl_config(object, curl_info, config->snapshot_path)) {
        goto cleanup;
    }

    // Parse recording configuration
    if (!parse_required_string(object, "record_path", &config->record_path) ||
        !parse_required_int(object, "record_duration", &config->record_duration) ||
        !parse_required_int(object, "event_buf_time", &config->event_buf_time)) {
        goto cleanup;
    }
    
    // Parse optional recording settings
    parse_optional_int(object, "record_enc_index", &config->record_enc_index, 0);
    parse_optional_int(object, "event_record_enc_index", &config->event_record_enc_index, 0);
    
    // Parse HTTP service port (try as string first, then as int)
    if (json_object_has_member(object, "http_service_port")) {
        // Try to get as string first (common in JSON configs)
        const char *port_str = json_object_get_string_member(object, "http_service_port");
        if (port_str) {
            config->http_service_port = atoi(port_str);
        } else {
            // If not a string, try as integer
            config->http_service_port = json_object_get_int_member(object, "http_service_port");
        }
        glog_trace("Parsed http_service_port: %d\n", config->http_service_port);
    } else {
        config->http_service_port = 0;
        glog_trace("Using default http_service_port: 0\n");
    }

    // Update HTTP service IP
    update_http_service_ip(config);
    
    result = TRUE;
    glog_trace("Configuration loaded successfully\n");
    
cleanup:
    if (reader) g_object_unref(reader);
    if (parser) g_object_unref(parser);
    
    return result;
}

void free_config(WebRTCConfig *config) {
    if (!config) return;
    
    // Free string fields safely
    g_free(config->camera_id);
    g_free(config->tty_name);
    g_free(config->server_ip);
    g_free(config->snapshot_path);
    g_free(config->device_setting_path);
    g_free(config->record_path);
    g_free(config->http_service_ip);
    
    // Free model config arrays
    for (int i = 0; i < MAX_DEVICES; i++) {
        g_free(config->model_config[i]);
        config->model_config[i] = NULL;
    }
    
    // Clear all pointers
    memset(config, 0, sizeof(WebRTCConfig));
}

// Callback function to handle HTTP response (for IP detection)
static size_t write_callback(void *ptr, size_t size, size_t nmemb, char *data) {
    size_t total_size = size * nmemb;
    size_t current_len = strlen(data);
    
    // Prevent buffer overflow
    if (current_len + total_size < IP_BUFFER_SIZE - 1) {
        strncat(data, ptr, total_size);
    }
    
    return total_size;
}

static void get_local_ip(char *ip_str) {
    struct ifaddrs *ifap = NULL;
    
    if (getifaddrs(&ifap) == -1) {
        glog_trace("Failed to get network interfaces: %s\n", strerror(errno));
        strcpy(ip_str, "127.0.0.1");  // Fallback to localhost
        return;
    }
    
    // Find first non-loopback IPv4 interface
    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char *ip_address = inet_ntoa(sa->sin_addr);
            
            // Skip loopback and invalid interfaces
            if (strcmp(ifa->ifa_name, "lo") != 0 && ip_address) {
                glog_trace("Found local IP on interface %s: %s\n", ifa->ifa_name, ip_address);
                strncpy(ip_str, ip_address, IP_BUFFER_SIZE - 1);
                ip_str[IP_BUFFER_SIZE - 1] = '\0';
                break;
            }
        }
    }
    
    freeifaddrs(ifap);
}

static void write_lines_to_file(const char *filename, const char *line1, const char *line2) {
    FILE *file = fopen(filename, "w");
    
    if (!file) {
        glog_trace("Failed to open file %s: %s\n", filename, strerror(errno));
        return;
    }
    
    fprintf(file, "%s\n%s\n", line1, line2);
    fclose(file);
    
    glog_trace("IP information written to %s\n", filename);
}

char* get_global_ip_with_timeout(void) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        glog_trace("Failed to initialize CURL\n");
        return NULL;
    }
    
    char *ip = g_malloc0(IP_BUFFER_SIZE);
    
    // Configure CURL with timeouts
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_URL, "http://ifconfig.me/ip");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, ip);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "webrtc-client/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        glog_trace("Failed to get external IP: %s\n", curl_easy_strerror(res));
        g_free(ip);
        return NULL;
    }
    
    // Remove trailing whitespace
    g_strstrip(ip);
    
    if (strlen(ip) == 0) {
        g_free(ip);
        return NULL;
    }
    
    glog_trace("Retrieved external IP: %s\n", ip);
    return ip;
}

static gpointer update_external_ip_thread(gpointer data) {
    WebRTCConfig *config = (WebRTCConfig *)data;
    char *global_ip = get_global_ip_with_timeout();
    
    if (global_ip) {
        char *new_service_ip = g_strdup_printf("%s:%d", global_ip, config->http_service_port);
        
        // Thread-safe update (consider adding mutex if needed)
        g_free(config->http_service_ip);
        config->http_service_ip = new_service_ip;
        
        glog_trace("Updated http_service_ip to external: %s\n", config->http_service_ip);
        
        // Write IP information to log file
        char local_ip[IP_BUFFER_SIZE];
        get_local_ip(local_ip);
        write_lines_to_file("local_ip.log", local_ip, new_service_ip);
        
        g_free(global_ip);
    } else {
        glog_trace("Failed to retrieve external IP, keeping local IP\n");
    }
    
    return NULL;
}

void update_http_service_ip(WebRTCConfig *config) {
    if (!config) return;
    
    char local_ip[IP_BUFFER_SIZE];
    get_local_ip(local_ip);
    
    // Set initial IP to local address
    g_free(config->http_service_ip);
    config->http_service_ip = g_strdup_printf("%s:%d", local_ip, config->http_service_port);
    
    glog_trace("Initial http_service_ip set to local: %s\n", config->http_service_ip);
    
    // Update external IP in background thread
    GThread *thread = g_thread_new("external-ip-update", update_external_ip_thread, config);
    if (thread) {
        g_thread_unref(thread);  // Don't wait for completion
    }
}

