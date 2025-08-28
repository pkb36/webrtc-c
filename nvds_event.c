#include "nvds_event.h"
#include "nvds_process.h"
#include "nvds_optical_flow.h"
#include "logging.h"
#include "device_setting.h"
#include "gstream_main.h"
#include "circular_buffer.h"
#include "curllib.h"
#include <string.h>
#include <unistd.h>
#include <signal.h>

// 외부 전역 변수 참조
extern ObjMonitor obj_info[NUM_CAMS][NUM_OBJS];
extern DeviceSetting g_setting;
extern int g_event_class_id;
extern int g_event_recording;
extern int g_preset_index;

// 이벤트 관련 전역 변수
static gboolean g_tracker_analysis_flag = FALSE;
static gboolean g_process_analysis_flag = FALSE;
static int g_analysis_state = 0;

void set_tracker_analysis(gboolean OnOff)
{
    g_tracker_analysis_flag = OnOff;
    
    if (OnOff) {
        glog_info("Tracker analysis ENABLED\n");
        g_analysis_state |= 0x01;
    } else {
        glog_info("Tracker analysis DISABLED\n");
        g_analysis_state &= ~0x01;
    }
}

void set_process_analysis(gboolean OnOff)
{
    g_process_analysis_flag = OnOff;
    
    if (OnOff) {
        glog_info("Process analysis ENABLED\n");
        g_analysis_state |= 0x02;
    } else {
        glog_info("Process analysis DISABLED\n");
        g_analysis_state &= ~0x02;
    }
}

int is_process_running(const char *process_name)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pgrep -x %s > /dev/null 2>&1", process_name);
    
    int result = system(cmd);
    return (result == 0) ? 1 : 0;
}

gboolean send_event_to_recorder_simple(int class_id, int camera_id)
{
    // 이벤트 버퍼에 저장
    if (g_setting.event_buffer_apply) {
        add_event_to_buffer(class_id, camera_id);
    }
    
    // 녹화 프로세스 시작
    if (!is_process_running("recorder")) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "./recorder --event %d --camera %d &", class_id, camera_id);
        system(cmd);
        
        glog_info("Started recorder for event: class=%d, camera=%d\n", class_id, camera_id);
        g_event_recording = 1;
        return TRUE;
    }
    
    return FALSE;
}

int send_notification_to_server(int class_id)
{
    char msg[512];
    snprintf(msg, sizeof(msg), 
             "{\"type\":\"event\",\"event_type\":\"detection\","
             "\"class_id\":%d,\"cam_idx\":%d,\"preset\":%d,"
             "\"timestamp\":\"%ld\"}",
             class_id, RGB_CAM, g_preset_index, time(NULL));
    
    send_msg_server(msg);
    
    glog_info("Event notification sent: class=%d\n", class_id);
    return 0;
}

void gather_event(int class_id, int obj_id, int cam_idx)
{
    static int last_event_time[NUM_CLASSES] = {0};
    int current_time = time(NULL);
    
    // 동일 이벤트는 60초 간격으로만 전송
    if (current_time - last_event_time[class_id] < NOTIFICATION_TIME_GAP) {
        return;
    }
    
    last_event_time[class_id] = current_time;
    
    // 이벤트 타입별 처리
    switch (class_id) {
        case CLASS_HEAT_COW:
            if (g_setting.heat_event_apply) {
                send_notification_to_server(class_id);
                send_event_to_recorder_simple(class_id, cam_idx);
            }
            break;
            
        case CLASS_FLIP_COW:
            if (g_setting.flip_event_apply) {
                // Optical Flow 결과 확인
                if (g_setting.opt_flow_apply) {
                    if (get_opt_flow_result(cam_idx, obj_id) > 0) {
                        send_notification_to_server(class_id);
                        send_event_to_recorder_simple(class_id, cam_idx);
                    }
                } else {
                    send_notification_to_server(class_id);
                    send_event_to_recorder_simple(class_id, cam_idx);
                }
            }
            break;
            
        case CLASS_LABOR_SIGN_COW:
            if (g_setting.labor_event_apply) {
                send_notification_to_server(class_id);
                send_event_to_recorder_simple(class_id, cam_idx);
            }
            break;
            
        default:
            break;
    }
}

void check_heat_count(int cam_idx, int obj_id)
{
    if (obj_info[cam_idx][obj_id].heat_count >= HEAT_COUNT_THRESHOLD) {
        obj_info[cam_idx][obj_id].notification_flag = 1;
        obj_info[cam_idx][obj_id].heat_count = 0;
        
        glog_trace("[HEAT] Object %d heat count threshold reached\n", obj_id);
    }
}

void check_events_for_notification(int cam_idx, int init)
{
    static int check_interval[NUM_CAMS] = {0};
    
    if (init) {
        memset(check_interval, 0, sizeof(check_interval));
        return;
    }
    
    check_interval[cam_idx]++;
    
    // 15초마다 체크 (15 FPS 기준으로 225 프레임)
    if (check_interval[cam_idx] < 225) {
        return;
    }
    
    check_interval[cam_idx] = 0;
    
    // 모든 객체에 대해 이벤트 체크
    for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
        if (obj_info[cam_idx][obj_id].notification_flag) {
            obj_info[cam_idx][obj_id].notification_flag = 0;
            
            int class_id = obj_info[cam_idx][obj_id].class_id;
            
            // Optical Flow 체크가 필요한 경우
            if (class_id == CLASS_FLIP_COW && g_setting.opt_flow_apply) {
                if (get_opt_flow_result(cam_idx, obj_id) == 0) {
                    init_opt_flow(cam_idx, obj_id, 1);
                    continue;
                }
            }
            
            // 이벤트 수집 및 전송
            gather_event(class_id, obj_id, cam_idx);
        }
    }
}

void trigger_notification(int cam_idx)
{
    check_events_for_notification(cam_idx, 0);
}

void setup_nv_analysis()
{
    glog_info("Setting up NV analysis...\n");
    
    // 분석 플래그 초기화
    g_tracker_analysis_flag = TRUE;
    g_process_analysis_flag = TRUE;
    g_analysis_state = 0x03;
    
    // 이벤트 버퍼 초기화
    if (g_setting.event_buffer_apply) {
        init_circular_buffer();
    }
    
    // 객체 정보 초기화
    for (int cam_idx = 0; cam_idx < NUM_CAMS; cam_idx++) {
        for (int obj_id = 0; obj_id < NUM_OBJS; obj_id++) {
            memset(&obj_info[cam_idx][obj_id], 0, sizeof(ObjMonitor));
        }
        check_events_for_notification(cam_idx, 1);
    }
    
    glog_info("NV analysis setup complete\n");
}

void endup_nv_analysis()
{
    glog_info("Ending NV analysis...\n");
    
    // 분석 플래그 해제
    g_tracker_analysis_flag = FALSE;
    g_process_analysis_flag = FALSE;
    g_analysis_state = 0x00;
    
    // 이벤트 버퍼 정리
    if (g_setting.event_buffer_apply) {
        cleanup_circular_buffer();
    }
    
    // 녹화 중지
    if (g_event_recording) {
        system("pkill -TERM recorder");
        g_event_recording = 0;
    }
    
    glog_info("NV analysis ended\n");
}

int is_event_recording()
{
    return g_event_recording;
}