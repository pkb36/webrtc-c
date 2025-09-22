/*
 * Global Variables Implementation
 * Provides safe getter functions for global state access
 */

#include "globals.h"
#include "unified_log.h"

/*
 * Global Variable Definitions
 * These are the actual variable definitions (not extern declarations)
 */

// Core configuration and state - defined in gstream_main.c
// WebRTCConfig g_config;
// DeviceSetting g_setting; 
// CurlIinfoType g_curlinfo;
// GstElement *g_pipeline;
// enum AppState g_app_state;

// Thread synchronization - defined in gstream_main.c
// pthread_mutex_t g_send_mutex;
// pthread_mutex_t g_process_msg_mutex;
// pthread_mutex_t g_send_info_mutex;
// pthread_mutex_t g_retry_connect_mutex;

/*
 * Safe Getter Functions
 * These provide controlled access to global variables with null checks
 */

WebRTCConfig* get_config(void) {
    // Note: g_config is defined in gstream_main.c as a static global
    // We need to make sure it's properly initialized
    return &g_config;
}

DeviceSetting* get_device_setting(void) {
    // Note: g_setting is defined in gstream_main.c as a static global
    return &g_setting;
}

CurlIinfoType* get_curl_info(void) {
    // Note: g_curlinfo is defined in gstream_main.c as a static global  
    return &g_curlinfo;
}

GstElement* get_pipeline(void) {
    if (!g_pipeline) {
        printf("Pipeline not initialized yet\n");
        return NULL;
    }
    return g_pipeline;
}

enum AppState get_app_state(void) {
    return g_app_state;
}

void set_app_state(enum AppState state) {
    enum AppState old_state = g_app_state;
    g_app_state = state;
    printf("App state changed from %d to %d\n", old_state, state);
}

int get_source_cam_idx(void) {
    return g_source_cam_idx;
}

void set_source_cam_idx(int idx) {
    if (idx < 0 || idx > 1) {  // Assuming 2 cameras max
        printf("Invalid camera index: %d, keeping current: %d\n", idx, g_source_cam_idx);
        return;
    }
    g_source_cam_idx = idx;
    printf("Camera index set to: %d\n", idx);
}

int get_move_speed(void) {
    return g_move_speed;
}

void set_move_speed(int speed) {
    if (speed < 1 || speed > 63) {  // PTZ speed range
        printf("Invalid move speed: %d, keeping current: %d\n", speed, g_move_speed);
        return;
    }
    g_move_speed = speed;
    printf("Move speed set to: %d\n", speed);
}

int get_preset_index(void) {
    return g_preset_index;
}

void set_preset_index(int index) {
    if (index < 0 || index >= MAX_PTZ_PRESET) {
        printf("Invalid preset index: %d, keeping current: %d\n", index, g_preset_index);
        return;
    }
    g_preset_index = index;
    printf("Preset index set to: %d\n", index);
}

gboolean is_event_recording_active(void) {
    return (g_event_recording != 0);
}

void set_event_recording(int recording) {
    g_event_recording = recording;
    printf("Event recording set to: %d\n", recording);
}

/*
 * Thread Safety Helpers
 */
pthread_mutex_t* get_send_mutex(void) {
    return &g_send_mutex;
}

pthread_mutex_t* get_send_info_mutex(void) {
    return &g_send_info_mutex;
}

pthread_mutex_t* get_process_msg_mutex(void) {
    return &g_process_msg_mutex;
}

pthread_mutex_t* get_retry_connect_mutex(void) {
    return &g_retry_connect_mutex;
}

pthread_mutex_t* get_motion_mutex(void) {
    return &g_motion_mutex;
}

/*
 * Notification Queue Access
 */
noti_queue* get_notification_queue(void) {
    return &notification_queue;
}

/*
 * Object Monitoring Access  
 */
ObjMonitor* get_obj_monitor(int cam_idx, int obj_idx) {
    if (cam_idx < 0 || cam_idx >= NUM_CAMS) {
        printf("Invalid camera index: %d\n", cam_idx);
        return NULL;
    }
    if (obj_idx < 0 || obj_idx >= NUM_OBJS) {
        printf("Invalid object index: %d\n", obj_idx);
        return NULL;
    }
    return &obj_info[cam_idx][obj_idx];
}

/*
 * Threshold Access
 */
float get_threshold_confidence(int class_id) {
    if (class_id < 0 || class_id >= NUM_CLASSES) {
        printf("Invalid class_id: %d\n", class_id);
        return 0.0f;
    }
    return threshold_confidence[class_id];
}

void set_threshold_confidence(int class_id, float confidence) {
    if (class_id < 0 || class_id >= NUM_CLASSES) {
        printf("Invalid class_id: %d\n", class_id);
        return;
    }
    if (confidence < 0.0f || confidence > 1.0f) {
        printf("Invalid confidence value: %f\n", confidence);
        return;
    }
    threshold_confidence[class_id] = confidence;
    printf("Threshold confidence[%d] set to: %f\n", class_id, confidence);
}

int get_threshold_duration(int class_id) {
    if (class_id < 0 || class_id >= NUM_CLASSES) {
        printf("Invalid class_id: %d\n", class_id);
        return 0;
    }
    return threshold_event_duration[class_id];
}

void set_threshold_duration(int class_id, int duration) {
    if (class_id < 0 || class_id >= NUM_CLASSES) {
        printf("Invalid class_id: %d\n", class_id);
        return;
    }
    if (duration < 0) {
        printf("Invalid duration: %d\n", duration);
        return;
    }
    threshold_event_duration[class_id] = duration;
    printf("Threshold duration[%d] set to: %d\n", class_id, duration);
}