#ifndef __WEBRTC_CONFIG_H__
#define __WEBRTC_CONFIG_H__

#include "curllib.h"
#include "log_wrapper.h"

typedef struct 
{
  char* camera_id;
  int   comm_socket_port;
  char* tty_name;
  int tty_buadrate;

  int   max_stream_cnt;
  int   stream_base_port;

  int   device_cnt;
  char* video_src[2];
  char* video_infer[2];
  char* video_enc[2];
  char* video_enc2[2];
  char* record_enc[2];
  char* snapshot_enc[2];

  char* server_ip;
  char* snapshot_path;
  int   status_timer_interval;

  char* device_setting_path;

  char* record_path;
  int   record_duration;
  int   record_enc_index;

  char* http_service_ip;
  int   event_buf_time;
  int   event_record_enc_index;
  int   http_service_port;            //LJH, 241209
} WebRTCConfig;

typedef struct 
{
  int color_pallet;
  int record_onoff;
  int analsys_onoff;
  char ptz_status[8];
} SystemConfig;


gboolean load_config(const char *file_name, WebRTCConfig* config, CurlIinfoType *curl_info);
void free_config(WebRTCConfig* config);
void update_http_service_ip(WebRTCConfig* config);

#endif
