#ifndef __NVDS_EVENT_H__
#define __NVDS_EVENT_H__

#include <gst/gst.h>
#include "global_define.h"

// 이벤트 타입
#define EVENT_EXIT                            9999
#define NOTIFICATION_TIME_GAP                 60

// 이벤트 관련 함수
gboolean send_event_to_recorder_simple(int class_id, int camera_id);
int send_notification_to_server(int class_id);
void gather_event(int class_id, int obj_id, int cam_idx);
void trigger_notification(int cam_idx);
void check_events_for_notification(int cam_idx, int init);
void check_heat_count(int cam_idx, int obj_id);

// 프로세스 관리
int is_process_running(const char *process_name);
int is_event_recording();

// 분석 제어
void set_tracker_analysis(gboolean OnOff);
void set_process_analysis(gboolean OnOff);
void setup_nv_analysis();
void endup_nv_analysis();

#endif /* __NVDS_EVENT_H__ */