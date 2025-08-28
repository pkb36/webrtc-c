#ifndef __WEBRTC_CONFIG_H__
#define __WEBRTC_CONFIG_H__

#include "curllib.h"
#include "logging.h"

#define MAX_DEVICES 2
#define PTZ_STATUS_SIZE 8
#define IP_BUFFER_SIZE 100

typedef struct {
    // Device identification
    char* camera_id;
    
    // Communication settings
    int comm_socket_port;
    char* tty_name;
    int tty_baudrate;  // Fixed typo: buadrate -> baudrate
    
    // Stream configuration
    int max_stream_cnt;
    int stream_base_port;
    int device_cnt;
    
    // Video settings per device
    int flip_method[MAX_DEVICES];     // 0: none, 1: horizontal, 2: vertical, 3: both
    int bitrate_high[MAX_DEVICES];
    int bitrate_low[MAX_DEVICES];
    char* model_config[MAX_DEVICES];
    
    // Server configuration
    char* server_ip;
    int status_timer_interval;
    
    // File paths
    char* snapshot_path;
    char* device_setting_path;
    char* record_path;
    
    // Recording settings
    int record_duration;
    int record_enc_index;
    int event_record_enc_index;
    
    // HTTP service settings
    char* http_service_ip;
    int http_service_port;
    int event_buf_time;
} WebRTCConfig;

typedef struct {
    int color_pallet;
    int record_onoff;
    int analysis_onoff;  // Fixed typo: analsys -> analysis
    char ptz_status[PTZ_STATUS_SIZE];
} SystemConfig;


gboolean load_config(const char *file_name, WebRTCConfig* config, CurlIinfoType *curl_info);
void free_config(WebRTCConfig* config);
void update_http_service_ip(WebRTCConfig* config);
char* get_global_ip_with_timeout(void);

#endif
