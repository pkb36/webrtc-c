#ifndef __DEVICE_SETTING_H__
#define __DEVICE_SETTING_H__

#include "ptz_control.h"
#include "logging.h"

// Constants for device settings
#define AUTO_PTZ_SEQ_SIZE 32
#define DEFAULT_AUTO_PTZ_MOVE_SPEED 0x08
#define DEFAULT_PTZ_MOVE_SPEED 0x10
#define DEFAULT_EVENT_DURATION 15
#define DEFAULT_TEMP_DIFF_THRESHOLD 7
#define RESNET50_THRESHOLD_DEFAULT 6
#define MIN_SETTINGS_FILE_SIZE 10

// Camera day/night modes
typedef enum {
    CAMERA_MODE_AUTO = 0,
    CAMERA_MODE_MANUAL_DAY = 1,
    CAMERA_MODE_MANUAL_NIGHT = 2
} CameraDayNightMode;

typedef struct {
    // Display and recording settings
    int color_pallet;
    int record_status;
    int analysis_status;
    int show_normal_text;
    int display_temp;
    
    // PTZ camera control settings
    char auto_ptz_seq[AUTO_PTZ_SEQ_SIZE];
    char ptz_preset[MAX_PTZ_PRESET][PTZ_POS_SIZE];
    char auto_ptz_preset[MAX_PTZ_PRESET][PTZ_POS_SIZE];
    int auto_ptz_move_speed;
    int ptz_move_speed;
    int camera_dn_mode;  // CameraDayNightMode
    
    // Detection and analysis settings
    int camera_index;
    int nv_interval;
    int enable_event_notify;
    
    // Threshold settings for different cow behaviors
    int normal_threshold;
    int labor_sign_threshold;
    int normal_sitting_threshold;
    int heat_threshold;
    int flip_threshold;
    
    // Time duration settings for events
    int heat_time;
    int flip_time;
    int labor_sign_time;
    int over_temp_time;
    
    // Optical flow settings
    int opt_flow_threshold;
    int opt_flow_apply;
    
    // ResNet50 AI model settings
    int resnet50_threshold;
    int resnet50_apply;
    
    // Temperature measurement settings
    int temp_diff_threshold;
    int temp_apply;
    int temp_correction;
    int threshold_upper_temp;
    int threshold_under_temp;
    
    // Event processing settings
    int event_buffer_apply;
    int heat_event_apply;
    int flip_event_apply;
    int labor_event_apply;
} DeviceSetting;

// Core functions
gboolean load_device_setting(const char *file_name, DeviceSetting* setting);
gboolean update_setting(const char *file_name, DeviceSetting* setting);

// File validation and backup functions
gboolean validate_settings_file(const char *file_name);
void ensure_valid_settings_file(const char *file_name, DeviceSetting* default_setting);
void create_settings_backup(const char *file_name);

// Utility functions
void init_default_device_setting(DeviceSetting* setting);
void display_confidence_duration(void);

#if MINDULE_INCLUDE
typedef struct 
{
  char ranch_pos[MAX_RANCH_POS][PTZ_POS_SIZE];
} RanchSetting;

gboolean update_ranch_setting(const char *fname, RanchSetting* setting);
#endif

extern float threshold_confidence[];
extern int threshold_event_duration[];

// Remove duplicate definition - already defined above

#endif
