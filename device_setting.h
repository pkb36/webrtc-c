#ifndef __DEVICE_SETTING_H__
#define __DEVICE_SETTING_H__

#include "ptz_control.h"
#include "log_wrapper.h"

typedef struct 
{
  int color_pallet;
  int record_status;
  int analysis_status;

  //Format flag (On/Off), time(stay time), Pos count, [POS...]	
  char auto_ptz_seq[32];
  //Format flag (On/Off), PTZ Data [POS...]	
  char ptz_preset[MAX_PTZ_PRESET][PTZ_POS_SIZE];
  char auto_ptz_preset[MAX_PTZ_PRESET][PTZ_POS_SIZE];

  int auto_ptz_move_speed;
  int ptz_move_speed;

  int enable_event_notify;
  int camera_dn_mode; // 0 => auto, 1 => manual day, 2 => manual night mode

  int nv_interval;
  int opt_flow_threshold;
  int display_temp;
  int temp_diff_threshold;
  int temp_apply;

  int normal_threshold;
  int labor_sign_threshold;
  int normal_sitting_threshold;
  int heat_threshold;
  int flip_threshold;
  int camera_index;
  int resnet50_threshold;
  int resnet50_apply;
  int opt_flow_apply;
  int heat_time;
  int flip_time;
  int threshold_upper_temp;
  int threshold_under_temp;
  int labor_sign_time;
  int over_temp_time;
  int temp_correction;
  int show_normal_text;
} DeviceSetting;

gboolean load_device_setting(const char *file_name, DeviceSetting* setting);
gboolean update_setting(const char *file_name, DeviceSetting* setting);
gboolean validate_settings_file(const char *file_name);
void ensure_valid_settings_file(const char *file_name, DeviceSetting* default_setting);
void create_settings_backup(const char *file_name);

#if MINDULE_INCLUDE
typedef struct 
{
  char ranch_pos[MAX_RANCH_POS][PTZ_POS_SIZE];
} RanchSetting;

gboolean update_ranch_setting(const char *fname, RanchSetting* setting);
#endif

extern float threshold_confidence[];
extern int threshold_event_duration[];

#define RESNET50_THRESHOLD_DEFAULT        6

#endif
