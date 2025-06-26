#ifndef __EVENT_RECORDER_H__
#define __EVENT_RECORDER_H__

#include "device_setting.h"
#include <glib.h>
#include <sys/time.h>
#include <pthread.h>

typedef enum {
	SENDER = 0,
	RECORDER,
	EVENT_RECORDER,
} UDPClientProcess;

typedef enum {
	RGB_CAM = 0,
	THERMAL_CAM,
	NUM_CAMS,
} CameraDevice;

typedef enum {
	MAIN_STREAM = 0,
	SECOND_STREAM,
	THIRD_STREAM,
	FORTH_STREAM
} StreamChoice;

#define MAIN_STREAM_PORT_SPACE 	100
#define MAX_QUEUE_SIZE          50    // 최대 큐 크기
#define EVENT_MERGE_WINDOW      3     // 이벤트 병합 시간 (초)

// 기존 함수들
void start_event_buf_process(int cam_idx);
int trigger_event_record(int cam_idx, char* http_str_path_out);
int get_udp_port(UDPClientProcess process, CameraDevice device, StreamChoice stream_choice, int stream_cnt);

// 전역 변수들
extern DeviceSetting g_setting;
extern WebRTCConfig g_config;

// 디버그 매크로
#ifdef DEBUG_EVENT_RECORDER
#define EVENT_DEBUG(fmt, ...) glog_trace("[EVENT_DEBUG] " fmt, ##__VA_ARGS__)
#else
#define EVENT_DEBUG(fmt, ...)
#endif

#endif // __EVENT_RECORDER_H__